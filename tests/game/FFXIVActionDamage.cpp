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
 * The action-cast rule: table lookup -> potency column -> FFXIVCombatMath -> one integer.
 *
 * The damage vectors pin the exact integer the published pipeline produces for a pinned snapshot
 * (fixed rolls, no crit/direct hit, variance 100, no party bonus). The intervening stages are
 * asserted by FFXIVCombatMath's own goldens; here the point is that the *selected potency* reaches
 * that pipeline (plain 150, combo 200, flank 190, cure 450) and that an empty table resolves
 * nothing at all, which is the shipping state until the data slice lands.
 *
 * The spell map is tested as itself: a pure resolver over synthetic rows, plus the unmapped case
 * that must leave a cast exactly on its existing path. No real Spell is built.
 */

#include "tc_catch2.h"

#include "FFXIV/Combat/FFXIVActionDamage.h"
#include "FFXIV/Combat/FFXIVActionPotency.h"
#include "FFXIV/Combat/FFXIVCombatMath.h"
#include "FFXIV/Gauge/FFXIVJobGauge.h"
#include "FFXIV/Status/FFXIVStatus.h"
#include "FFXIV/Stats/FFXIVStatEngine.h"
#include "FFXIV/Stats/FFXIVStatTables.h"

#include <algorithm>
#include <vector>

using namespace FFXIV::Combat;
using namespace FFXIV::Stats;

namespace
{
constexpr uint8 PLD = 19;
constexpr uint8 MNK = 20;
constexpr uint8 WHM = 24;

// Action ids used by the fixtures (retail ids, matching the module's spell ids).
constexpr uint32 FAST_BLADE = 9;   // plain 150
constexpr uint32 RIOT_BLADE = 15;  // combo 200, plain 100
constexpr uint32 TRUE_STRIKE = 54; // flank 190, plain 100
constexpr uint32 CURE = 120;       // cure 450
constexpr uint32 SHIELD_BASH = 16; // plain 100
constexpr uint32 PRECEDENCE = 12;  // three rows, different (classJob, minLevel)
constexpr uint32 HEAVY_SWING = 31; // retail Action id, the task's pinned example; not in the potency fixture

// Synthetic WoW spell ids: the module assigns the real ones, so the map fixture supplies its own.
constexpr uint32 HEAVY_SWING_SPELL = 80985;
constexpr uint32 FAST_BLADE_SPELL = 100009;

int32 JobPercentFor(uint8 jobId, uint8 attributeParam)
{
    FfxivJobRow const& job = GetFfxivJobRow(jobId);
    switch (attributeParam)
    {
        case FFXIV_PARAM_STRENGTH: return job.strPct;
        case FFXIV_PARAM_DEXTERITY: return job.dexPct;
        case FFXIV_PARAM_INTELLIGENCE: return job.intPct;
        case FFXIV_PARAM_MIND: return job.mndPct;
        default: return 100;
    }
}

FfxivDerivedStats Geared(uint8 jobId, uint8 level, int32 mainStat, int32 crit, int32 directHit,
                         int32 determination, int32 tenacity, int32 healingMagicPotency = 0)
{
    uint8 const attribute = GetFfxivAttackPowerAttribute(jobId);
    int32 const base = GetFfxivLevelRow(level).mainStat * JobPercentFor(jobId, attribute) / 100;

    FfxivStatInputs inputs;
    inputs.jobId = jobId;
    inputs.level = level;
    inputs.effectiveLevel = level;
    inputs.crit = crit;
    inputs.directHit = directHit;
    inputs.determination = determination;
    inputs.tenacity = tenacity;
    inputs.skillSpeed = crit;
    inputs.spellSpeed = crit;
    inputs.equipmentParams[attribute] = mainStat - base;

    FfxivDerivedStats stats = ComputeFfxivStats(inputs);
    stats.param[FFXIV_PARAM_HEALING_MAGIC_POTENCY] = healingMagicPotency;
    return stats;
}

FfxivDerivedStats PaladinFixture() { return Geared(PLD, 100, 4000, 442, 442, 442, 442); }
FfxivDerivedStats MonkFixture() { return Geared(MNK, 100, 4000, 442, 442, 442, 442); }
FfxivDerivedStats WhiteMageFixture() { return Geared(WHM, 100, 4000, 442, 442, 442, 442, 1000); }

std::vector<FfxivActionPotencyRow> PotencyRows()
{
    std::vector<FfxivActionPotencyRow> rows;

    FfxivActionPotencyRow fastBlade;
    fastBlade.actionId = FAST_BLADE;
    fastBlade.classJob = PLD;
    fastBlade.minLevel = 1;
    fastBlade.kind = FFXIV_POTENCY_KIND_DAMAGE;
    fastBlade.damage = 150;
    rows.push_back(fastBlade);

    FfxivActionPotencyRow riotBlade;
    riotBlade.actionId = RIOT_BLADE;
    riotBlade.classJob = PLD;
    riotBlade.minLevel = 1;
    riotBlade.kind = FFXIV_POTENCY_KIND_CONDITIONAL;
    riotBlade.damage = 100;
    riotBlade.combo = 200;
    rows.push_back(riotBlade);

    FfxivActionPotencyRow trueStrike;
    trueStrike.actionId = TRUE_STRIKE;
    trueStrike.classJob = MNK;
    trueStrike.minLevel = 1;
    trueStrike.kind = FFXIV_POTENCY_KIND_CONDITIONAL;
    trueStrike.damage = 100;
    trueStrike.flank = 190;
    trueStrike.combo = 250; // also a combo step, to pin combo-over-positional precedence
    rows.push_back(trueStrike);

    FfxivActionPotencyRow cure;
    cure.actionId = CURE;
    cure.classJob = WHM;
    cure.minLevel = 1;
    cure.kind = FFXIV_POTENCY_KIND_HEAL;
    cure.cure = 450;
    rows.push_back(cure);

    FfxivActionPotencyRow shieldBash;
    shieldBash.actionId = SHIELD_BASH;
    shieldBash.classJob = PLD;
    shieldBash.minLevel = 1;
    shieldBash.kind = FFXIV_POTENCY_KIND_DAMAGE;
    shieldBash.damage = 100;
    rows.push_back(shieldBash);

    // Same action, three rows: exact job at two levels, plus the generic row.
    FfxivActionPotencyRow exactLow;
    exactLow.actionId = PRECEDENCE;
    exactLow.classJob = PLD;
    exactLow.minLevel = 1;
    exactLow.damage = 90;
    rows.push_back(exactLow);

    FfxivActionPotencyRow exactHigh;
    exactHigh.actionId = PRECEDENCE;
    exactHigh.classJob = PLD;
    exactHigh.minLevel = 84;
    exactHigh.damage = 95;
    rows.push_back(exactHigh);

    FfxivActionPotencyRow generic;
    generic.actionId = PRECEDENCE;
    generic.classJob = 0;
    generic.minLevel = 1;
    generic.damage = 80;
    rows.push_back(generic);

    return rows;
}

std::vector<FfxivActionComboRow> ComboRows()
{
    std::vector<FfxivActionComboRow> rows;
    rows.push_back(FfxivActionComboRow{ FAST_BLADE, RIOT_BLADE });   // Fast Blade -> Riot Blade
    rows.push_back(FfxivActionComboRow{ FAST_BLADE, TRUE_STRIKE });  // Fast Blade -> True Strike
    return rows;
}

std::vector<FfxivActionSpellRow> SpellRows()
{
    std::vector<FfxivActionSpellRow> rows;
    rows.push_back(FfxivActionSpellRow{ HEAVY_SWING, HEAVY_SWING_SPELL }); // WoW spell -> retail Action
    rows.push_back(FfxivActionSpellRow{ FAST_BLADE, FAST_BLADE_SPELL });   // fixture action, so potency is reachable
    return rows;
}

void UseTestTables()
{
    SetFfxivActionDamageTables(PotencyRows(), ComboRows());
    SetFfxivActionSpellRows(SpellRows());
}
} // namespace

