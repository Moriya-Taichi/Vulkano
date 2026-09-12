#include "engine.hpp"
#include "spirv-reflect/include/spirv/unified1/spirv.h"
#include <cstring>
#include <set>

namespace vulkano {
namespace {
struct Value {
    uint32_t type = 0;
    uint64_t bits = 0;
    std::vector<Value> elements;
};
struct Type {
    uint32_t width = 0, element = 0;
    bool signedness = false, boolean = false;
};
struct Node {
    uint32_t type = 0, op = 0;
    std::vector<uint32_t> args;
};
uint64_t mask(uint32_t width) { return width == 64 ? UINT64_MAX : (uint64_t(1) << width) - 1; }
int64_t signedValue(uint64_t bits, uint32_t width) {
    if (width < 64 && (bits & (uint64_t(1) << (width - 1)))) bits |= ~mask(width);
    int64_t result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}
struct Evaluator {
    const Shader &shader;
    std::map<uint32_t, Type> types;
    std::map<uint32_t, Node> nodes;
    std::map<uint32_t, uint32_t> ids;
    std::map<uint32_t, Value> values;
    std::set<uint32_t> active;
    uint32_t workgroup = 0;
    explicit Evaluator(const Shader &s) : shader(s) {
        const auto &code = s.code;
        require(code.size() >= 5 && code[0] == SpvMagicNumber, "Invalid workgroup module");
        for (size_t i = 5; i < code.size();) {
            const auto count = code[i] >> 16, op = code[i] & 0xffff;
            require(count && count <= code.size() - i, "Malformed workgroup instruction");
            if (op == SpvOpTypeBool && count == 2) types[code[i + 1]] = {32, 0, false, true};
            if (op == SpvOpTypeInt && count == 4) {
                const auto width = code[i + 2];
                require(width == 8 || width == 16 || width == 32 || width == 64, "Invalid integer width");
                types[code[i + 1]] = {width, 0, code[i + 3] != 0};
            }
            if (op == SpvOpTypeVector && count == 4) types[code[i + 1]] = {0, code[i + 2], false};
            if (op == SpvOpDecorate && count == 4) {
                if (code[i + 2] == SpvDecorationSpecId) ids[code[i + 1]] = code[i + 3];
                if (code[i + 2] == SpvDecorationBuiltIn && code[i + 3] == SpvBuiltInWorkgroupSize)
                    workgroup = code[i + 1];
            }
            if ((op >= SpvOpConstantTrue && op <= SpvOpConstantNull) ||
                (op >= SpvOpSpecConstantTrue && op <= SpvOpSpecConstantOp)) {
                require(count >= 3, "Malformed workgroup constant");
                nodes[code[i + 2]] = {code[i + 1], op, {code.begin() + i + 3, code.begin() + i + count}};
            }
            i += count;
        }
        for (size_t i = 5; i < code.size(); i += code[i] >> 16)
            if ((code[i] & 0xffff) == SpvOpGroupDecorate && (code[i] >> 16) >= 3) {
                const auto found = ids.find(code[i + 1]);
                if (found != ids.end())
                    for (size_t n = 2; n < code[i] >> 16; ++n) ids[code[i + n]] = found->second;
            }
    }
    Value arithmetic(uint32_t op, uint32_t resultType, const std::vector<Value> &args) {
        require(types.count(resultType), "Workgroup expressions must have integer or boolean types");
        const auto type = types.at(resultType);
        if (type.element) {
            require(!args.empty(), "Missing vector operands");
            const size_t count = args.back().elements.size();
            require(count > 0 && count <= 4, "Invalid workgroup vector");
            Value result{resultType, 0, {}};
            for (size_t i = 0; i < count; ++i) {
                std::vector<Value> lane;
                for (const auto &a : args) {
                    require(a.elements.empty() || a.elements.size() == count, "Incompatible constant vectors");
                    lane.push_back(a.elements.empty() ? a : a.elements[i]);
                }
                result.elements.push_back(arithmetic(op, type.element, lane));
            }
            return result;
        }
        require(!args.empty() && types.count(args[0].type), "Missing scalar operands");
        for (const auto &a : args) require(a.elements.empty(), "Expected scalar constant");
        const auto a = args[0].bits, b = args.size() > 1 ? args[1].bits : 0;
        const auto width = types.at(args[0].type).width;
        require(width && type.width, "Invalid scalar constant type");
        const auto sa = signedValue(a, width), sb = signedValue(b, width);
        const bool unary = op == SpvOpSNegate || op == SpvOpNot || op == SpvOpLogicalNot ||
                           op == SpvOpUConvert || op == SpvOpSConvert || op == SpvOpBitcast;
        require(args.size() == (unary ? 1u : op == SpvOpSelect ? 3u : 2u), "Incorrect constant operand count");
        uint64_t result = 0;
        switch (op) {
        case SpvOpIAdd: result = a + b; break;
        case SpvOpISub: result = a - b; break;
        case SpvOpIMul: result = a * b; break;
        case SpvOpUDiv: require(b != 0, "Constant division by zero"); result = a / b; break;
        case SpvOpUMod: require(b != 0, "Constant modulo by zero"); result = a % b; break;
        case SpvOpSDiv:
        case SpvOpSRem:
        case SpvOpSMod: {
            require(sb != 0 && !(sa == signedValue(uint64_t(1) << (width - 1), width) && sb == -1), "Undefined signed constant division");
            if (op == SpvOpSDiv) result = uint64_t(sa / sb);
            else {
                auto remainder = sa % sb;
                if (op == SpvOpSMod && remainder && ((remainder < 0) != (sb < 0))) remainder += sb;
                result = uint64_t(remainder);
            }
            break;
        }
        case SpvOpShiftLeftLogical: require(b < width, "Constant shift exceeds width"); result = a << b; break;
        case SpvOpShiftRightLogical: require(b < width, "Constant shift exceeds width"); result = a >> b; break;
        case SpvOpShiftRightArithmetic:
            require(b < width, "Constant shift exceeds width");
            result = a >> b;
            if (b && sa < 0) result |= mask(width) ^ (mask(width) >> b);
            break;
        case SpvOpBitwiseOr: result = a | b; break;
        case SpvOpBitwiseXor: result = a ^ b; break;
        case SpvOpBitwiseAnd: result = a & b; break;
        case SpvOpNot: result = ~a; break;
        case SpvOpSNegate: result = uint64_t(0) - a; break;
        case SpvOpUConvert: result = a; break;
        case SpvOpSConvert: result = uint64_t(sa); break;
        case SpvOpBitcast:
            require(width == type.width, "Bitcast must preserve width"); result = a; break;
        case SpvOpLogicalEqual: case SpvOpIEqual: result = a == b; break;
        case SpvOpLogicalNotEqual: case SpvOpINotEqual: result = a != b; break;
        case SpvOpLogicalAnd: result = a && b; break;
        case SpvOpLogicalOr: result = a || b; break;
        case SpvOpLogicalNot: result = !a; break;
        case SpvOpUGreaterThan: result = a > b; break;
        case SpvOpUGreaterThanEqual: result = a >= b; break;
        case SpvOpULessThan: result = a < b; break;
        case SpvOpULessThanEqual: result = a <= b; break;
        case SpvOpSGreaterThan: result = sa > sb; break;
        case SpvOpSGreaterThanEqual: result = sa >= sb; break;
        case SpvOpSLessThan: result = sa < sb; break;
        case SpvOpSLessThanEqual: result = sa <= sb; break;
        case SpvOpSelect: result = a ? b : args[2].bits; break;
        default: throw std::invalid_argument("Unsupported operation in integer workgroup expression");
        }
        return {resultType, result & mask(type.width), {}};
    }
    Value evaluate(uint32_t id) {
        if (values.count(id)) return values.at(id);
        require(nodes.count(id) && active.size() < 256 && active.insert(id).second,
                "Invalid or cyclic workgroup expression");
        const auto &n = nodes.at(id);
        Value result{n.type, 0, {}};
        if (n.op == SpvOpConstant || n.op == SpvOpSpecConstant || n.op == SpvOpConstantTrue ||
            n.op == SpvOpConstantFalse || n.op == SpvOpSpecConstantTrue || n.op == SpvOpSpecConstantFalse ||
            n.op == SpvOpConstantNull) {
            require(types.count(n.type) && types.at(n.type).width, "Expected integer/boolean scalar");
            const auto width = types.at(n.type).width;
            if (n.op == SpvOpConstant || n.op == SpvOpSpecConstant) {
                require(n.args.size() == (width == 64 ? 2u : 1u), "Invalid constant word count");
                result.bits = n.args[0];
                if (width == 64) result.bits |= uint64_t(n.args[1]) << 32;
            } else result.bits = n.op == SpvOpConstantTrue || n.op == SpvOpSpecConstantTrue;
            if (n.op == SpvOpSpecConstant || n.op == SpvOpSpecConstantTrue || n.op == SpvOpSpecConstantFalse) {
                const auto spec = ids.find(id);
                if (spec != ids.end()) {
                    const auto value = shader.constants.find(spec->second);
                    if (value != shader.constants.end()) {
                        require(value->second.bytes == width / 8, "Workgroup constant width mismatch");
                        result.bits = value->second.bits;
                    }
                }
            }
            result.bits &= mask(width);
            if (types.at(n.type).boolean) result.bits = result.bits != 0;
        } else if (n.op == SpvOpConstantComposite || n.op == SpvOpSpecConstantComposite) {
            for (auto component : n.args) result.elements.push_back(evaluate(component));
        } else if (n.op == SpvOpSpecConstantOp) {
            require(n.args.size() >= 2, "Missing specialization expression operands");
            const auto op = n.args[0];
            if (op == SpvOpCompositeExtract) {
                result = evaluate(n.args[1]);
                for (size_t i = 2; i < n.args.size(); ++i) {
                    require(n.args[i] < result.elements.size(), "Constant component out of bounds");
                    Value next = result.elements[n.args[i]];
                    result = std::move(next);
                }
                require(result.type == n.type, "Constant component type mismatch");
            } else if (op == SpvOpCompositeInsert) {
                require(n.args.size() >= 4, "Missing composite insertion indices");
                const auto object = evaluate(n.args[1]);
                result = evaluate(n.args[2]);
                Value *target = &result;
                for (size_t i = 3; i < n.args.size(); ++i) {
                    require(n.args[i] < target->elements.size(), "Constant insertion out of bounds");
                    target = &target->elements[n.args[i]];
                }
                require(target->type == object.type && result.type == n.type, "Constant insertion type mismatch");
                *target = object;
            } else if (op == SpvOpVectorShuffle) {
                require(n.args.size() >= 4, "Missing shuffle operands");
                auto a = evaluate(n.args[1]), b = evaluate(n.args[2]);
                a.elements.insert(a.elements.end(), b.elements.begin(), b.elements.end());
                for (size_t i = 3; i < n.args.size(); ++i) {
                    require(n.args[i] < a.elements.size(), "Undefined shuffle component in workgroup size");
                    result.elements.push_back(a.elements[n.args[i]]);
                }
            } else {
                std::vector<Value> operands;
                for (size_t i = 1; i < n.args.size(); ++i) operands.push_back(evaluate(n.args[i]));
                result = arithmetic(op, n.type, operands);
            }
        } else throw std::invalid_argument("Workgroup size must be a constant expression");
        active.erase(id);
        values[id] = result;
        return result;
    }
    uint32_t dimension(uint32_t id) {
        const auto value = evaluate(id);
        require(value.elements.empty() && types.count(value.type) && types.at(value.type).width == 32 &&
                    value.bits > 0 && value.bits <= UINT32_MAX,
                "Workgroup dimensions must be nonzero 32-bit integers");
        return uint32_t(value.bits);
    }
};
} // namespace
std::array<uint32_t, 3> reflectWorkgroupSize(const Shader &shader, uint32_t entryId) {
    Evaluator evaluator(shader);
    std::array<uint32_t, 3> result{};
    const auto &code = shader.code;
    for (size_t i = 5; i < code.size(); i += code[i] >> 16) {
        const auto op = code[i] & 0xffff, count = code[i] >> 16;
        if (count == 6 && code[i + 1] == entryId) {
            if (op == SpvOpExecutionMode && code[i + 2] == SpvExecutionModeLocalSize)
                result = {code[i + 3], code[i + 4], code[i + 5]};
            if (op == SpvOpExecutionModeId && code[i + 2] == SpvExecutionModeLocalSizeId)
                for (size_t n = 0; n < 3; ++n) result[n] = evaluator.dimension(code[i + 3 + n]);
        }
    }
    // WorkgroupSize takes precedence over LocalSize/LocalSizeId in SPIR-V.
    if (evaluator.workgroup) {
        const auto value = evaluator.evaluate(evaluator.workgroup);
        require(value.elements.size() == 3, "WorkgroupSize must have three components");
        for (size_t n = 0; n < 3; ++n) {
            require(value.elements[n].bits && value.elements[n].bits <= UINT32_MAX &&
                        evaluator.types.at(value.elements[n].type).width == 32,
                    "Invalid WorkgroupSize component");
            result[n] = uint32_t(value.elements[n].bits);
        }
    }
    return result;
}
} // namespace vulkano
