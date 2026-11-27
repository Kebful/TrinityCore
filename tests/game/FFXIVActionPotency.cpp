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
 * FFXIV action potency (data-only slice, work/stat-research/POTENCY-EXTRACTOR-20261123.md).
 *
 * Every expectation is either
 *   - read from the module table the server table is generated from
 *     (release/modules/ffxivhaven/datascripts/classes/spells/shared/ffxiv-action-potency.generated.ts),
 *     including the full cross-check vector below, or
 *   - the ActionTransient description of the named action, quoted in the comment as the tree the
 *     sheet writes (the starter actions' rows are decoded by the generator and checked there against
 *     datascripts/classes/spells/ffxiv-starter-data.ts).
 *
 * The vector is the module table's plain `potency` / `curePotency` / `kind` for every one of its 664
 * action ids (the 651 Action rows the module ships as spells plus the nine hand-authored starters),
 * with 0xFFFF for a slot the sheet states conditionally or not at all. It is transcribed by the same
 * generator (--cpp-test-out), so this test compares the compiled server resolver against the
 * module's published table rather than against itself.
 *
 * gnum68 = ClassJob and gnum72 = level is INFERRED from the values and the gates (report §3.1); the
 * numbers are the sheet's, and no coefficient, multiplier or point conversion appears here.
 */

#include "tc_catch2.h"

#include "FFXIVActionPotency.h"

#include <cstddef>
#include <iterator>

using namespace FFXIV::Combat;

namespace
{
constexpr uint16 NoValue = 0xFFFF; // the encoding FFXIVActionPotency.inc and the vector share

struct ExpectedPotency
{
    uint32 actionId;
    uint16 damage;
    uint16 cure;
    uint8 kind;
};

// { actionId, damage, cure, kind }, straight from the module table.
ExpectedPotency const EXPECTED[] =
{
#include "FFXIVActionPotencyCrossCheck.inc"
};

FfxivActionPotency Potency(uint32 actionId, uint8 job, uint8 level)
{
    return GetFfxivActionPotency(actionId, job, level);
}

// The row counts of the module table, as published in POTENCY-EXTRACTOR-20261123.md §4.
constexpr std::size_t EXPECTED_ACTIONS = 664;
} // namespace

TEST_CASE("FFXIV action potency: table coverage", "[ffxiv]")
{
    REQUIRE(std::size(EXPECTED) == EXPECTED_ACTIONS);

    for (ExpectedPotency const& expected : EXPECTED)
        CHECK(HasFfxivActionPotency(expected.actionId));

    // Ids outside the shipped set have no row. 0 is not an Action id and 70000 is past the sheet.
    CHECK_FALSE(HasFfxivActionPotency(0));
    CHECK_FALSE(HasFfxivActionPotency(70000));
    CHECK_FALSE(HasFfxivActionPotency(4));

    // An unknown id resolves to "nothing", not to a default potency.
    FfxivActionPotency unknown = Potency(70000, 24, 90);
    CHECK(unknown.damage == 0);
    CHECK(unknown.cure == 0);
    CHECK(unknown.kind == FFXIV_POTENCY_KIND_NONE);
}

TEST_CASE("FFXIV action potency: module table cross-check", "[ffxiv]")
{
    // All 664 rows: every plain damage and cure value is identical to the module table's, and so is
    // the kind of every row. A conditional slot has no plain value and is resolved by the branch
    // tests below instead.
    std::size_t damage = 0, heal = 0, conditional = 0, none = 0;
    for (ExpectedPotency const& expected : EXPECTED)
    {
        FfxivActionPotency actual = Potency(expected.actionId, 0, 0);

        INFO("action " << expected.actionId);
        CHECK(static_cast<uint32>(actual.kind) == expected.kind);

        if (expected.damage != NoValue)
        {
            CHECK(actual.damage == expected.damage);
            CHECK(actual.kind == FFXIV_POTENCY_KIND_DAMAGE);
        }
        if (expected.cure != NoValue)
            CHECK(actual.cure == expected.cure);

        // A row that states nothing resolves to nothing at any job and level.
        if (expected.damage == NoValue && expected.cure == NoValue &&
            expected.kind == FFXIV_POTENCY_KIND_NONE)
        {
            CHECK(actual.damage == 0);
            CHECK(actual.cure == 0);
        }

        switch (expected.kind)
        {
            case FFXIV_POTENCY_KIND_DAMAGE: ++damage; break;
            case FFXIV_POTENCY_KIND_HEAL: ++heal; break;
            case FFXIV_POTENCY_KIND_CONDITIONAL: ++conditional; break;
            default: ++none; break;
        }
    }

    // The published bucket sizes: 235 Action rows + 5 starter damage minus the seven rows whose
    // only "Potency:" is a tick or an MP regen (233), 74 + 2 heals, 144 minus the two conditional
    // DoTs (142), 202 + 2 travel + 9 rows the DoT correction moved (213). A DoT-only action's
    // damage column is 0: its tick is ffxiv_action_dot's, never a direct hit.
    // The four Warrior status-gated replacement actions (16463, 16465, 36924, 36925) are damage rows,
    // selected by tools/generate-ffxiv-class-spells.py's PROC_JOBS instead of IsPlayerAction.
    CHECK(damage == 233);
    CHECK(heal == 76);
    CHECK(conditional == 142);
    CHECK(none == 213);
}

