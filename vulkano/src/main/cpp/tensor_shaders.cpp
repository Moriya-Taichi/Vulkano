#include "tensors.hpp"
#define SPV_ENABLE_UTILITY_CODE
#include "spirv-reflect/include/spirv/unified1/spirv.h"
#include <optional>
#include <unordered_map>

namespace vulkano {
void reflectTensorBinding(Device &d, const Shader &shader, uint32_t variable, BindingLayout &binding) {
    require((d.enabledExtra & TensorResources) && d.extensions->tensor.shaderTensorAccess &&
                (binding.stages & d.extensions->tensorProperties.shaderTensorSupportedStages) == binding.stages,
            "Tensor shader access is unavailable for this stage");
    const auto &code = shader.code;
    std::unordered_map<uint32_t, const uint32_t *> nodes;
    std::unordered_map<uint32_t, uint32_t> specialization;
    for (size_t i = 5; i < code.size(); i += code[i] >> 16) {
        const auto words = code[i] >> 16;
        auto op = SpvOp(code[i] & 0xffff);
        bool result, type;
        SpvHasResultAndType(op, &result, &type);
        if (result && words > (type ? 2u : 1u))
            nodes[code[i + (type ? 2 : 1)]] = code.data() + i;
        if (op == SpvOpDecorate && words == 4 && code[i + 2] == SpvDecorationSpecId)
            specialization[code[i + 1]] = code[i + 3];
    }
    auto node = [&](uint32_t id) {
        const auto it = nodes.find(id);
        require(it != nodes.end(), "Tensor SPIR-V references an unknown ID");
        return it->second;
    };
    // Resolve literal and directly specialized dimensions. More complex SPIR-V
    // constant expressions remain subject to the driver's shader validation.
    auto scalar = [&](uint32_t id) -> std::optional<uint64_t> {
        auto n = node(id);
        if ((n[0] & 0xffff) != SpvOpConstant && (n[0] & 0xffff) != SpvOpSpecConstant)
            return {};
        if (auto spec = specialization.find(id); spec != specialization.end())
            if (auto value = shader.constants.find(spec->second); value != shader.constants.end())
                return value->second;
        require((n[0] >> 16) >= 4, "Malformed tensor scalar constant");
        auto t = node(n[1]);
        if ((t[0] & 0xffff) != SpvOpTypeInt)
            return {};
        uint64_t value = n[3];
        if (t[2] == 64) {
            require((n[0] >> 16) >= 5, "Malformed 64-bit tensor constant");
            value |= uint64_t(n[4]) << 32;
        }
        return value;
    };
    auto declaration = node(variable);
    require((declaration[0] & 0xffff) == SpvOpVariable, "Tensor descriptor must be a variable");
    auto type = node(declaration[1]);
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto op = SpvOp(type[0] & 0xffff);
        if (op == SpvOpTypePointer)
            type = node(type[3]);
        else if (op == SpvOpTypeArray || op == SpvOpTypeRuntimeArray)
            type = node(type[2]);
        else
            break;
    }
    require((type[0] & 0xffff) == SpvOpTypeTensorARM, "Invalid tensor descriptor type");
    auto element = node(type[2]);
    switch (element[0] & 0xffff) {
    case SpvOpTypeBool:
        binding.storageFormat = VK_FORMAT_R8_BOOL_ARM;
        break;
    case SpvOpTypeFloat:
        switch (element[2]) {
        case 16:
            binding.storageFormat = VK_FORMAT_R16_SFLOAT;
            break;
        case 32:
            binding.storageFormat = VK_FORMAT_R32_SFLOAT;
            break;
        case 64:
            binding.storageFormat = VK_FORMAT_R64_SFLOAT;
            break;
        default:
            throw std::invalid_argument("Unsupported tensor float width");
        }
        break;
    case SpvOpTypeInt:
        switch (element[2]) {
        case 8:
            binding.storageFormat = element[3] ? VK_FORMAT_R8_SINT : VK_FORMAT_R8_UINT;
            break;
        case 16:
            binding.storageFormat = element[3] ? VK_FORMAT_R16_SINT : VK_FORMAT_R16_UINT;
            break;
        case 32:
            binding.storageFormat = element[3] ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
            break;
        case 64:
            binding.storageFormat = element[3] ? VK_FORMAT_R64_SINT : VK_FORMAT_R64_UINT;
            break;
        default:
            throw std::invalid_argument("Unsupported tensor integer width");
        }
        break;
    default:
        throw std::invalid_argument("Tensor element must be scalar");
    }
    if ((type[0] >> 16) >= 4) {
        if (auto rank = scalar(type[3])) {
            require(*rank > 0 && *rank <= d.extensions->tensorProperties.maxTensorDimensionCount,
                    "Shader tensor rank exceeds limits");
            binding.tensorRank = uint32_t(*rank);
        }
    }
    if ((type[0] >> 16) >= 5 && binding.tensorRank) {
        auto shape = node(type[4]);
        if ((shape[0] & 0xffff) == SpvOpConstantComposite || (shape[0] & 0xffff) == SpvOpSpecConstantComposite) {
            require((shape[0] >> 16) == binding.tensorRank + 3, "Shader tensor shape/rank mismatch");
            for (uint32_t i = 0; i < binding.tensorRank; ++i) {
                auto dimension = scalar(shape[3 + i]);
                if (!dimension) {
                    binding.tensorDimensions.clear();
                    break;
                }
                require(*dimension && *dimension <= d.extensions->tensorProperties.maxPerDimensionTensorElements,
                        "Shader tensor dimension exceeds limits");
                binding.tensorDimensions.push_back(int64_t(*dimension));
            }
        }
    }
    const auto &limits = d.extensions->tensorProperties;
    for (size_t i = 5; i < code.size(); i += code[i] >> 16) {
        auto op = SpvOp(code[i] & 0xffff);
        if (op != SpvOpTensorReadARM && op != SpvOpTensorWriteARM)
            continue;
        require((code[i] >> 16) >= (op == SpvOpTensorReadARM ? 5u : 4u), "Malformed tensor access instruction");
        auto accessType = op == SpvOpTensorReadARM ? node(code[i + 1]) : node(node(code[i + 3])[1]);
        uint64_t count = 1;
        if ((accessType[0] & 0xffff) == SpvOpTypeArray) {
            auto size = scalar(accessType[3]);
            if (!size)
                continue;
            count = *size;
            require(count && count <= limits.maxTensorShaderAccessArrayLength,
                    "Tensor shader access array exceeds limits");
            accessType = node(accessType[2]);
        }
        uint32_t bytes = (accessType[0] & 0xffff) == SpvOpTypeBool ? 1 : accessType[2] / 8;
        require(bytes && count <= limits.maxTensorShaderAccessSize / bytes, "Tensor shader access size exceeds limits");
    }
}
} // namespace vulkano