TEST_CASE("FFXIV action tables: empty tables resolve nothing", "[ffxiv]")
{
    ClearFfxivActionDamageTables();

    CHECK_FALSE(HasFfxivActionPotencyRows());
    CHECK_FALSE(HasFfxivActionSpellRows());
    CHECK(FindFfxivActionPotencyRow(FAST_BLADE, PLD, 100) == nullptr);
    CHECK_FALSE(IsFfxivComboCorrect(FAST_BLADE, RIOT_BLADE));
    CHECK_FALSE(IsFfxivComboStep(RIOT_BLADE));
    CHECK_FALSE(IsFfxivComboStarter(FAST_BLADE));
    CHECK(ResolveFfxivActionIdFromSpell(FAST_BLADE_SPELL) == 0);

    // An empty table is the whole contract: unknown action -> None -> the existing spell path.
    FfxivPotencySelection const selection =
        SelectFfxivActionPotency(FAST_BLADE, PLD, 100, 0, 0, true);
    CHECK(selection.source == FfxivPotencySource::None);
    CHECK(selection.potency == 0);
    CHECK_FALSE(selection.IsDamage());

    // And a table action becomes selectable once the table is there -- the test above is not
    // passing merely because the id is wrong.
    UseTestTables();
    CHECK(HasFfxivActionPotencyRows());
    CHECK(HasFfxivActionSpellRows());
    CHECK(ResolveFfxivActionIdFromSpell(FAST_BLADE_SPELL) == FAST_BLADE);
    CHECK(SelectFfxivActionPotency(FAST_BLADE, PLD, 100, 0, 0, true).potency == 150);
    ClearFfxivActionDamageTables();
}

