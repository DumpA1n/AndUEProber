// Demonstration: include the dumped AIOHeader.hpp and use `->` to access
// member variables on dumped UE types.
//
// The header is per-game and gitignored (each user generates their own
// dump). __has_include keeps the project buildable on a fresh clone —
// when no dump is present this TU is empty; when a dump lands at the
// expected path the demo function activates automatically.

#if __has_include("../../misc/com.tencent.tmgp.dfm/AIOHeader.hpp")
#include "../../misc/com.tencent.tmgp.dfm/AIOHeader.hpp"

#include <cstdio>
#include <cstring>

// Back the typed view with a raw byte buffer so this links without a
// live game process at compile time.
extern "C" void demo_aio_pointer_access()
{
    alignas(8) uint8_t mem[0x100] = {0};

    // FVector is in CoreUObject — three floats at 0/4/8.
    FVector* v = reinterpret_cast<FVector*>(mem);
    v->X = 1.0f;
    v->Y = 2.0f;
    v->Z = 3.0f;

    // Read the same bytes through the typed view to confirm offsets line up.
    float xyz[3];
    std::memcpy(xyz, mem, sizeof(xyz));
    std::printf("v->X=%.1f v->Y=%.1f v->Z=%.1f | raw [0]=%.1f [4]=%.1f [8]=%.1f\n",
                v->X, v->Y, v->Z, xyz[0], xyz[1], xyz[2]);
}

#endif // __has_include
