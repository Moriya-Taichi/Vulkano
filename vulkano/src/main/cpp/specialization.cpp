#include "engine.hpp"
#include "spirv-reflect/include/spirv/unified1/spirv.h"
#include <cstring>
#include <map>

namespace vulkano {
SpecializationData::SpecializationData(const Shader &shader) {
    if (shader.constants.empty())
        return;
    const auto &code = shader.code;
    require(code.size() >= 5 && code[0] == SpvMagicNumber, "Invalid specialization shader");
    std::map<uint32_t, uint32_t> sizes, types, ids;
    for (size_t i = 5; i < code.size();) {
        const auto words = code[i] >> 16, op = code[i] & 0xffff;
        require(words && words <= code.size() - i, "Malformed specialization instruction");
        if (op == SpvOpTypeBool && words == 2)
            sizes[code[i + 1]] = 4;
        if ((op == SpvOpTypeInt && words == 4) || (op == SpvOpTypeFloat && words == 3)) {
            require(code[i + 2] == 8 || code[i + 2] == 16 || code[i + 2] == 32 || code[i + 2] == 64,
                    "Unsupported scalar bit width");
            sizes[code[i + 1]] = code[i + 2] / 8;
        }
        if ((op == SpvOpSpecConstant || op == SpvOpSpecConstantTrue || op == SpvOpSpecConstantFalse) && words >= 3)
            types[code[i + 2]] = code[i + 1];
        if (op == SpvOpDecorate && words == 4 && code[i + 2] == SpvDecorationSpecId)
            ids[code[i + 1]] = code[i + 3];
        i += words;
    }
    for (size_t i = 5; i < code.size(); i += code[i] >> 16)
        if ((code[i] & 0xffff) == SpvOpGroupDecorate && (code[i] >> 16) >= 3) {
            const auto group = ids.find(code[i + 1]);
            if (group != ids.end())
                for (size_t n = 2; n < code[i] >> 16; ++n)
                    ids[code[i + n]] = group->second;
        }
    std::map<uint32_t, uint32_t> declared;
    for (const auto &[result, type] : types)
        if (const auto id = ids.find(result); id != ids.end()) {
            const auto size = sizes.find(type);
            require(size != sizes.end(), "Function constant must be a scalar");
            const auto found = declared.emplace(id->second, size->second);
            require(found.second || found.first->second == size->second, "Constant ID has conflicting scalar widths");
        }
    for (const auto &[id, value] : shader.constants) {
        const auto found = declared.find(id);
        require(found != declared.end(), "Unknown specialization constant ID");
        require(value.bytes == found->second, "Function constant byte size differs from the SPIR-V scalar type");
        require(value.bytes == 8 || value.bits < (uint64_t(1) << (value.bytes * 8)), "Function constant bit overflow");
        require(data.size() <= UINT32_MAX - value.bytes, "Function constant data exceeds Vulkan offset range");
        const auto offset = data.size();
        entries.push_back({id, uint32_t(offset), value.bytes});
        data.resize(offset + value.bytes);
        // Specialization values use native byte order, including the narrow scalar types.
        switch (value.bytes) {
        case 1: {
            const uint8_t v = uint8_t(value.bits);
            std::memcpy(data.data() + offset, &v, 1);
            break;
        }
        case 2: {
            const uint16_t v = uint16_t(value.bits);
            std::memcpy(data.data() + offset, &v, 2);
            break;
        }
        case 4: {
            const uint32_t v = uint32_t(value.bits);
            std::memcpy(data.data() + offset, &v, 4);
            break;
        }
        case 8:
            std::memcpy(data.data() + offset, &value.bits, 8);
            break;
        }
    }
}
} // namespace vulkano