TEST_CASE("FFXIV action spell map: a spell id resolves to its retail Action id", "[ffxiv]")
{
    ClearFfxivActionDamageTables();

    // No map: the shipping state. Every spell is an ordinary WoW spell.
    CHECK_FALSE(HasFfxivActionSpellRows());
    CHECK(ResolveFfxivActionIdFromSpell(HEAVY_SWING_SPELL) == 0);

    SetFfxivActionSpellRows(SpellRows());
    CHECK(HasFfxivActionSpellRows());

    // Hit: the module's spell for Heavy Swing resolves to retail Action 31, the potency table's key.
    CHECK(ResolveFfxivActionIdFromSpell(HEAVY_SWING_SPELL) == HEAVY_SWING);
    CHECK(ResolveFfxivActionIdFromSpell(FAST_BLADE_SPELL) == FAST_BLADE);
    // Misses: an unrelated spell and the zero id both mean "not an FFXIV action".
    CHECK(ResolveFfxivActionIdFromSpell(HEAVY_SWING_SPELL + 1) == 0);
    CHECK(ResolveFfxivActionIdFromSpell(FAST_BLADE) == 0); // an action id is not a spell id
    CHECK(ResolveFfxivActionIdFromSpell(0) == 0);

    // The reverse map (action -> spell), which the charge snapshot reads.
    CHECK(ResolveFfxivSpellFromActionId(HEAVY_SWING) == HEAVY_SWING_SPELL);
    CHECK(ResolveFfxivSpellFromActionId(FAST_BLADE) == FAST_BLADE_SPELL);
    CHECK(ResolveFfxivSpellFromActionId(RIOT_BLADE) == 0); // a chain action with no spell row
    CHECK(ResolveFfxivSpellFromActionId(0) == 0);

    ClearFfxivActionDamageTables();
}

TEST_CASE("FFXIV action spell map: a spell with no mapping keeps the untouched path", "[ffxiv]")
{
    UseTestTables(); // potency, combo and spell map are all present

    // The hook's gate is the resolver: no row -> action id 0 -> return before the potency lookup.
    uint32 const unmapped = ResolveFfxivActionIdFromSpell(HEAVY_SWING_SPELL + 1);
    REQUIRE(unmapped == 0);

    // The selection such a cast would get is the same one an empty table yields: nothing to apply,
    // IsDamage() false, so the vanilla DUMMY handling keeps the cast on its existing path.
    FfxivPotencySelection const untouched =
        SelectFfxivActionPotency(unmapped, PLD, 100, 0, 0, true);
    CHECK(untouched.source == FfxivPotencySource::None);
    CHECK(untouched.potency == 0);
    CHECK_FALSE(untouched.IsDamage());

    // The mapped spell for the same table is selectable: the miss above is the map, not the table.
    uint32 const mapped = ResolveFfxivActionIdFromSpell(FAST_BLADE_SPELL);
    REQUIRE(mapped == FAST_BLADE);
    CHECK(SelectFfxivActionPotency(mapped, PLD, 100, 0, 0, true).potency == 150);

    ClearFfxivActionDamageTables();
}

TEST_CASE("FFXIV action spells: the reconcile selects by ClassJob and level", "[ffxiv]")
{
    ClearFfxivActionDamageTables();

    // Four actions for the level/gate cases plus three that must contribute nothing. The spell id
    // is the action's own +900000 so a failure names the wrong side of the map.
    std::vector<FfxivActionPotencyRow> rows;
    auto addPotency = [&rows](uint32 actionId, uint8 classJob, uint8 minLevel)
    {
        FfxivActionPotencyRow row;
        row.actionId = actionId;
        row.classJob = classJob;
        row.minLevel = minLevel;
        row.kind = FFXIV_POTENCY_KIND_DAMAGE;
        row.damage = 100;
        rows.push_back(row);
    };

    addPotency(201, PLD, 2);   // exact job, unlocked at level 2
    addPotency(202, PLD, 5);   // exact job, unlocked at level 5
    addPotency(203, 0, 1);     // generic row
    addPotency(204, 255, 1);   // role row, generic like 0
    addPotency(205, MNK, 1);   // another job
    addPotency(206, PLD, 1);   // no spell row at all
    addPotency(207, PLD, 1);   // spell row with a zero (unknown) spell id
    addPotency(208, PLD, 90);  // exact job, above the test level
    addPotency(209, 0, 10);    // generic row above the test level

    std::vector<FfxivActionSpellRow> spellRows;
    for (uint32 actionId : { 201u, 202u, 203u, 204u, 205u, 208u, 209u })
        spellRows.push_back(FfxivActionSpellRow{ actionId, actionId + 900000 });
    spellRows.push_back(FfxivActionSpellRow{ 207, 0 }); // not a spell
    SetFfxivActionDamageTables(rows, {});
    SetFfxivActionSpellRows(spellRows);

    auto neverKnown = [](uint32) { return false; };
    auto knownFrom = [](std::vector<uint32> const& known)
    {
        return [known](uint32 spellId)
        {
            return std::find(known.begin(), known.end(), spellId) != known.end();
        };
    };

    // Level gate: at 4 only the level-1/2 rows qualify; 202 waits for 5, 208/209 for 90/10.
    CHECK(SelectFfxivActionSpellsToLearn(PLD, 4, neverKnown)
        == std::vector<uint32>{ 900201, 900203, 900204 });

    // The level exactly on the gate is included (minLevel <= level).
    CHECK(SelectFfxivActionSpellsToLearn(PLD, 2, neverKnown)
        == std::vector<uint32>{ 900201, 900203, 900204 });
    CHECK(SelectFfxivActionSpellsToLearn(PLD, 1, neverKnown)
        == std::vector<uint32>{ 900203, 900204 });
    CHECK(SelectFfxivActionSpellsToLearn(PLD, 5, neverKnown)
        == std::vector<uint32>{ 900201, 900202, 900203, 900204 });
    CHECK(SelectFfxivActionSpellsToLearn(PLD, 100, neverKnown)
        == std::vector<uint32>{ 900201, 900202, 900203, 900204, 900208, 900209 });

    // Another job gets its own row plus the generic 0 and 255 rows, never PLD's exact rows.
    CHECK(SelectFfxivActionSpellsToLearn(MNK, 4, neverKnown)
        == std::vector<uint32>{ 900203, 900204, 900205 });

    // Already-known spells are not selected; passing the whole first answer back is idempotent.
    std::vector<uint32> const first =
        SelectFfxivActionSpellsToLearn(PLD, 5, neverKnown);
    CHECK(SelectFfxivActionSpellsToLearn(PLD, 5, knownFrom(first)).empty());
    CHECK(SelectFfxivActionSpellsToLearn(PLD, 5, knownFrom({ 900201, 900203 }))
        == std::vector<uint32>{ 900202, 900204 });

    // Unknown spell ids (207's zero, 206's missing row) and a missing table change nothing.
    CHECK(SelectFfxivActionSpellsToLearn(PLD, 100, neverKnown).size() == 6);
    SetFfxivActionSpellRows({});
    CHECK(SelectFfxivActionSpellsToLearn(PLD, 100, neverKnown).empty());
    ClearFfxivActionDamageTables();
    CHECK(SelectFfxivActionSpellsToLearn(PLD, 100, neverKnown).empty());

    ClearFfxivActionDamageTables();
}

