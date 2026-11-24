/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * The FFXIV resource-cost gate, pure: the rows are the ones tools/generate-ffxiv-job-gauges.py writes
 * from the shipped sheets (Inner Beast 49 / Fell Cleave 3549 -50, Inner Release 1177 waives Fell
 * Cleave and Decimate). The status-gated replacement (Nascent Chaos -> Inner Chaos) is not a core
 * rule: the client swaps the action and livescripts/spell-scripts/Warrior/warrior.ts gates it.
 */

#include "tc_catch2.h"

#include "FFXIV/Gauge/FFXIVJobGauge.h"

using namespace FFXIV::Gauge;

namespace
{
constexpr uint8 WAR = 21;
constexpr uint8 MRD = 3;
constexpr uint8 BEAST = 22;

constexpr uint32 INNER_BEAST = 49;
constexpr uint32 STEEL_CYCLONE = 51;
constexpr uint32 INFURIATE = 52;
constexpr uint32 FELL_CLEAVE = 3549;
constexpr uint32 DECIMATE = 3550;
constexpr uint32 MAIM = 37;
constexpr uint32 INNER_CHAOS = 16465;
constexpr uint32 CHAOTIC_CYCLONE = 16463;
constexpr uint32 INNER_RELEASE = 1177;

ObjectGuid const Player1 = ObjectGuid::Create<HighGuid::Player>(1);

std::vector<FfxivJobGaugeRow> Gauges()
{
    return { { WAR, BEAST, "Beast Gauge", 100 } };
}

// The rows the generator writes for Warrior (the level-54+ kit included: the old table stopped at
// Inner Beast/Steel Cyclone because those were the only rows that carried Action col 32).
std::vector<FfxivActionGaugeRow> Deltas()
{
    return {
        { MAIM, WAR, BEAST, 35, 10, true },
        { INNER_BEAST, WAR, BEAST, 35, -50, false },
        { STEEL_CYCLONE, WAR, BEAST, 45, -50, false },
        { INFURIATE, WAR, BEAST, 50, 50, false },
        { FELL_CLEAVE, WAR, BEAST, 54, -50, false },
        { DECIMATE, WAR, BEAST, 60, -50, false },
        { CHAOTIC_CYCLONE, WAR, BEAST, 72, -50, false },
        { INNER_CHAOS, WAR, BEAST, 80, -50, false },
    };
}

std::vector<FfxivActionCostWaiverRow> Waivers()
{
    return { { INNER_RELEASE, FELL_CLEAVE }, { INNER_RELEASE, DECIMATE } };
}

struct CostFixture
{
    CostFixture()
    {
        SetFfxivJobGaugeTables(Gauges(), Deltas());
        SetFfxivActionCostWaiverRows(Waivers());
        ClearFfxivGaugeState();
    }

    ~CostFixture()
    {
        ClearFfxivJobGaugeTables();
        ClearFfxivGaugeState();
    }
};
} // namespace

TEST_CASE("FFXIV cost: a spender's cost is its negative plain row", "[ffxiv]")
{
    ClearFfxivJobGaugeTables();
    ClearFfxivGaugeState();
    CHECK(GetFfxivActionGaugeCost(FELL_CLEAVE, WAR, 54) == 0); // no table: nothing costs anything
    CHECK(CanAffordFfxivActionGauge(Player1, FELL_CLEAVE, WAR, 54));

    CostFixture fixture;
    CHECK(GetFfxivActionGaugeCost(INNER_BEAST, WAR, 50) == 50);
    CHECK(GetFfxivActionGaugeCost(FELL_CLEAVE, WAR, 60) == 50);
    CHECK(GetFfxivActionGaugeCost(INNER_CHAOS, WAR, 80) == 50);
    // A gain is not a cost, and a comboOnly row never is one.
    CHECK(GetFfxivActionGaugeCost(INFURIATE, WAR, 50) == 0);
    CHECK(GetFfxivActionGaugeCost(MAIM, WAR, 50) == 0);
    // minLevel: Fell Cleave has no row below 54.
    CHECK(GetFfxivActionGaugeCost(FELL_CLEAVE, WAR, 53) == 0);
    // Marauder has no gauge, so nothing costs anything there either.
    CHECK(GetFfxivActionGaugeCost(INNER_BEAST, MRD, 50) == 0);
}

TEST_CASE_METHOD(CostFixture, "FFXIV cost: the cast gate, at 0 and at the cost", "[ffxiv]")
{
    CHECK_FALSE(CanAffordFfxivActionGauge(Player1, INNER_BEAST, WAR, 50)); // 0 < 50: refused
    CHECK(AddFfxivGauge(Player1, WAR, 49) == 49);
    CHECK_FALSE(CanAffordFfxivActionGauge(Player1, INNER_BEAST, WAR, 50));
    CHECK(AddFfxivGauge(Player1, WAR, 1) == 50);
    CHECK(CanAffordFfxivActionGauge(Player1, INNER_BEAST, WAR, 50));       // exactly the cost
    CHECK(CanAffordFfxivActionGauge(Player1, INFURIATE, WAR, 50));         // free actions always pass
    CHECK(CanAffordFfxivActionGauge(Player1, DECIMATE, WAR, 60));          // same 50 cost

    CHECK(SpendFfxivGauge(Player1, WAR, 50));
    CHECK(GetFfxivGauge(Player1, WAR) == 0);
    CHECK_FALSE(CanAffordFfxivActionGauge(Player1, DECIMATE, WAR, 60));
    // The waive is a parameter of the pure check (the Player overload reads the status auras).
    CHECK(CanAffordFfxivActionGauge(Player1, DECIMATE, WAR, 60, true));
}

TEST_CASE_METHOD(CostFixture, "FFXIV cost: a waived cost neither refuses nor spends", "[ffxiv]")
{
    CHECK(AddFfxivGauge(Player1, WAR, 100) == 100);

    // Inner Release: Fell Cleave costs 0, so the row's -50 is not applied at all.
    CHECK_FALSE(ApplyFfxivActionGauge(Player1, WAR, 60, FELL_CLEAVE, false, true));
    CHECK(GetFfxivGauge(Player1, WAR) == 100);

    // Without the waiver the same cast spends.
    CHECK(ApplyFfxivActionGauge(Player1, WAR, 60, FELL_CLEAVE, false, false));
    CHECK(GetFfxivGauge(Player1, WAR) == 50);

    // The gain part of a row is not affected by the waiver parameter.
    CHECK(ApplyFfxivActionGauge(Player1, WAR, 50, INFURIATE, false, true));
    CHECK(GetFfxivGauge(Player1, WAR) == 100);
}

TEST_CASE_METHOD(CostFixture, "FFXIV cost: the waiver table names the statuses", "[ffxiv]")
{
    CHECK(GetFfxivActionCostWaiverStatuses(FELL_CLEAVE) == std::vector<uint32>{ INNER_RELEASE });
    CHECK(GetFfxivActionCostWaiverStatuses(DECIMATE) == std::vector<uint32>{ INNER_RELEASE });
    CHECK(GetFfxivActionCostWaiverStatuses(INNER_BEAST).empty());

    // A row with a zero id is dropped, and clearing the tables drops the waivers too.
    SetFfxivActionCostWaiverRows({ { 0, FELL_CLEAVE }, { INNER_RELEASE, 0 } });
    CHECK(GetFfxivActionCostWaiverStatuses(FELL_CLEAVE).empty());
    ClearFfxivJobGaugeTables();
    CHECK(GetFfxivActionCostWaiverStatuses(INNER_BEAST).empty());
}
