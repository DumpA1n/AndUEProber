#include "SDKIdentifiers.hpp"
#include "SDKMathNames.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
struct Member
{
    std::string Type;
    std::string Name;
};

int failures = 0;

void check(bool condition, const char *label)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}
} // namespace

int main()
{
    check(SDKIdentifiers::Sanitize("1PCharacter") == "_1PCharacter",
          "identifiers beginning with digits are prefixed");
    check(SDKIdentifiers::Sanitize("NULL") == "NULL_", "system macro names are suffixed");
    check(SDKIdentifiers::Sanitize("value-name") == "value_name", "punctuation is sanitized");
    check(SDKIdentifiers::IsUsableType("struct TArray<struct UObject*>"),
          "nested reflected container type is accepted");
    check(!SDKIdentifiers::IsUsableType("struct *"), "anonymous elaborated pointer type is rejected");
    check(!SDKIdentifiers::IsUsableType("struct TArray<struct *>"),
          "invalid nested reflected type is rejected");
    check(!SDKIdentifiers::IsUsableType("struct TArray<>"), "empty template type is rejected");
    std::unordered_set<std::string> identifiers;
    check(SDKIdentifiers::MakeUnique("Disable", identifiers) == "Disable", "first identifier is preserved");
    check(SDKIdentifiers::MakeUnique("Disable", identifiers) == "Disable_2",
          "duplicate identifier receives a stable suffix");

    std::vector<Member> rotator{{"float", "pitch"}, {"float", "Yaw"}, {"float", "roll"}};
    check(SDKMathNames::Canonicalize("FRotator", rotator), "mixed-case rotator fields are recognized");
    check(rotator[0].Name == "Pitch" && rotator[1].Name == "Yaw" && rotator[2].Name == "Roll",
          "rotator fields use the identifiers required by generated helpers");

    std::vector<Member> vector{{"double", "x"}, {"double", "y"}, {"double", "z"}};
    check(SDKMathNames::Canonicalize("FVector", vector), "double vector fields are recognized");
    check(vector[0].Name == "X" && vector[1].Name == "Y" && vector[2].Name == "Z",
          "vector fields are canonicalized");

    std::vector<Member> incomplete{{"float", "pitch"}, {"float", "yaw"}};
    check(!SDKMathNames::Canonicalize("FRotator", incomplete), "incomplete math fields reject helpers");
    check(incomplete[0].Name == "pitch" && incomplete[1].Name == "yaw",
          "rejected fields are not partially rewritten");

    std::vector<Member> duplicate{{"float", "X"}, {"float", "x"}, {"float", "Y"}, {"float", "Z"}};
    check(!SDKMathNames::Canonicalize("FVector", duplicate), "ambiguous math fields reject helpers");

    std::vector<Member> integer{{"int32_t", "X"}, {"int32_t", "Y"}, {"int32_t", "Z"}};
    check(!SDKMathNames::Canonicalize("FVector", integer), "non-floating fields reject math helpers");

    check(!SDKMathNames::Canonicalize("FUnrelated", vector), "unrelated structs reject math helpers");

    if (!failures)
        std::puts("PASS: generated math helpers bind only to complete canonicalized scalar fields");
    return failures ? 1 : 0;
}