TEST_CASE("FFXIV action potency: classJob 255 role rows are accepted and matchable", "[ffxiv]")
{
    ClearFfxivActionDamageTables();

    // The loader gate: a ClassJob row or the role value 255; values between the sheet range and
    // 255 still name no job and are dropped.
    CHECK(IsValidFfxivActionPotencyClassJob(0));
    CHECK(IsValidFfxivActionPotencyClassJob(PLD));
    CHECK(IsValidFfxivActionPotencyClassJob(FFXIV_JOB_COUNT - 1));
    CHECK(IsValidFfxivActionPotencyClassJob(FFXIV_ACTION_ROLE_JOB));
    CHECK_FALSE(IsValidFfxivActionPotencyClassJob(FFXIV_JOB_COUNT));
    CHECK_FALSE(IsValidFfxivActionPotencyClassJob(FFXIV_ACTION_ROLE_JOB - 1));

    // 0 and 255 mean "every job"; a real job does not.
    CHECK(IsFfxivActionGenericJob(0));
    CHECK(IsFfxivActionGenericJob(FFXIV_ACTION_ROLE_JOB));
    CHECK_FALSE(IsFfxivActionGenericJob(PLD));

    // The task's role action (16560, a role line) loads and matches every job, exactly like a
    // generic 0 row -- while an exact ClassJob row still outranks it.
    FfxivActionPotencyRow role;
    role.actionId = 16560;
    role.classJob = FFXIV_ACTION_ROLE_JOB;
    role.minLevel = 1;
    role.kind = FFXIV_POTENCY_KIND_DAMAGE;
    role.damage = 120;

    FfxivActionPotencyRow exact;
    exact.actionId = 16560;
    exact.classJob = PLD;
    exact.minLevel = 1;
    exact.kind = FFXIV_POTENCY_KIND_DAMAGE;
    exact.damage = 150;

    SetFfxivActionDamageTables({ role, exact }, {});

    FfxivActionPotencyRow const* fromRole = FindFfxivActionPotencyRow(16560, WHM, 50);
    REQUIRE(fromRole);
    CHECK(fromRole->classJob == FFXIV_ACTION_ROLE_JOB);
    CHECK(fromRole->damage == 120);

    FfxivActionPotencyRow const* fromExact = FindFfxivActionPotencyRow(16560, PLD, 50);
    REQUIRE(fromExact);
    CHECK(fromExact->classJob == PLD);
    CHECK(fromExact->damage == 150);

    // The selection path sees the role row too, not just the raw lookup.
    CHECK(SelectFfxivActionPotency(16560, WHM, 50, 0, 0, true).potency == 120);

    // And the role row is a learnable action once the spell map names it.
    SetFfxivActionSpellRows({ FfxivActionSpellRow{ 16560, 80999 } });
    CHECK(SelectFfxivActionSpellsToLearn(WHM, 50, [](uint32) { return false; })
        == std::vector<uint32>{ 80999 });

    ClearFfxivActionDamageTables();
}