TEST_CASE("FFXIV action potency: Cure and Medica job/level trees", "[ffxiv]")
{
    // Cure (120), sheet: If ClassJob == 24 (White Mage) then (If level >= 85 then 500 else 450)
    // else 450. The 85 is the trait level the sheet gates on; the fallback is the same 450.
    CHECK(Potency(120, 24, 1).cure == 450);
    CHECK(Potency(120, 24, 50).cure == 450);
    CHECK(Potency(120, 24, 84).cure == 450);
    CHECK(Potency(120, 24, 85).cure == 500);
    CHECK(Potency(120, 24, 90).cure == 500);
    CHECK(Potency(120, 6, 90).cure == 450);   // Conjurer keeps the base value
    CHECK(Potency(120, 0, 100).cure == 450);

    // Medica (124), sheet: the same shape with 400 / 300.
    CHECK(Potency(124, 24, 50).cure == 300);
    CHECK(Potency(124, 24, 84).cure == 300);
    CHECK(Potency(124, 24, 85).cure == 400);
    CHECK(Potency(124, 24, 90).cure == 400);
    CHECK(Potency(124, 6, 90).cure == 300);

    for (uint32 cure : { 120u, 124u })
    {
        FfxivActionPotency potency = Potency(cure, 24, 90);
        CHECK(potency.kind == FFXIV_POTENCY_KIND_HEAL);
        CHECK(potency.damage == 0);  // a heal states no damage potency
    }
}

TEST_CASE("FFXIV action potency: per-job branched actions", "[ffxiv]")
{
    // Warrior, Heavy Swing (31): If ClassJob == 21 then (If level >= 94 then 240 else
    // (If level >= 84 then 200 else 150)) else 150.
    CHECK(Potency(31, 21, 50).damage == 150);
    CHECK(Potency(31, 21, 83).damage == 150);
    CHECK(Potency(31, 21, 84).damage == 200);
    CHECK(Potency(31, 21, 93).damage == 200);
    CHECK(Potency(31, 21, 94).damage == 240);
    CHECK(Potency(31, 19, 90).damage == 150);   // Paladin keeps the base number
    CHECK(Potency(31, 21, 90).kind == FFXIV_POTENCY_KIND_CONDITIONAL);

    // White Mage, Glare III (25859): If ClassJob == 24 then (If level >= 94 then 350 else 310)
    // else 310.
    CHECK(Potency(25859, 24, 50).damage == 310);
    CHECK(Potency(25859, 24, 93).damage == 310);
    CHECK(Potency(25859, 24, 94).damage == 350);
    CHECK(Potency(25859, 6, 94).damage == 310);
    CHECK(Potency(25859, 0, 94).damage == 310);

    // Machinist, Drill (16498) and Heated Split Shot (7411): the same job/level chain for 31.
    CHECK(Potency(16498, 31, 50).damage == 620);
    CHECK(Potency(16498, 31, 94).damage == 660);
    CHECK(Potency(16498, 0, 94).damage == 620);

    CHECK(Potency(7411, 31, 50).damage == 180);
    CHECK(Potency(7411, 31, 84).damage == 200);
    CHECK(Potency(7411, 31, 94).damage == 220);
    CHECK(Potency(7411, 0, 94).damage == 180);

    // Second Wind (7541) is a 13-job chain behind one level test: 800 at level 94 for the jobs the
    // sheet names (2, 20, 4, 22, 29, 30, 34, 39, 2, 23, 31, 38, 41 -- the sheet lists 2 twice), 500
    // for every other job and below 94.
    CHECK(Potency(7541, 2, 94).cure == 800);
    CHECK(Potency(7541, 31, 94).cure == 800);
    CHECK(Potency(7541, 41, 94).cure == 800);
    CHECK(Potency(7541, 24, 94).cure == 500);
    CHECK(Potency(7541, 2, 93).cure == 500);
    CHECK(Potency(7541, 2, 94).kind == FFXIV_POTENCY_KIND_HEAL);
}

