// Host-only checks for soul crystal mappings, using the same predicate as the world loader.
#include "FFXIV/Jobs/FFXIVJobSlot.h"

#include <cassert>
#include <iostream>

int main()
{
    using FFXIV::Jobs::IsFfxivJobStoneMappingValid;
    constexpr uint64 darkKnightMask = uint64(1) << 30;
    constexpr uint64 machinistMask = uint64(1) << 32;
    constexpr uint64 viperMask = uint64(1) << 40;
    constexpr uint64 rogueMask = uint64(1) << 15;

    assert(IsFfxivJobStoneMappingValid(32, 32, darkKnightMask, darkKnightMask));
    assert(IsFfxivJobStoneMappingValid(31, 31, machinistMask, machinistMask));
    assert(IsFfxivJobStoneMappingValid(41, 41, viperMask, viperMask));
    assert(!IsFfxivJobStoneMappingValid(41, 29, rogueMask, rogueMask));
    assert(!IsFfxivJobStoneMappingValid(41, 41, viperMask, rogueMask | viperMask));
    assert(!IsFfxivJobStoneMappingValid(41, 41, viperMask, 0));
    assert(!IsFfxivJobStoneMappingValid(0, 0, 0, 0));
    std::cout << "job_slot: PASS\n";
}