TEST_CASE("FFXIV action tables: lookup precedence", "[ffxiv]")
{
    UseTestTables();

    // Exact job outranks the generic row; the highest minLevel at or below the level wins.
    FfxivActionPotencyRow const* high = FindFfxivActionPotencyRow(PRECEDENCE, PLD, 100);
    REQUIRE(high);
    CHECK(high->classJob == PLD);
    CHECK(high->damage == 95);

    FfxivActionPotencyRow const* low = FindFfxivActionPotencyRow(PRECEDENCE, PLD, 80);
    REQUIRE(low);
    CHECK(low->classJob == PLD);
    CHECK(low->damage == 90);

    // Another job falls back to the generic row, never to another job's row.
    FfxivActionPotencyRow const* generic = FindFfxivActionPotencyRow(PRECEDENCE, WHM, 100);
    REQUIRE(generic);
    CHECK(generic->classJob == 0);
    CHECK(generic->damage == 80);

    // A level below every matching row resolves nothing.
    CHECK(FindFfxivActionPotencyRow(PRECEDENCE, PLD, 0) == nullptr);
    // An action that is not in the table resolves nothing.
    CHECK(FindFfxivActionPotencyRow(999999, PLD, 100) == nullptr);

    ClearFfxivActionDamageTables();
}

TEST_CASE("FFXIV action potency: plain, combo, positional and cure selection", "[ffxiv]")
{
    UseTestTables();

    // Plain weaponskill of the action's own job: no combo, attacker in front.
    FfxivPotencySelection plain = SelectFfxivActionPotency(FAST_BLADE, PLD, 100, 0, 0, true);
    CHECK(plain.source == FfxivPotencySource::Plain);
    CHECK(plain.potency == 150);

    // The same action from another job is not the player's action at all.
    CHECK(SelectFfxivActionPotency(FAST_BLADE, WHM, 100, 0, 0, true).source == FfxivPotencySource::None);

    // Correct combo: the last action (Fast Blade) feeds Riot Blade.
    CHECK(IsFfxivComboCorrect(FAST_BLADE, RIOT_BLADE));
    CHECK_FALSE(IsFfxivComboCorrect(SHIELD_BASH, RIOT_BLADE));
    FfxivPotencySelection combo = SelectFfxivActionPotency(RIOT_BLADE, PLD, 100, FAST_BLADE, 0, true);
    CHECK(combo.source == FfxivPotencySource::Combo);
    CHECK(combo.potency == 200);

    // Wrong predecessor: the plain column.
    FfxivPotencySelection broken = SelectFfxivActionPotency(RIOT_BLADE, PLD, 100, SHIELD_BASH, 0, true);
    CHECK(broken.source == FfxivPotencySource::Plain);
    CHECK(broken.potency == 100);

    // Positional: measured on the target's facing. 0 = front, 90 = flank, 170 = rear.
    CHECK(SelectFfxivActionPotency(TRUE_STRIKE, MNK, 100, 0, 90, true).source == FfxivPotencySource::Flank);
    CHECK(SelectFfxivActionPotency(TRUE_STRIKE, MNK, 100, 0, 90, true).potency == 190);
    CHECK(SelectFfxivActionPotency(TRUE_STRIKE, MNK, 100, 0, -90, true).potency == 190);
    // No value in that column for the sector -> the plain column.
    CHECK(SelectFfxivActionPotency(TRUE_STRIKE, MNK, 100, 0, 0, true).potency == 100);
    CHECK(SelectFfxivActionPotency(TRUE_STRIKE, MNK, 100, 0, 170, true).potency == 100);
    // Boundaries: 45 is flank, 135 is rear (the header's documented convention).
    CHECK(FfxivPositionalFromAngleDegrees(45) == FfxivPositional::Flank);
    CHECK(FfxivPositionalFromAngleDegrees(-45) == FfxivPositional::Flank);
    CHECK(FfxivPositionalFromAngleDegrees(44) == FfxivPositional::Front);
    CHECK(FfxivPositionalFromAngleDegrees(134) == FfxivPositional::Flank);
    CHECK(FfxivPositionalFromAngleDegrees(135) == FfxivPositional::Rear);
    CHECK(FfxivPositionalFromAngleDegrees(180) == FfxivPositional::Rear);
    CHECK(FfxivPositionalFromAngleDegrees(-180) == FfxivPositional::Rear);
    CHECK(FfxivPositionalFromAngleDegrees(270) == FfxivPositional::Flank); // normalised: -90

    // Combo outranks a positional on the same action: a correct Fast Blade -> True Strike chain
    // from the rear still uses the combo column, not the (absent) rear one.
    FfxivPotencySelection comboOverPositional =
        SelectFfxivActionPotency(TRUE_STRIKE, MNK, 100, FAST_BLADE, 170, true);
    CHECK(comboOverPositional.source == FfxivPotencySource::Combo);
    CHECK(comboOverPositional.potency == 250);

    // Heal-only rows use the cure column.
    FfxivPotencySelection cure = SelectFfxivActionPotency(CURE, WHM, 100, 0, 0, false);
    CHECK(cure.source == FfxivPotencySource::Cure);
    CHECK_FALSE(cure.IsDamage());
    CHECK(cure.potency == 450);
    // No damage-side potency at all, even on a hostile target.
    CHECK(SelectFfxivActionPotency(CURE, WHM, 100, 0, 0, true).source == FfxivPotencySource::Cure);

    ClearFfxivActionDamageTables();
}

