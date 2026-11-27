// Host test for the FFXIV job-slot resolver (server/game/FFXIV/Jobs/FFXIVJobSlot.h): which job a
// class slot plus the equipped Soul of <Job> resolves to. No server, no engine, no game data.
//
// Compile (paths from the TrinityCore checkout root):
//   g++ -std=c++17 -Wall -Wextra -I src/server/game -I src/common
//       tests/job-slot/job_slot_test.cpp -o job_slot_test
//   ./job_slot_test           # prints "job_slot: PASS"
//
// The fixture rows are the ones datascripts/items/job-stones.ts writes into ffxiv_job_slot (jobId,
// classId, itemEntry); the item entries are stand-ins because the real entries are allocated by the
// data build. The class/job numbers are the sheets': Marauder is class 13 / ClassJob 3, Archer class
// 17 / ClassJob 5, Dark Knight ClassJob 32, Dancer ClassJob 38, Gunbreaker 37.

#include "FFXIV/Jobs/FFXIVJobSlot.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace FFXIV::Jobs;

namespace
{
    int s_failures = 0;

#define CHECK(expr)                                                                   \
    do                                                                                \
    {                                                                                 \
        if (!(expr))                                                                  \
        {                                                                             \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #expr ") failed\n"; \
            ++s_failures;                                                             \
        }                                                                             \
    } while (0)

    constexpr uint8 MARAUDER_CLASS = 13;   // ClassJob 3
    constexpr uint8 ARCHER_CLASS = 17;     // ClassJob 5
    constexpr uint8 WARRIOR_CLASS = 22;    // ClassJob 21
    constexpr uint8 DRK_JOB = 32;
    constexpr uint8 DNC_JOB = 38;
    constexpr uint8 WAR_JOB = 21;
    constexpr uint32 DRK_STONE = 9001;
    constexpr uint32 DNC_STONE = 9002;
    constexpr uint32 GNB_STONE = 9003;

    std::vector<FfxivJobSlotRow> Fixture()
    {
        return {
            { DRK_JOB, MARAUDER_CLASS, DRK_STONE },
            { 37,      MARAUDER_CLASS, GNB_STONE },
            { DNC_JOB, ARCHER_CLASS,   DNC_STONE },
        };
    }

    std::array<uint32, FFXIV_JOB_SLOT_EQUIPPED> Worn(uint32 first, uint32 second = 0)
    {
        return { first, second };
    }
}

int main()
{
    std::vector<FfxivJobSlotRow> const rows = Fixture();

    // Dark Knight: the Soul of the Dark Knight on Marauder is a job of its own.
    CHECK(ResolveFfxivJobSlotFrom(rows, MARAUDER_CLASS, 3, Worn(DRK_STONE)) == DRK_JOB);
    // ... and Marauder without it stays Marauder's own ClassJob row.
    CHECK(ResolveFfxivJobSlotFrom(rows, MARAUDER_CLASS, 3, Worn(0)) == 3);
    // Gunbreaker is a second job on the same slot: the stone picks, not the class.
    CHECK(ResolveFfxivJobSlotFrom(rows, MARAUDER_CLASS, 3, Worn(GNB_STONE)) == 37);
    // The other trinket slot is read too.
    CHECK(ResolveFfxivJobSlotFrom(rows, MARAUDER_CLASS, 3, Worn(0, DRK_STONE)) == DRK_JOB);

    // Dancer: Archer slot, Dancer job.
    CHECK(ResolveFfxivJobSlotFrom(rows, ARCHER_CLASS, 5, Worn(DNC_STONE)) == DNC_JOB);
    CHECK(ResolveFfxivJobSlotFrom(rows, ARCHER_CLASS, 5, Worn(0)) == 5);

    // A stone belonging to another slot changes nothing (the item's class mask would also stop it).
    CHECK(ResolveFfxivJobSlotFrom(rows, ARCHER_CLASS, 5, Worn(DRK_STONE)) == 5);
    // A class with no hosted job (Warrior has a slot of its own) keeps its ClassJob row.
    CHECK(ResolveFfxivJobSlotFrom(rows, WARRIOR_CLASS, WAR_JOB, Worn(DRK_STONE)) == WAR_JOB);
    // Two stones: the table's first matching row wins deterministically (the loader sorts by jobId).
    CHECK(ResolveFfxivJobSlotFrom(rows, MARAUDER_CLASS, 3, Worn(GNB_STONE, DRK_STONE)) == DRK_JOB);

    if (s_failures != 0)
    {
        std::cerr << "job_slot: " << s_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "job_slot: PASS\n";
    return 0;
}