TEST_CASE("FFXIV action potency: the sheet's repeated tests stay faithful", "[ffxiv]")
{
    // Fast Blade (9), sheet: If ClassJob == 19 then (If level >= 94 then 220 else
    // (If level >= 84 then 200 else 150)) else (If ClassJob == 19 then
    // (If level >= 84 then 200 else 150) else 150).
    //
    // The false branch of the outer test re-tests ClassJob == 19 after ClassJob != 19 (the module
    // table's "gnum68!=19 && gnum68=19" rows). The evaluator runs the second test as written, so
    // those rows can never match and a non-Paladin gets the 150 of the sheet's last else -- the
    // resolver does not drop the row, merge it with the first, or pick a value for it.
    CHECK(Potency(9, 19, 50).damage == 150);
    CHECK(Potency(9, 19, 83).damage == 150);
    CHECK(Potency(9, 19, 84).damage == 200);
    CHECK(Potency(9, 19, 93).damage == 200);
    CHECK(Potency(9, 19, 94).damage == 220);
    CHECK(Potency(9, 20, 90).damage == 150);
    CHECK(Potency(9, 0, 0).damage == 150);
    CHECK(Potency(9, 19, 90).kind == FFXIV_POTENCY_KIND_CONDITIONAL);

    // The redundant re-test is present in the generated table, not collapsed: the second
    // ClassJob == 19 node is a node of its own (see the generator's one-for-one assertion).
    CHECK(HasFfxivActionPotency(9));
}

TEST_CASE("FFXIV action potency: the nine hand-authored starter actions", "[ffxiv]")
{
    // Datasheet values from datascripts/classes/spells/ffxiv-starter-data.ts; the generator decodes
    // the same rows and fails if the sheet stops agreeing with them.
    CHECK(Potency(119, 24, 90).damage == 140);   // Stone: "potency of 140"
    CHECK(Potency(121, 24, 90).damage == 50);    // Aero: "potency of 50" + a 30 DoT (not in the API)
    CHECK(Potency(141, 24, 90).damage == 180);   // Fire
    CHECK(Potency(142, 24, 90).damage == 180);   // Blizzard
    CHECK(Potency(144, 24, 90).damage == 100);   // Thunder: "potency of 100" + a 45 DoT

    CHECK(Potency(119, 24, 90).kind == FFXIV_POTENCY_KIND_DAMAGE);
    CHECK(Potency(121, 24, 90).kind == FFXIV_POTENCY_KIND_DAMAGE);
    CHECK(Potency(141, 24, 90).kind == FFXIV_POTENCY_KIND_DAMAGE);
    CHECK(Potency(142, 24, 90).kind == FFXIV_POTENCY_KIND_DAMAGE);
    CHECK(Potency(144, 24, 90).kind == FFXIV_POTENCY_KIND_DAMAGE);

    // Teleport (5) and Return (6) state no potency.
    for (uint32 action : { 5u, 6u })
    {
        FfxivActionPotency potency = Potency(action, 24, 90);
        CHECK(potency.kind == FFXIV_POTENCY_KIND_NONE);
        CHECK(potency.damage == 0);
        CHECK(potency.cure == 0);
    }

    // Damage over time is not in this API: Aero's second number is a per-tick potency the sheet
    // states separately, and the resolver must not fold it into damage or cure.
    CHECK(Potency(121, 24, 90).cure == 0);
    CHECK(Potency(144, 24, 90).cure == 0);
}

TEST_CASE("FFXIV action potency: a plain row ignores job and level", "[ffxiv]")
{
    // Shield Bash (16) is "potency of 100" with no condition: every job and level sees 100.
    for (uint8 job : { 0, 19, 21, 24, 31, 42 })
        for (uint8 level : { 1, 50, 90, 100 })
            CHECK(Potency(16, job, level).damage == 100);

    CHECK(Potency(16, 19, 90).kind == FFXIV_POTENCY_KIND_DAMAGE);
    CHECK(Potency(16, 19, 90).cure == 0);
}