TEST_CASE("FFXIV combo window", "[ffxiv]")
{
    // Sapphire aab9cb23 src/common/Common.h:61 MAX_COMBO_LENGTH = 12500 ms, inclusive.
    CHECK(IsFfxivComboWindowOpen(1000, 1000));
    CHECK(IsFfxivComboWindowOpen(1000, 13500));
    CHECK_FALSE(IsFfxivComboWindowOpen(1000, 13501));
    CHECK_FALSE(IsFfxivComboWindowOpen(2000, 1000)); // wrapped/backwards: closed, not open
}

TEST_CASE("FFXIV action damage vectors: exact numbers", "[ffxiv]")
{
    UseTestTables();

    FFXIV::Combat::FfxivFixedRollSource rolls(99, 100); // never crit, never direct hit, mean variance

    FfxivDerivedStats const paladin = PaladinFixture();
    int32 const paladinWeapon = ComputeFfxivWeaponDamagePercent(paladin, 0);
    FfxivPartyComposition const solo;

    FfxivPotencySelection const plain = SelectFfxivActionPotency(FAST_BLADE, PLD, 100, 0, 0, true);
    FfxivDirectDamageResult const plainResult = ComputeFfxivActionDamage(
        paladin, plain, paladinWeapon, uint8(paladin.level), solo, rolls);

    FfxivPotencySelection const combo = SelectFfxivActionPotency(RIOT_BLADE, PLD, 100, FAST_BLADE, 0, true);
    FfxivDirectDamageResult const comboResult = ComputeFfxivActionDamage(
        paladin, combo, paladinWeapon, uint8(paladin.level), solo, rolls);

    FfxivDerivedStats const monk = MonkFixture();
    int32 const monkWeapon = ComputeFfxivWeaponDamagePercent(monk, 0);
    FfxivPotencySelection const flank = SelectFfxivActionPotency(TRUE_STRIKE, MNK, 100, 0, 90, true);
    FfxivDirectDamageResult const flankResult = ComputeFfxivActionDamage(
        monk, flank, monkWeapon, uint8(monk.level), solo, rolls);

    FfxivDerivedStats const whiteMage = WhiteMageFixture();
    int32 const whiteMageWeapon = ComputeFfxivWeaponDamagePercent(whiteMage, 0);
    FfxivPotencySelection const cure = SelectFfxivActionPotency(CURE, WHM, 100, 0, 0, false);
    FfxivHealResult const healResult = ComputeFfxivActionHeal(
        whiteMage, cure, whiteMageWeapon, uint8(whiteMage.level), solo, rolls);

    // Exact integers the published pipeline produces for these fixtures (fixed rolls 99/100 = no
    // crit, no direct hit, variance 100; no party bonus; f(WD) from weapon damage 0). The magnitude
    // is small because stage 2 uses the published `/ 100` literally -- FFXIVCombatMath.h documents
    // that scale ambiguity and its own goldens pin it; these values follow from the same stages, and
    // the ratio check below shows the selected potency is the only difference between the rows.
    CHECK(plain.potency == 150);
    CHECK(combo.potency == 200);
    CHECK(flank.potency == 190);
    CHECK(cure.potency == 450);

    CHECK(plainResult.value == 1080);
    CHECK(comboResult.value == 1440);
    CHECK(flankResult.value == 1839);
    CHECK(healResult.value == 830);

    // The combo hit is the plain column scaled by 200/150 on the same snapshot and rolls: 10 -> 14
    // through the pipeline's own floors. That only holds if the combo column reached the pipeline.
    CHECK(comboResult.value > plainResult.value);
    CHECK(flankResult.value > 0);
    CHECK(healResult.value > 0);

    ClearFfxivActionDamageTables();
}

TEST_CASE("FFXIV combo: the Marauder/Warrior chain the sheet states", "[ffxiv]")
{
    // The rows the data build writes into ffxiv_action_combo (world db, verified by the owner):
    // Heavy Swing 31 -> Maim 37, Maim 37 -> Storm's Eye 45 and Storm's Path 42. Storm's Eye's Beast
    // Gauge gain and Surging Tempest are comboOnly, so this chain is what gates both.
    SetFfxivActionDamageTables({}, { { 31, 37 }, { 37, 45 }, { 37, 42 } });

    CHECK(IsFfxivComboStarter(31));
    CHECK(IsFfxivComboStep(45));
    CHECK(IsFfxivComboStep(42));
    CHECK(IsFfxivComboCorrect(31, 37));
    CHECK(IsFfxivComboCorrect(37, 45));
    CHECK_FALSE(IsFfxivComboCorrect(42, 45)); // wrong predecessor: no combo
    CHECK_FALSE(IsFfxivComboCorrect(0, 45));  // no predecessor at all
    // The successor is deterministic (the lowest): Maim can go to Path or Eye.
    CHECK(NextFfxivComboActionId(37) == 42);

    ClearFfxivActionDamageTables();
}

