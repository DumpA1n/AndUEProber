#include "FunctionLayout.hpp"
#include "OwnedFunctions.hpp"
#include <array>
#include <cstring>
#include <cstdio>

using Generated = andueprober_sdk::OwnedFunctionLayout;
static_assert(sizeof(Generated) == sizeof(OwnedNativeFunction));
static_assert(alignof(Generated) == alignof(OwnedNativeFunction));
static_assert(offsetof(Generated, flags) == offsetof(OwnedNativeFunction, flags));
static_assert(offsetof(Generated, numParms) == offsetof(OwnedNativeFunction, numParms));
static_assert(offsetof(Generated, parmsSize) == offsetof(OwnedNativeFunction, parmsSize));
static_assert(offsetof(Generated, returnOffset) == offsetof(OwnedNativeFunction, returnOffset));
static_assert(offsetof(Generated, nativeFunction) == offsetof(OwnedNativeFunction, nativeFunction));
int main() {
    Generated value{};
    value.flags = 0x80000410; value.numParms = 3; value.parmsSize = 24;
    value.returnOffset = 0xffff; value.nativeFunction = 0;
    OwnedNativeFunction native;
    static_assert(std::is_trivially_copyable_v<Generated> && std::is_trivially_copyable_v<OwnedNativeFunction>);
    std::memcpy(&native, &value, sizeof(native));
    if (native.flags != value.flags || native.numParms != value.numParms || native.parmsSize != value.parmsSize ||
        native.returnOffset != value.returnOffset || native.nativeFunction != 0 || ownedNativeFunctionCalls.load() != 0) return 1;
    std::puts("PASS: generated frozen function layout compiles and matches the independently compiled owned native record; no function invocation");
}
