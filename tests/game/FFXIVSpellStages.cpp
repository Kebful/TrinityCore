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
 * The engine-spine stage rule (FFXIV/Combat/FFXIVSpellStages.h):
 *
 *   * selection is the data -- a mapped spell whose Action has a potency row -- and nothing else;
 *   * every avoidance roll an FFXIV action must not make is cleared in one place;
 *   * the engine crit chance is zero (the per-target FFXIV roll decides);
 *   * the roll seed is per target, so an area cast does not crit or vary as one block.
 *
 * No Unit or Spell is built: the predicate and the pure roll source are the whole contract.
 */

#include "tc_catch2.h"

#include "FFXIV/Combat/FFXIVActionDamage.h"
#include "FFXIV/Combat/FFXIVCombatMath.h"
#include "FFXIV/Combat/FFXIVSpellStages.h"

#include <vector>

using namespace FFXIV::Combat;

namespace
{
constexpr uint32 ACTION_WITH_POTENCY = 31;   // Heavy Swing
constexpr uint32 ACTION_DOT_ONLY = 17864;    // Bio: a potency row with damage 0 and dot 20
constexpr uint32 ACTION_WITHOUT_ROW = 99999; // mapped, but the potency table does not carry it
constexpr uint32 SPELL_WITH_POTENCY = 80985;
constexpr uint32 SPELL_DOT_ONLY = 82202;
constexpr uint32 SPELL_WITHOUT_ROW = 89999;
constexpr uint32 PLACEHOLDER_SPELL = 123456; // no map row at all

void InstallTables()
{
    std::vector<FfxivActionPotencyRow> potency;
    FfxivActionPotencyRow heavy;
    heavy.actionId = ACTION_WITH_POTENCY;
    heavy.classJob = 0;
    heavy.minLevel = 1;
    heavy.damage = 150;
    potency.push_back(heavy);

    FfxivActionPotencyRow bio;
    bio.actionId = ACTION_DOT_ONLY;
    bio.classJob = 0;
    bio.minLevel = 1;
    bio.dot = 20;
    potency.push_back(bio);

    SetFfxivActionDamageTables(std::move(potency), {});

    std::vector<FfxivActionSpellRow> spells;
    spells.push_back(FfxivActionSpellRow{ ACTION_WITH_POTENCY, SPELL_WITH_POTENCY, 3, false });
    spells.push_back(FfxivActionSpellRow{ ACTION_DOT_ONLY, SPELL_DOT_ONLY, 2, false });
    spells.push_back(FfxivActionSpellRow{ ACTION_WITHOUT_ROW, SPELL_WITHOUT_ROW, 3, false });
    SetFfxivActionSpellRows(std::move(spells));
}
} // namespace

TEST_CASE("FFXIV action stage selection is the data, never an id list", "[ffxiv]")
{
    InstallTables();

    CHECK(IsFfxivActionSpell(SPELL_WITH_POTENCY));
    CHECK(IsFfxivActionSpell(SPELL_DOT_ONLY));      // a DoT-only row still owns the spell
    CHECK_FALSE(IsFfxivActionSpell(SPELL_WITHOUT_ROW)); // mapped but no potency row
    CHECK_FALSE(IsFfxivActionSpell(PLACEHOLDER_SPELL)); // no mapping
    CHECK_FALSE(IsFfxivActionSpell(uint32(0)));

    // The predicate the Unit/Spell guards read is the same one.
    CHECK(FfxivActionOwnsCombatStages(nullptr) == false);

    ClearFfxivActionDamageTables();
    CHECK_FALSE(IsFfxivActionSpell(SPELL_WITH_POTENCY)); // empty table: nothing is ours
}

TEST_CASE("FFXIV action clears every avoidance roll in one place", "[ffxiv]")
{
    InstallTables();

    // MISS, RESIST, DODGE, PARRY, BLOCK, DEFLECT -> NONE (0) for an FFXIV action.
    for (uint8 miss : { uint8(1), uint8(2), uint8(3), uint8(4), uint8(5), uint8(9) })
    {
        uint8 value = miss;
        NeutraliseFfxivAvoidance(SPELL_WITH_POTENCY, value);
        CHECK(value == 0);
    }

    // Evade (6), immunity (7/8) and reflect (11) are not rolls and stay.
    for (uint8 miss : { uint8(6), uint8(7), uint8(8), uint8(11) })
    {
        uint8 value = miss;
        NeutraliseFfxivAvoidance(SPELL_WITH_POTENCY, value);
        CHECK(value == miss);
    }

    // A non-action spell is untouched, and so is a mapped spell with no potency row.
    uint8 untouched = 3;
    NeutraliseFfxivAvoidance(PLACEHOLDER_SPELL, untouched);
    CHECK(untouched == 3);
    uint8 mappedOnly = 3;
    NeutraliseFfxivAvoidance(SPELL_WITHOUT_ROW, mappedOnly);
    CHECK(mappedOnly == 3);
}

TEST_CASE("FFXIV action neutralises the engine crit chance", "[ffxiv]")
{
    InstallTables();

    float chance = 24.5f;
    NeutraliseFfxivCritChance(SPELL_WITH_POTENCY, chance);
    CHECK(chance == 0.0f);

    float other = 24.5f;
    NeutraliseFfxivCritChance(PLACEHOLDER_SPELL, other);
    CHECK(other == 24.5f);
}

TEST_CASE("FFXIV per-target roll seed: same target reproduces, another target diverges", "[ffxiv]")
{
    constexpr uint64 CASTER = 0x1000000000000A01ull;
    constexpr uint64 TARGET_A = 0x2000000000000B01ull;
    constexpr uint64 TARGET_B = 0x2000000000000B02ull;
    constexpr uint32 ACTION = ACTION_WITH_POTENCY;
    constexpr uint32 NOW = 123456;

    // A cast-wide seed would be this value for every target; the target guid is in the mix.
    CHECK(FfxivActionRollSeed(CASTER, ACTION, TARGET_A, NOW)
          == FfxivActionRollSeed(CASTER, ACTION, TARGET_A, NOW));
    CHECK(FfxivActionRollSeed(CASTER, ACTION, TARGET_A, NOW)
          != FfxivActionRollSeed(CASTER, ACTION, TARGET_B, NOW));

    // The actual draws the pipeline makes: over 16 casts each target draws its own sequence.
    FfxivSeededRollSource rollsA(FfxivActionRollSeed(CASTER, ACTION, TARGET_A, NOW));
    FfxivSeededRollSource rollsB(FfxivActionRollSeed(CASTER, ACTION, TARGET_B, NOW));
    bool anyDifference = false;
    for (int i = 0; i < 16; ++i)
    {
        int32 const varianceA = rollsA.RollVariance(95, 105);
        int32 const varianceB = rollsB.RollVariance(95, 105);
        if (varianceA != varianceB)
            anyDifference = true;
        CHECK(varianceA >= 95);
        CHECK(varianceA <= 105);
        CHECK(varianceB >= 95);
        CHECK(varianceB <= 105);
    }
    CHECK(anyDifference);
}
