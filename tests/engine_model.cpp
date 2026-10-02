#include "andueprober/EngineModel.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
using namespace andueprober;
namespace {
// UStruct::IsChildOf through the SuperStruct walk.
bool isChildOfWalk(const std::map<std::uintptr_t, std::uintptr_t>& super, std::uintptr_t child, std::uintptr_t parent) {
    for (auto at = child; at; at = super.at(at)) if (at == parent) return true;
    return false;
}
std::vector<std::uintptr_t> ancestry(const std::map<std::uintptr_t, std::uintptr_t>& super, std::uintptr_t child) {
    std::vector<std::uintptr_t> result;
    for (auto at = child; at; at = super.at(at)) result.push_back(at);
    return result;
}
}
int main() {
    // Delta Force 1.201.37117.67 registers three UClass objects whose FNames share the
    // "DFMAbilityInstance" entry and differ only in Number.
    REQUIRE(fnameToString("DFMAbilityInstance", 0) == "DFMAbilityInstance");
    REQUIRE(fnameToString("DFMAbilityInstance", 1) == "DFMAbilityInstance_0");
    REQUIRE(fnameToString("DFMAbilityInstance", 2) == "DFMAbilityInstance_1");
    const std::set<std::string> numbered{fnameToString("DFMAbilityInstance", 0),
        fnameToString("DFMAbilityInstance", 1), fnameToString("DFMAbilityInstance", 2)};
    REQUIRE(numbered.size() == 3);
    REQUIRE(fnameToString("Actor", 0x7fffffffu) == "Actor_2147483646");

    {
        const auto classes = intrinsicCastClasses();
        std::map<std::string_view, std::uint64_t> flags;
        for (const auto& item : classes) REQUIRE(flags.emplace(item.name, item.classCastFlags).second);
        REQUIRE(flags.at("Object") == 0);
        const std::map<std::string_view, std::string_view> super{{"Field", "Object"}, {"Enum", "Field"},
            {"Struct", "Field"}, {"ScriptStruct", "Struct"}, {"Class", "Struct"}, {"Function", "Struct"}};
        REQUIRE(super.size() + 1 == flags.size());
        for (const auto& [name, parent] : super) {
            REQUIRE((flags.at(name) & flags.at(parent)) == flags.at(parent));
            REQUIRE(flags.at(name) != flags.at(parent));
        }
        REQUIRE(flags.at("Class") == 0x29 && flags.at("Function") == 0x80009);
    }

    {
        // int32 Add_IntInt(int32 A, int32 B): CPF_Parm | CPF_ZeroConstructor, and the return
        // value additionally carries CPF_OutParm | CPF_ReturnParm.
        const std::vector<FunctionParameter> addIntInt{{0x280, 0, 1, 4}, {0x280, 4, 1, 4}, {0x780, 8, 1, 4}};
        const auto derived = deriveFunctionParameters(0, addIntInt);
        REQUIRE(derived && *derived == (FunctionParameterSummary{3, 12, 8}));

        REQUIRE(deriveFunctionParameters(0, {}) == FunctionParameterSummary{});
        const std::vector<FunctionParameter> noReturn{{0x280, 0, 1, 8}, {0x280, 8, 3, 4}};
        REQUIRE(deriveFunctionParameters(0, noReturn) == (FunctionParameterSummary{2, 20, 0xffff}));

        // The walk stops at the first local; a later parameter-flagged property is not counted.
        const std::vector<FunctionParameter> local{{0x280, 0, 1, 4}, {0x200, 8, 1, 4}, {0x780, 12, 1, 4}};
        REQUIRE(deriveFunctionParameters(0, local) == (FunctionParameterSummary{1, 4, 0xffff}));
        // FUNC_HasDefaults admits zero-constructed locals and stops at the first that is not.
        REQUIRE(deriveFunctionParameters(0x80, local) == (FunctionParameterSummary{2, 16, 12}));
        const std::vector<FunctionParameter> constructedLocal{{0x280, 0, 1, 4}, {0x0, 8, 1, 16}, {0x780, 24, 1, 4}};
        REQUIRE(deriveFunctionParameters(0x80, constructedLocal) == (FunctionParameterSummary{1, 4, 0xffff}));

        REQUIRE(!deriveFunctionParameters(0, std::vector<FunctionParameter>{{0x280, 0xfffe, 1, 4}}));
        REQUIRE(!deriveFunctionParameters(0, std::vector<FunctionParameter>{{0x280, -4, 1, 4}}));
        REQUIRE(!deriveFunctionParameters(0, std::vector<FunctionParameter>(256, FunctionParameter{0x280, 0, 1, 0})));
    }

    {
        // TMap<int32, FString>: TPair is {int32, pad, FString} = 24 bytes aligned to 8, and
        // TSetElement appends HashNextId and HashIndex.
        REQUIRE(scriptMapLayout(4, 4, 16, 8) == (ScriptMapLayout{8, {24, 28, 32, {8, 32}}}));
        // TMap<uint8, uint8>: the int32 hash members raise the element alignment to 4.
        REQUIRE(scriptMapLayout(1, 1, 1, 1) == (ScriptMapLayout{1, {4, 8, 12, {4, 12}}}));
        // TMap<FName, UObject*> with an 8-byte, 4-aligned FName.
        REQUIRE(scriptMapLayout(8, 4, 8, 8) == (ScriptMapLayout{8, {16, 20, 24, {8, 24}}}));
        REQUIRE(scriptMapLayout(4, 4, 4, 4) == (ScriptMapLayout{4, {8, 12, 16, {4, 16}}}));
        REQUIRE(scriptSetLayout(8, 8) == (ScriptSetLayout{8, 12, 16, {8, 16}}));
        REQUIRE(scriptSetLayout(1, 1) == (ScriptSetLayout{4, 8, 12, {4, 12}}));
        // TSparseArray elements are never smaller than the free-list link.
        REQUIRE(scriptSetLayout(0, 1) == (ScriptSetLayout{0, 4, 8, {4, 8}}));
        REQUIRE(!scriptMapLayout(4, 3, 4, 4) && !scriptMapLayout(4, 4, 4, 0) && !scriptMapLayout(-1, 4, 4, 4));
        REQUIRE(!scriptSetLayout(0x7ffffffc, 8));
    }

    {
        REQUIRE(classifyBoolProperty(1, 1, 0, 1, 0xff) == BoolPropertyKind::Native);
        REQUIRE(classifyBoolProperty(1, 1, 0, 0x04, 0x04) == BoolPropertyKind::Bitfield);
        REQUIRE(classifyBoolProperty(4, 4, 2, 0x10, 0x10) == BoolPropertyKind::Bitfield);
        // A bitfield whose bit is 0 shares ByteMask 1 with a native bool but keeps FieldMask 1.
        REQUIRE(classifyBoolProperty(1, 1, 0, 1, 1) == BoolPropertyKind::Bitfield);
        REQUIRE(classifyBoolProperty(4, 1, 0, 1, 0xff) == BoolPropertyKind::Invalid);
        REQUIRE(classifyBoolProperty(1, 1, 1, 0x04, 0x04) == BoolPropertyKind::Invalid);
        REQUIRE(classifyBoolProperty(1, 1, 0, 0x06, 0x06) == BoolPropertyKind::Invalid);
        REQUIRE(classifyBoolProperty(1, 1, 0, 0x04, 0xff) == BoolPropertyKind::Invalid);
        REQUIRE(classifyBoolProperty(3, 3, 0, 1, 0xff) == BoolPropertyKind::Invalid);
        // A native quartet read one byte early, as when a target stores its own byte before
        // FieldSize, no longer satisfies ElementSize == FieldSize.
        REQUIRE(classifyBoolProperty(1, 0, 1, 0, 1) == BoolPropertyKind::Invalid);
    }

    {
        constexpr std::uintptr_t chain = 0x30;
        const std::map<std::uintptr_t, std::uintptr_t> super{{0x1000, 0}, {0x2000, 0x1000}, {0x3000, 0x2000},
            {0x4000, 0x2000}, {0x5000, 0x4000}};
        REQUIRE(structBaseChainArray(ancestry(super, 0x5000), chain) ==
            (std::vector<std::uintptr_t>{0x1030, 0x2030, 0x4030, 0x5030}));
        // FStructBaseChain::IsChildOfUsingStructArray on the constructed arrays agrees with
        // the SuperStruct walk for every ordered pair.
        for (const auto& [child, unused] : super) {
            (void)unused;
            const auto bases = structBaseChainArray(ancestry(super, child), chain);
            for (const auto& [parent, alsoUnused] : super) {
                (void)alsoUnused;
                const auto parentDepthMinusOne = ancestry(super, parent).size() - 1;
                const bool indexed = parentDepthMinusOne <= bases.size() - 1 && bases[parentDepthMinusOne] == parent + chain;
                REQUIRE(indexed == isChildOfWalk(super, child, parent));
            }
        }
    }
    std::puts("engine model checks passed");
}