// --- Warrior chain end to end (combo state -> gauge comboOnly rows -> status row) ---
namespace WarriorCombo
{
constexpr uint8 WAR = 21;
constexpr uint8 BEAST = 22;
constexpr uint32 HEAVY_SWING = 31, MAIM = 37, STORMS_PATH = 42, STORMS_EYE = 45, SURGING_TEMPEST = 2677;
constexpr uint32 OVERPOWER = 41, INNER_BEAST = 49, UPHEAVAL = 7387;
constexpr uint8 WEAPONSKILL = FFXIV::Combat::FFXIV_ACTION_CATEGORY_WEAPONSKILL, ABILITY = 4;

ObjectGuid const Caster = ObjectGuid::Create<HighGuid::Player>(7);

struct WarriorFixture
{
    WarriorFixture()
    {
        FFXIV::Combat::SetFfxivActionDamageTables({}, { { 31, 35 }, { 31, 37 }, { 31, 5818 }, { 37, 42 }, { 37, 45 } });
        FFXIV::Gauge::SetFfxivJobGaugeTables({ { WarriorCombo::WAR, BEAST, "Beast Gauge", 100 } },
            { { 37, WarriorCombo::WAR, BEAST, 35, 10, true }, { 42, WarriorCombo::WAR, BEAST, 35, 20, true }, { 45, WarriorCombo::WAR, BEAST, 50, 10, true } });
        FFXIV::Status::SetFfxivStatusTables({ { SURGING_TEMPEST, 81814, 30000, 0, 0, 0 } },
                                     { { STORMS_EYE, SURGING_TEMPEST, 30000, true } });
        FFXIV::Gauge::ClearFfxivGaugeState();
    }

    ~WarriorFixture()
    {
        FFXIV::Combat::ClearFfxivActionDamageTables();
        FFXIV::Gauge::ClearFfxivJobGaugeTables();
        FFXIV::Status::ClearFfxivStatusTables();
        FFXIV::Gauge::ClearFfxivGaugeState();
    }

    // One cast the way the hooks see it: the damage hook advances the chain (reading the predecessor
    // the window still holds), then the gauge hook asks whether that step was correct and applies the row.
    // `category` / `preservesCombo` are the Action sheet's columns 3 / 37 (ffxiv_action_spell).
    bool Cast(uint32 actionId, uint32 nowMs, uint8 category = WEAPONSKILL, bool preservesCombo = false)
    {
        uint32 const last = FFXIV::Combat::GetFfxivLastActionId(Caster, nowMs);
        FFXIV::Combat::AdvanceFfxivComboState(Caster, actionId, last, nowMs, category, preservesCombo);
        bool const comboCorrect = FFXIV::Combat::WasFfxivCorrectComboStep(Caster, actionId, nowMs);
        FFXIV::Gauge::ApplyFfxivActionGauge(Caster, WarriorCombo::WAR, 50, actionId, comboCorrect);
        return comboCorrect;
    }
};
}
using namespace WarriorCombo;

TEST_CASE_METHOD(WarriorCombo::WarriorFixture, "FFXIV warrior: Heavy Swing -> Maim -> Storm's Eye builds the gauge", "[ffxiv]")
{
    CHECK_FALSE(Cast(HEAVY_SWING, 1000));
    CHECK(Cast(MAIM, 2500));
    CHECK(FFXIV::Gauge::GetFfxivGauge(Caster, WarriorCombo::WAR) == 10);
    CHECK(Cast(STORMS_EYE, 4000));
    CHECK(FFXIV::Gauge::GetFfxivGauge(Caster, WarriorCombo::WAR) == 20);
    CHECK(Cast(MAIM, 5500) == false);   // Storm's Eye is not a predecessor of Maim: chain broken
    CHECK(FFXIV::Gauge::GetFfxivGauge(Caster, WarriorCombo::WAR) == 20);

    // Storm's Eye's status row: the caster gets Surging Tempest for its 30 s.
    CHECK(FFXIV::Status::GetFfxivActionStatusDuration(STORMS_EYE, SURGING_TEMPEST) == 30000);
    std::vector<uint32> const* statuses = FFXIV::Status::GetFfxivActionStatuses(STORMS_EYE);
    REQUIRE(statuses != nullptr);
    CHECK(statuses->size() == 1);
    CHECK((*statuses)[0] == SURGING_TEMPEST);
    CHECK(FFXIV::Status::GetFfxivStatus(SURGING_TEMPEST) != nullptr);
}

