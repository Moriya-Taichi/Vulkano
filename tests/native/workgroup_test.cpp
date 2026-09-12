#include "engine.hpp"
#include "spirv-reflect/include/spirv/unified1/spirv.h"
#include <iostream>
using namespace vulkano;
namespace {
int checks = 0;
void expect(bool value) { ++checks; if (!value) throw std::runtime_error("Workgroup expression mismatch"); }
template<class F> void rejects(F f) { bool failed = false; try { f(); } catch(const std::exception &) { failed = true; } expect(failed); }
void emit(Shader &s, uint32_t op, std::initializer_list<uint32_t> args) {
    s.code.push_back((uint32_t(args.size() + 1) << 16) | op);
    s.code.insert(s.code.end(), args);
}
Shader base() {
    Shader s; s.code = {SpvMagicNumber, 0x10300, 0, 1024, 0};
    emit(s, SpvOpTypeInt, {1,32,0}); emit(s, SpvOpTypeInt, {2,32,1});
    emit(s, SpvOpTypeBool, {3}); emit(s, SpvOpTypeInt, {4,64,1});
    emit(s, SpvOpTypeVector, {5,1,3});
    emit(s, SpvOpConstant, {1,20,1});
    emit(s, SpvOpExecutionModeId, {1000,SpvExecutionModeLocalSizeId,100,20,20});
    return s;
}
uint32_t binary(uint32_t op, uint32_t a, uint32_t b, bool sign = false) {
    auto s=base(); const uint32_t t=sign?2:1;
    emit(s,SpvOpConstant,{t,21,a}); emit(s,SpvOpConstant,{t,22,b});
    emit(s,SpvOpSpecConstantOp,{t,100,op,21,22});
    return reflectWorkgroupSize(s,1000)[0];
}
}
int main() try {
    expect(binary(SpvOpIAdd,12,30)==42);
    expect(binary(SpvOpISub,3,5)==0xfffffffeu);
    expect(binary(SpvOpIMul,0x80000001u,3)==0x80000003u);
    expect(binary(SpvOpUDiv,43,2)==21);
    expect(binary(SpvOpUMod,43,2)==1);
    expect(binary(SpvOpSDiv,uint32_t(-43),2,true)==uint32_t(-21));
    expect(binary(SpvOpSRem,uint32_t(-43),2,true)==uint32_t(-1));
    expect(binary(SpvOpSMod,uint32_t(-43),2,true)==1);
    expect(binary(SpvOpSMod,43,uint32_t(-2),true)==uint32_t(-1));
    expect(binary(SpvOpBitwiseAnd,15,9)==9);
    expect(binary(SpvOpBitwiseOr,8,3)==11);
    expect(binary(SpvOpBitwiseXor,15,9)==6);
    expect(binary(SpvOpShiftLeftLogical,3,4)==48);
    expect(binary(SpvOpShiftRightLogical,0x80000000u,31)==1);
    expect(binary(SpvOpShiftRightArithmetic,0x80000000u,31,true)==UINT32_MAX);
    rejects([] { binary(SpvOpUDiv,1,0); });
    rejects([] { binary(SpvOpSRem,1,0,true); });
    rejects([] { binary(SpvOpSDiv,0x80000000u,UINT32_MAX,true); });
    rejects([] { binary(SpvOpShiftLeftLogical,1,32); });
    rejects([] { binary(SpvOpIAdd,UINT32_MAX,1); });
    for(auto op : {SpvOpUGreaterThan,SpvOpUGreaterThanEqual,SpvOpULessThan,SpvOpULessThanEqual,
                   SpvOpIEqual,SpvOpINotEqual,SpvOpSGreaterThan,SpvOpSLessThan}) {
        auto s=base(); emit(s,SpvOpConstant,{1,21,7}); emit(s,SpvOpConstant,{1,22,3});
        emit(s,SpvOpSpecConstantOp,{3,23,uint32_t(op),21,22});
        emit(s,SpvOpSpecConstantOp,{1,100,SpvOpSelect,23,21,22});
        const bool yes=op==SpvOpUGreaterThan || op==SpvOpUGreaterThanEqual || op==SpvOpINotEqual || op==SpvOpSGreaterThan;
        expect(reflectWorkgroupSize(s,1000)[0]==(yes?7u:3u));
    }
    {
        auto s=base(); emit(s,SpvOpDecorate,{21,SpvDecorationSpecId,7});
        emit(s,SpvOpSpecConstant,{1,21,4}); emit(s,SpvOpSpecConstantOp,{1,100,SpvOpIMul,21,21});
        expect(reflectWorkgroupSize(s,1000)[0]==16);
        s.constants[7]={9,4}; expect(reflectWorkgroupSize(s,1000)[0]==81);
        s.constants[7]={9,8}; rejects([&] { reflectWorkgroupSize(s,1000); });
    }
    {
        auto s=base(); emit(s,SpvOpConstant,{4,21,0xfffffff9u,UINT32_MAX});
        emit(s,SpvOpSpecConstantOp,{1,100,SpvOpSConvert,21});
        expect(reflectWorkgroupSize(s,1000)[0]==uint32_t(-7));
    }
    {
        auto s=base(); emit(s,SpvOpConstant,{1,21,7}); emit(s,SpvOpConstant,{1,22,3});
        emit(s,SpvOpSpecConstantComposite,{5,23,21,22,20});
        emit(s,SpvOpSpecConstantOp,{5,24,SpvOpVectorShuffle,23,23,1,0,2});
        emit(s,SpvOpSpecConstantOp,{1,100,SpvOpCompositeExtract,24,0});
        expect(reflectWorkgroupSize(s,1000)[0]==3);
        emit(s,SpvOpDecorate,{24,SpvDecorationBuiltIn,SpvBuiltInWorkgroupSize});
        expect(reflectWorkgroupSize(s,1000)==std::array<uint32_t,3>{3,7,1});
    }
    {
        auto s=base(); emit(s,SpvOpSpecConstantOp,{1,100,SpvOpIAdd,100,20});
        rejects([&] { reflectWorkgroupSize(s,1000); });
        s.code.push_back(0); rejects([&] { reflectWorkgroupSize(s,1000); });
    }
    std::cout << checks << " CPU workgroup checks passed\n";
} catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
