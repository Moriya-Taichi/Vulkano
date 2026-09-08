#include "graphs.hpp"
#define SPV_ENABLE_UTILITY_CODE
#include "spirv-reflect/include/spirv/unified1/spirv.h"
#include <cstring>
#include <set>

namespace vulkano {
namespace {
std::pair<std::string, uint32_t> literalString(const uint32_t *words, uint32_t count) {
    const auto *text = reinterpret_cast<const char *>(words);
    const auto *end = static_cast<const char *>(std::memchr(text, 0, size_t(count) * 4));
    require(end, "Unterminated SPIR-V string");
    return {std::string(text, end), uint32_t((end - text) / 4 + 1)};
}
} // namespace
GraphInterface reflectGraph(Device &d, const Shader &shader) {
    const auto &code = shader.code;
    require(code.size() >= 5 && code[0] == SpvMagicNumber && code[3] && code[4] == 0, "Invalid graph SPIR-V header");
    std::unordered_map<uint32_t, const uint32_t *> nodes;
    std::map<uint32_t, uint32_t> indices, sets, specs;
    std::vector<uint32_t> interfaces;
    std::vector<std::pair<uint32_t, uint32_t>> constants;
    GraphInterface result;
    bool foundEntry = false, capability = false;
    for (size_t i = 5; i < code.size();) {
        const uint32_t count = code[i] >> 16;
        require(count && count <= code.size() - i, "Malformed graph SPIR-V instruction");
        auto op = SpvOp(code[i] & 0xffff);
        const auto *n = code.data() + i;
        bool hasResult, hasType;
        SpvHasResultAndType(op, &hasResult, &hasType);
        if (hasResult) {
            const uint32_t idIndex = hasType ? 2 : 1;
            require(count > idIndex && n[idIndex] && n[idIndex] < code[3], "Invalid graph SPIR-V result ID");
            require(nodes.emplace(n[idIndex], n).second, "Duplicate graph SPIR-V result ID");
        }
        if (op == SpvOpCapability) {
            require(count == 2, "Invalid graph capability");
            capability |= n[1] == SpvCapabilityGraphARM;
        }
        if (op == SpvOpDecorate && count == 4) {
            if (n[2] == SpvDecorationBinding)
                indices[n[1]] = n[3];
            if (n[2] == SpvDecorationDescriptorSet)
                sets[n[1]] = n[3];
            if (n[2] == SpvDecorationSpecId)
                specs[n[1]] = n[3];
        }
        if (op >= SpvOpSpecConstantTrue && op <= SpvOpSpecConstantOp)
            result.specialization = true;
        if (op == SpvOpExtInstImport) {
            require(count >= 3, "Invalid graph instruction set import");
            const auto name = literalString(n + 2, count - 2).first;
            if (name.rfind("NonSemantic.", 0) != 0)
                result.operationSets.push_back(name);
        }
        if (op == SpvOpGraphEntryPointARM) {
            require(count >= 3, "Invalid graph entry point");
            const auto [name, length] = literalString(n + 2, count - 2);
            if (name == shader.entry) {
                require(!foundEntry, "Duplicate graph entry point name");
                foundEntry = true;
                interfaces.assign(n + 2 + length, n + count);
            }
        }
        if (op == SpvOpGraphConstantARM) {
            require(count == 4, "Invalid graph constant");
            constants.emplace_back(n[3], n[2]);
        }
        i += count;
    }
    require(capability && foundEntry, "SPIR-V has no matching graph entry point");
    auto node = [&](uint32_t id, uint32_t minimum) {
        auto it = nodes.find(id);
        require(it != nodes.end() && (it->second[0] >> 16) >= minimum, "Invalid graph SPIR-V reference");
        return it->second;
    };
    std::set<uint32_t> used;
    for (auto id : interfaces) {
        require(used.insert(id).second && indices.count(id) && sets.count(id) && sets[id] == 0,
                "Graph interfaces need unique variables in descriptor set zero");
        auto v = node(id, 4);
        require((v[0] & 0xffff) == SpvOpVariable && v[3] == SpvStorageClassUniformConstant,
                "Graph interface must use UniformConstant storage");
        BindingLayout binding{indices[id], VK_DESCRIPTOR_TYPE_TENSOR_ARM};
        auto pointer = node(v[1], 4);
        require((pointer[0] & 0xffff) == SpvOpTypePointer, "Graph interface pointer required");
        auto type = node(pointer[3], 3);
        if ((type[0] & 0xffff) == SpvOpTypeArray) {
            require((type[0] >> 16) == 4, "Malformed graph tensor array");
            auto length = node(type[3], 4);
            require((length[0] & 0xffff) == SpvOpConstant || (length[0] & 0xffff) == SpvOpSpecConstant,
                    "Graph array length must be a literal or direct function constant");
            binding.count = length[3];
            if (specs.count(type[3]) && shader.constants.count(specs[type[3]]))
                binding.count = shader.constants.at(specs[type[3]]);
            require(binding.count, "Graph tensor array must not be empty");
        } else
            require((type[0] & 0xffff) == SpvOpTypeTensorARM,
                    "Graph interfaces require tensors or fixed tensor arrays");
        reflectTensorBinding(d, shader, id, binding, true);
        for (const auto &previous : result.bindings)
            require(previous.binding != binding.binding, "Graph interfaces alias the same descriptor binding");
        result.bindings.push_back(std::move(binding));
    }
    for (const auto &[constant, id] : constants) {
        BindingLayout binding{};
        reflectTensorBinding(d, shader, id, binding, true);
        require(result.constants.emplace(constant, std::move(binding)).second, "Duplicate graph constant ID");
    }
    return result;
}
} // namespace vulkano