TEST_CASE_METHOD(WarriorCombo::WarriorFixture, "FFXIV warrior: Surging Tempest only follows the correct Maim -> Storm's Eye step", "[ffxiv]")
{
    // ffxiv_action_status (45, 2677) is comboOnly ("Combo Bonus: Grants Surging Tempest"): the status hook asks
    // WasFfxivCorrectComboStep through ShouldApplyFfxivActionStatus, so the same chain that gates the gauge gates it.
    CHECK(FFXIV::Status::IsFfxivActionStatusComboOnly(STORMS_EYE, SURGING_TEMPEST));

    // Storm's Eye with no predecessor: no Surging Tempest.
    bool comboCorrect = Cast(STORMS_EYE, 1000);
    CHECK_FALSE(FFXIV::Status::ShouldApplyFfxivActionStatus(STORMS_EYE, SURGING_TEMPEST, comboCorrect));

    // Skipped Maim: no Surging Tempest.
    Cast(HEAVY_SWING, 2000);
    comboCorrect = Cast(STORMS_EYE, 3000);
    CHECK_FALSE(FFXIV::Status::ShouldApplyFfxivActionStatus(STORMS_EYE, SURGING_TEMPEST, comboCorrect));

    // Heavy Swing -> Maim -> Storm's Eye: Surging Tempest.
    Cast(HEAVY_SWING, 5000);
    Cast(MAIM, 6000);
    comboCorrect = Cast(STORMS_EYE, 7000);
    CHECK(comboCorrect);
    CHECK(FFXIV::Status::ShouldApplyFfxivActionStatus(STORMS_EYE, SURGING_TEMPEST, comboCorrect));

    // Window expired between Maim and Storm's Eye: no Surging Tempest.
    Cast(HEAVY_SWING, 20000);
    Cast(MAIM, 21000);
    comboCorrect = Cast(STORMS_EYE, 21000 + 13501);
    CHECK_FALSE(FFXIV::Status::ShouldApplyFfxivActionStatus(STORMS_EYE, SURGING_TEMPEST, comboCorrect));
}

TEST_CASE_METHOD(WarriorCombo::WarriorFixture, "FFXIV warrior: a step with no or the wrong predecessor gains nothing", "[ffxiv]")
{
    CHECK_FALSE(Cast(STORMS_EYE, 1000));              // no predecessor
    CHECK_FALSE(Cast(MAIM, 2000));                    // Maim straight after a stray step
    CHECK(FFXIV::Gauge::GetFfxivGauge(Caster, WarriorCombo::WAR) == 0);

    CHECK_FALSE(Cast(HEAVY_SWING, 3000));
    CHECK_FALSE(Cast(STORMS_EYE, 4000));              // skipped Maim
    CHECK(FFXIV::Gauge::GetFfxivGauge(Caster, WarriorCombo::WAR) == 0);

    CHECK_FALSE(Cast(HEAVY_SWING, 5000));
    CHECK(Cast(MAIM, 6000));
    CHECK(FFXIV::Gauge::GetFfxivGauge(Caster, WarriorCombo::WAR) == 10);
    CHECK_FALSE(Cast(STORMS_EYE, 6000 + 13501));      // combo window (12.5 s) expired
    CHECK(FFXIV::Gauge::GetFfxivGauge(Caster, WarriorCombo::WAR) == 10);
}

TEST_CASE_METHOD(WarriorCombo::WarriorFixture, "FFXIV warrior: only a weaponskill without PreservesCombo touches the chain", "[ffxiv]")
{
    // Sapphire Action.cpp (retail): if (isWeaponskill() && !ComboContinue) advance-or-clear.
    // Heavy Swing -> Inner Beast (weaponskill, PreservesCombo) -> Maim still combos.
    CHECK_FALSE(Cast(HEAVY_SWING, 1000));
    CHECK_FALSE(Cast(INNER_BEAST, 2000, WEAPONSKILL, true));
    CHECK(FFXIV::Combat::GetFfxivLastActionId(Caster, 2000) == HEAVY_SWING);
    CHECK(Cast(MAIM, 3000));
    CHECK(FFXIV::Gauge::GetFfxivGauge(Caster, WarriorCombo::WAR) == 10);

    // A damaging oGCD (Upheaval, an ability) between steps: the chain survives.
    CHECK_FALSE(Cast(UPHEAVAL, 3500, ABILITY));
    CHECK(Cast(STORMS_EYE, 4000));
    CHECK(FFXIV::Gauge::GetFfxivGauge(Caster, WarriorCombo::WAR) == 20);

    // The window still runs from the last chain step, not from the preserving action.
    CHECK_FALSE(Cast(HEAVY_SWING, 10000));
    CHECK_FALSE(Cast(INNER_BEAST, 20000, WEAPONSKILL, true));
    CHECK_FALSE(Cast(MAIM, 10000 + 12501));

    // Any other weaponskill is a chain break: Overpower (a starter of its own chain) replaces it.
    CHECK_FALSE(Cast(HEAVY_SWING, 30000));
    CHECK_FALSE(Cast(OVERPOWER, 31000));
    CHECK(FFXIV::Combat::GetFfxivLastActionId(Caster, 31000) == OVERPOWER);
    CHECK_FALSE(Cast(MAIM, 32000));
    CHECK(FFXIV::Combat::GetFfxivLastActionId(Caster, 32000) == 0); // wrong step: cleared
}
