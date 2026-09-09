#include "engine.hpp"
#include "extensions.hpp"
#include "spirv-reflect/include/spirv/unified1/spirv.h"
#include <algorithm>
namespace vulkano {
void validateCooperativeShader(Device &d, const Shader &shader, const std::array<uint32_t, 3> &local) {
    require(d.enabled & CooperativeMatrix, "Cooperative matrix feature was not enabled");
    const uint64_t threads = uint64_t(local[0]) * local[1] * local[2];
    require(d.subgroup.subgroupSize && threads % d.subgroup.subgroupSize == 0,
            "Cooperative workgroup must contain whole subgroups");
    const auto &code = shader.code;
    std::map<uint32_t, uint32_t> specIds, constants, scalarTypes;
    for (size_t at = 5; at < code.size(); at += code[at] >> 16) {
        auto op = code[at] & 0xffff, words = code[at] >> 16;
        if (op == SpvOpDecorate && words == 4 && code[at + 2] == SpvDecorationSpecId)
            specIds[code[at + 1]] = code[at + 3];
        if (op == SpvOpTypeFloat && words == 3) {
            const auto bits = code[at + 2];
            if (bits == 16 || bits == 32 || bits == 64)
                scalarTypes[code[at + 1]] = bits == 16 ? 0 : bits == 32 ? 1 : 2;
        }
        if (op == SpvOpTypeInt && words == 4) {
            const auto bits = code[at + 2];
            if (bits == 8 || bits == 16 || bits == 32 || bits == 64)
                scalarTypes[code[at + 1]] = (code[at + 3] ? 3 : 7) + (bits == 8    ? 0
                                                                      : bits == 16 ? 1
                                                                      : bits == 32 ? 2
                                                                                   : 3);
        }
    }
    for (size_t at = 5; at < code.size(); at += code[at] >> 16) {
        auto op = code[at] & 0xffff, words = code[at] >> 16;
        if ((op == SpvOpConstant || op == SpvOpSpecConstant) && words == 4) {
            auto value = code[at + 3];
            const auto spec = specIds.find(code[at + 2]);
            if (spec != specIds.end()) {
                auto supplied = shader.constants.find(spec->second);
                if (supplied != shader.constants.end())
                    value = supplied->second.uint32();
            }
            constants[code[at + 2]] = value;
        }
    }
    struct Matrix {
        uint32_t component, scope, rows, cols, use;
    };
    std::map<uint32_t, Matrix> matrices;
    auto value = [&](uint32_t id) {
        auto it = constants.find(id);
        require(it != constants.end(), "Matrix dimensions/use require scalar specialization constants");
        return it->second;
    };
    auto compatibleType = [](uint32_t a, uint32_t b) {
        return a == b || (a >= 3 && a <= 10 && b >= 3 && b <= 10 && (a - 3) % 4 == (b - 3) % 4);
    };
    for (size_t at = 5; at < code.size(); at += code[at] >> 16) {
        if ((code[at] & 0xffff) != SpvOpTypeCooperativeMatrixKHR)
            continue;
        require((code[at] >> 16) == 7 && scalarTypes.count(code[at + 2]), "Invalid cooperative matrix component type");
        Matrix m{scalarTypes.at(code[at + 2]), value(code[at + 3]), value(code[at + 4]), value(code[at + 5]),
                 value(code[at + 6])};
        require(m.scope == VK_SCOPE_SUBGROUP_KHR && m.use <= 2 && m.rows && m.cols,
                "Unsupported matrix scope/use/dimensions");
        bool supported = std::any_of(
            d.extensions->matrixConfigurations.begin(), d.extensions->matrixConfigurations.end(), [&](const auto &p) {
                if (p.scope != m.scope)
                    return false;
                if (m.use == 0)
                    return p.MSize == m.rows && p.KSize == m.cols && compatibleType(m.component, p.AType);
                if (m.use == 1)
                    return p.KSize == m.rows && p.NSize == m.cols && compatibleType(m.component, p.BType);
                return p.MSize == m.rows && p.NSize == m.cols &&
                       (compatibleType(m.component, p.CType) || compatibleType(m.component, p.ResultType));
            });
        require(supported, "Cooperative matrix type/shape is unsupported by this device");
        matrices.emplace(code[at + 1], m);
    }
    std::map<uint32_t, uint32_t> valueTypes;
    for (size_t at = 5; at < code.size(); at += code[at] >> 16)
        if ((code[at] & 0xffff) != SpvOpTypeCooperativeMatrixKHR && (code[at] >> 16) >= 3 &&
            matrices.count(code[at + 1]))
            valueTypes[code[at + 2]] = code[at + 1];
    auto matrixValue = [&](uint32_t id) -> const Matrix & {
        auto it = valueTypes.find(id);
        require(it != valueTypes.end(), "Cannot resolve cooperative matrix operand");
        return matrices.at(it->second);
    };
    auto operandType = [](const Matrix &m, bool signedInteger) {
        require(m.component >= 3 || !signedInteger, "Float matrix has an integer signedness operand");
        return m.component < 3 ? m.component : (signedInteger ? 3u : 7u) + (m.component - 3) % 4;
    };
    for (size_t at = 5; at < code.size(); at += code[at] >> 16) {
        if ((code[at] & 0xffff) != SpvOpCooperativeMatrixMulAddKHR)
            continue;
        const auto words = code[at] >> 16;
        require((words == 6 || words == 7) && matrices.count(code[at + 1]), "Invalid cooperative multiply-add");
        const auto &a = matrixValue(code[at + 3]), &b = matrixValue(code[at + 4]), &c = matrixValue(code[at + 5]),
                   &result = matrices.at(code[at + 1]);
        const uint32_t operands = words == 7 ? code[at + 6] : 0;
        require(!(operands & ~31u) && a.use == 0 && b.use == 1 && c.use == 2 && result.use == 2 && a.cols == b.rows &&
                    a.rows == c.rows && b.cols == c.cols && c.rows == result.rows && c.cols == result.cols,
                "Invalid matrix multiply-add shape/use");
        bool supported = std::any_of(
            d.extensions->matrixConfigurations.begin(), d.extensions->matrixConfigurations.end(), [&](const auto &p) {
                return p.scope == VK_SCOPE_SUBGROUP_KHR && p.MSize == a.rows && p.NSize == b.cols &&
                       p.KSize == a.cols && p.AType == operandType(a, operands & 1) &&
                       p.BType == operandType(b, operands & 2) && p.CType == operandType(c, operands & 4) &&
                       p.ResultType == operandType(result, operands & 8) &&
                       bool(p.saturatingAccumulation) == bool(operands & 16);
            });
        require(supported, "Cooperative multiply-add type/shape/saturation combination is unsupported");
    }
}
} // namespace vulkano
