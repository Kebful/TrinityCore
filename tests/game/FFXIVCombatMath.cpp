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
 * Goldens for the published FFXIV combat pipeline (FFXIV/Combat).
 *
 * Every expectation below is arithmetic on B-FORMULA-REFERENCE's published
 * stages, recomputed independently (a Python reference, not this code) and then
 * transcribed with the intermediate values in the comment. No vector is a
 * range: each one pins an integer.
 *
 * Level constants used throughout (FFXIVStatTables, the same M/S/D the engine
 * reads): L50 202/341/341, L80 340/380/1300, L90 390/400/1900, L100 440/420/2780.
 *
 * NOTE on magnitude: the second stage's published `/ 100` (B §F23.5) is applied
 * literally, so an L100 100-potency hit is ~47, not ~4700. FFXIVCombatMath.h
 * explains the ambiguity; the goldens pin the literal reading so any change is
 * visible here.
 *
 * Rolls: `FfxivFixedRollSource(99, 100)` never crits or direct-hits (99*100 is
 * above every chance), `(0, 100)` forces both on when the chance is non-zero, and
 * `(5, 100)` forces only crit when the crit chance is 5.1 % (500 < 510, 500 >= 40).
 */

#include "tc_catch2.h"

#include "FFXIV/Combat/FFXIVCombatMath.h"
#include "FFXIV/Combat/FFXIVCombatTables.h"
#include "FFXIV/Stats/FFXIVStatEngine.h"
#include "FFXIV/Stats/FFXIVStatTables.h"

#include <array>
#include <vector>

using namespace FFXIV::Combat;
using namespace FFXIV::Stats;

namespace
{
constexpr uint8 WHM = 24;
constexpr uint8 PLD = 19;
constexpr uint8 MNK = 20;
constexpr uint8 BRD = 23;
constexpr uint8 BLM = 25;
constexpr uint8 BLU = 36;

FfxivPartyComposition Party(std::initializer_list<FfxivPartyRole> roles)
{
    FfxivPartyComposition party;
    for (FfxivPartyRole role : roles)
        party = party.With(role);
    return party;
}

FfxivPartyComposition AllRoles() { return FfxivPartyComposition(0x1F); }

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

// A geared snapshot: `mainStat` is the raw attack attribute (no party bonus -- the
// pipeline applies that), the secondaries go through the engine so rate.* is the
// real slice-1 output. HMP (BaseParam 34) is set on the snapshot afterwards
// because slice 1 does not carry it.
FfxivDerivedStats Geared(uint8 jobId, uint8 level, int32 mainStat, int32 crit, int32 directHit,
                         int32 determination, int32 tenacity, int32 skillSpeed, int32 spellSpeed,
                         int32 healingMagicPotency = 0)
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
    inputs.skillSpeed = skillSpeed;
    inputs.spellSpeed = spellSpeed;
    inputs.equipmentParams[attribute] = mainStat - base;

    FfxivDerivedStats stats = ComputeFfxivStats(inputs);
    stats.param[FFXIV_PARAM_HEALING_MAGIC_POTENCY] = healingMagicPotency;
    return stats;
}

// The WHM fixture of the L100 goldens: MND 4000, WD 132, CRIT/DH/DET/TNC/SPS 442.
FfxivDerivedStats Whm100(int32 determination = 442)
{
    return Geared(WHM, 100, 4000, 442, 442, determination, 442, 442, 442);
}

FfxivTraitState Traits() { return FfxivTraitState{}; }

int StageIndex(FfxivCombatTrace const& trace, FfxivCombatStageId id)
{
    for (uint8 i = 0; i < trace.count; ++i)
        if (trace.stages[i].id == id)
            return int(i);
    return -1;
}

int64 StageValue(FfxivCombatTrace const& trace, FfxivCombatStageId id)
{
    int64 const* value = trace.Find(id);
    return value ? *value : -1;
}
} // namespace

TEST_CASE("FFXIV f(AP) table: anchors, interpolation and provenance", "[ffxiv]")
{
    // B §F23.3/§F24.1/§F24.7 and data/ffxiv-combat-constants-20261123.tsv: the
    // published coefficient series. L100 is the one point two implementations
    // agree on ("190 tank / 237 other").
    uint8 provenance = 0;
    CHECK(GetFfxivAttackPowerCoefficient(50, false, &provenance) == 75);
    CHECK(provenance == FFXIV_COMBAT_AP_SOURCED);
    CHECK(GetFfxivAttackPowerCoefficient(60, false) == 114);
    CHECK(GetFfxivAttackPowerCoefficient(70, false) == 125);
    CHECK(GetFfxivAttackPowerCoefficient(80, false) == 165);
    CHECK(GetFfxivAttackPowerCoefficient(90, false) == 195);
    CHECK(GetFfxivAttackPowerCoefficient(100, false, &provenance) == 237);
    CHECK(provenance == FFXIV_COMBAT_AP_SOURCED);

    CHECK(GetFfxivAttackPowerCoefficient(50, true) == 56);
    CHECK(GetFfxivAttackPowerCoefficient(60, true) == 91);
    CHECK(GetFfxivAttackPowerCoefficient(70, true) == 105);
    CHECK(GetFfxivAttackPowerCoefficient(80, true) == 115);
    CHECK(GetFfxivAttackPowerCoefficient(90, true) == 156);
    CHECK(GetFfxivAttackPowerCoefficient(100, true) == 190);

    // Levels 1..50 use the published flat band for the non-tank series; the tank
    // series has no published value below 50 and holds the L50 anchor (flagged).
    CHECK(GetFfxivAttackPowerCoefficient(1, false, &provenance) == 75);
    CHECK(provenance == FFXIV_COMBAT_AP_PUBLISHED_BAND);
    CHECK(GetFfxivAttackPowerCoefficient(30, false) == 75);
    CHECK(GetFfxivAttackPowerCoefficient(30, true, &provenance) == 56);
    CHECK(provenance == FFXIV_COMBAT_AP_HELD);

    // Linear, truncated toward zero: 50->60 is 75->114 over 10 levels, so L55 is
    // 75 + trunc(39*5/10) = 75 + 19 = 94. Tank 50->60 is 56->91: 56 + trunc(35*5/10) = 73.
    CHECK(GetFfxivAttackPowerCoefficient(55, false, &provenance) == 94);
    CHECK(provenance == FFXIV_COMBAT_AP_INTERPOLATED);
    CHECK(GetFfxivAttackPowerCoefficient(55, true) == 73);
    // 60->70 is 114->125: L65 = 114 + trunc(11*5/10) = 114 + 5 = 119 (tank 98).
    CHECK(GetFfxivAttackPowerCoefficient(65, false) == 119);
    CHECK(GetFfxivAttackPowerCoefficient(65, true) == 98);
    // 80->90 is 165->195: L85 = 165 + trunc(30*5/10) = 180 (tank 115 + trunc(41*5/10) = 135).
    CHECK(GetFfxivAttackPowerCoefficient(85, false) == 180);
    CHECK(GetFfxivAttackPowerCoefficient(85, true) == 135);

    // f(HMP) divisor: published only at Lv70/Lv80 (B §F23.4); elsewhere held.
    CHECK(GetFfxivHealingPotencyDivisor(70, &provenance) == 264);
    CHECK(provenance == FFXIV_COMBAT_HMP_SOURCED);
    CHECK(GetFfxivHealingPotencyDivisor(80, &provenance) == 304);
    CHECK(provenance == FFXIV_COMBAT_HMP_SOURCED);
    CHECK(GetFfxivHealingPotencyDivisor(50, &provenance) == 264);
    CHECK(provenance == FFXIV_COMBAT_HMP_HELD);
    CHECK(GetFfxivHealingPotencyDivisor(90) == 304);
    CHECK(GetFfxivHealingPotencyDivisor(100) == 304);
}

TEST_CASE("FFXIV job combat identity: attribute, trait, auto potency, DoT branch", "[ffxiv]")
{
    // B §F23.13: the attribute is a job property, not a damage-type property.
    CHECK(GetFfxivAttackPowerAttribute(WHM) == FFXIV_PARAM_MIND);
    CHECK(GetFfxivAttackPowerAttribute(BRD) == FFXIV_PARAM_DEXTERITY);
    CHECK(GetFfxivAttackPowerAttribute(BLM) == FFXIV_PARAM_INTELLIGENCE);
    CHECK(GetFfxivAttackPowerAttribute(MNK) == FFXIV_PARAM_STRENGTH);
    CHECK(GetFfxivAttackPowerAttribute(PLD) == FFXIV_PARAM_STRENGTH);

    // B §F24.2 / I §9.5
    CHECK(IsFfxivTankJob(PLD));
    CHECK_FALSE(IsFfxivTankJob(WHM));
    CHECK(ComputeFfxivDamageTraitPercent(WHM) == 130); // healer
    CHECK(ComputeFfxivDamageTraitPercent(BLM) == 130); // caster
    CHECK(ComputeFfxivDamageTraitPercent(BRD) == 120); // physical ranged
    CHECK(ComputeFfxivDamageTraitPercent(MNK) == 100); // melee
    CHECK(ComputeFfxivDamageTraitPercent(PLD) == 100); // tank
    CHECK(ComputeFfxivDamageTraitPercent(BLU) == 150); // BLU exception

    // B §F24.2: 90 melee (incl. healer/caster/DNC) / 80 ranged.
    CHECK(ComputeFfxivAutoAttackPotency(MNK) == 90);
    CHECK(ComputeFfxivAutoAttackPotency(WHM) == 90);
    CHECK(ComputeFfxivAutoAttackPotency(BLM) == 90);
    CHECK(ComputeFfxivAutoAttackPotency(BRD) == 80);
    CHECK(ComputeFfxivAutoAttackPotency(31) == 80); // MCH

    // B §F23.7: DoW jobs tick physical, DoM jobs magical (job-derived default).
    CHECK(GetFfxivDotType(MNK) == FFXIV_DOT_PHYSICAL);
    CHECK(GetFfxivDotType(WHM) == FFXIV_DOT_MAGICAL);
    CHECK(GetFfxivDotType(BLM) == FFXIV_DOT_MAGICAL);

    // Party roles for the +1 %/role bonus: ClassJob role 3 covers casters and
    // physical ranged alike, so the job type has to separate them.
    FfxivPartyRole role = FfxivPartyRole::Count;
    CHECK(GetFfxivPartyRole(PLD, role));
    CHECK(role == FfxivPartyRole::Tank);
    CHECK(GetFfxivPartyRole(WHM, role));
    CHECK(role == FfxivPartyRole::Healer);
    CHECK(GetFfxivPartyRole(MNK, role));
    CHECK(role == FfxivPartyRole::Melee);
    CHECK(GetFfxivPartyRole(BRD, role));
    CHECK(role == FfxivPartyRole::PhysicalRanged);
    CHECK(GetFfxivPartyRole(BLM, role));
    CHECK(role == FfxivPartyRole::Caster);
    // Crafters/gatherers and the neutral row are not party roles.
    CHECK_FALSE(GetFfxivPartyRole(8, role));  // CRP
    CHECK_FALSE(GetFfxivPartyRole(16, role)); // MIN
    CHECK_FALSE(GetFfxivPartyRole(0, role));

    FfxivPartyComposition party;
    for (uint8 jobId : { PLD, WHM, MNK, BRD, BLM })
    {
        CHECK(GetFfxivPartyRole(jobId, role));
        party = party.With(role);
    }
    CHECK(party.UniqueRoleCount() == 5);
    CHECK(party.BonusPercent() == 105);
}

TEST_CASE("FFXIV stat terms: f(ATK), f(WD), f(SPD), f(HMP), level correction", "[ffxiv]")
{
    FfxivDerivedStats const stats = Whm100();

    // f(ATK) = trunc(237*(4000-440)/440) + 100 = trunc(1917.5...) + 100 = 2017.
    CHECK(ComputeFfxivAttackPowerPercent(stats, 4000, false) == 2017);
    // Tank series at L100 is 190: trunc(190*3560/440) = 1537 + 100 = 1637.
    CHECK(ComputeFfxivAttackPowerPercent(stats, 4000, true) == 1637);
    // Neutral at ATK == MAIN, the level base main stat (440 at L100).
    CHECK(ComputeFfxivAttackPowerPercent(stats, 440, false) == 100);

    // f(WD) = floor(440*115/1000) + 132 = 50 + 132 = 182 for WHM (MND 115 %);
    // a MNK (STR 110 %) with the same weapon damage is floor(48.4) + 132 = 180.
    CHECK(ComputeFfxivWeaponDamagePercent(stats, 132) == 182);
    CHECK(ComputeFfxivWeaponDamagePercent(Geared(MNK, 100, 4000, 442, 442, 442, 442, 442, 442), 132) == 180);
    // B §F14 auto: floor(f(WD) * delay / 3000).
    CHECK(ComputeFfxivAutoAttackWeaponDamagePercent(
              Geared(MNK, 100, 4000, 442, 442, 442, 442, 442, 442), 132, 2500) == 150);
    CHECK(ComputeFfxivAutoAttackWeaponDamagePercent(
              Geared(MNK, 100, 4000, 442, 442, 442, 442, 442, 442), 132, 3000) == 180);

    // f(SPD) at L100 SPS 442: 1000 + trunc(130*22/2780) = 1001.
    CHECK(ComputeFfxivSpeedPermille(stats, FFXIV_DOT_MAGICAL) == 1001);
    // Below the level base S the term is negative. B §F13 writes fl(), B §B.0
    // says negative speed intermediates keep signed truncation; this follows
    // B §B.0, so SPS 400 gives trunc(-0.93) = 0 and f(SPD) stays 1000 (fl would
    // give 999). The one-point difference is exactly the ambiguity B §B.0 resolves.
    FfxivDerivedStats const lowSpeed = Geared(WHM, 100, 4000, 442, 442, 442, 442, 442, 400);
    CHECK(ComputeFfxivSpeedPermille(lowSpeed, FFXIV_DOT_MAGICAL) == 1000);;

    // f(HMP) = floor(100*(3000-440)/304) + 100 = floor(842.1) + 100 = 942.
    CHECK(ComputeFfxivHealingMagicPotencyPercent(stats, 3000) == 942);
    CHECK(ComputeFfxivHealingMagicPotencyPercent(stats, 440) == 100);
    // L50 uses the held 264 divisor: floor(100*(1000-202)/264) = 302 -> 402.
    CHECK(ComputeFfxivHealingMagicPotencyPercent(Geared(WHM, 50, 1000, 440, 440, 440, 341, 341, 341),
                                                 1000) == 402);

    // B §F23.5 level correction: -2.5 % per attacker level below the target.
    CHECK(ComputeFfxivLevelCorrectionPermille(100, 100) == 1000);
    CHECK(ComputeFfxivLevelCorrectionPermille(100, 99) == 1000);
    CHECK(ComputeFfxivLevelCorrectionPermille(80, 82) == 950);
    CHECK(ComputeFfxivLevelCorrectionPermille(90, 100) == 750);
    CHECK(ComputeFfxivLevelCorrectionPermille(80, 100) == 500);
    CHECK(ComputeFfxivLevelCorrectionPermille(50, 100) == 0); // clamped at zero
}

TEST_CASE("FFXIV party bonus: +1 % per unique role, 0..5", "[ffxiv]")
{
    CHECK(FfxivPartyComposition().UniqueRoleCount() == 0);
    CHECK(FfxivPartyComposition().BonusPercent() == 100);
    CHECK(Party({ FfxivPartyRole::Tank }).BonusPercent() == 101);
    CHECK(Party({ FfxivPartyRole::Tank, FfxivPartyRole::Healer }).BonusPercent() == 102);
    CHECK(AllRoles().UniqueRoleCount() == 5);
    CHECK(AllRoles().BonusPercent() == 105);
    // Bits above the five roles are ignored (pets are not roles).
    CHECK(FfxivPartyComposition(0xFF).UniqueRoleCount() == 5);

    // Applied to the main stat with one floor: floor(4000*105/100) = 4200.
    CHECK(ComputeFfxivPartyBoostedMainStat(4000, AllRoles()) == 4200);
    CHECK(ComputeFfxivPartyBoostedMainStat(4000, FfxivPartyComposition()) == 4000);
    // floor(4001*102/100) = 4081 (4081.02)
    CHECK(ComputeFfxivPartyBoostedMainStat(4001, Party({ FfxivPartyRole::Tank, FfxivPartyRole::Melee })) == 4081);
}

TEST_CASE("FFXIV L100 direct damage: published stage values", "[ffxiv]")
{
    FfxivDerivedStats const stats = Whm100();
    FfxivFixedRollSource rolls(99, 100); // no crit, no direct hit, variance 100

    SECTION("100 potency, baseline DET (the worked example)")
    {
        FfxivDirectDamageResult const r =
            ComputeFfxivDirectDamage(stats, 100, 182, 100, FfxivPartyComposition(), Traits(), rolls);

        // f(ATK)=2017, f(DET)=1000, f(TNC)=1000, f(WD)=182, trait=130
        // D1 = floor(100*2017*1000/100)/1000 = 2 017 000/1000 = 2017
        CHECK(StageValue(r.trace, FfxivCombatStageId::AttackPowerPercent) == 2017);
        CHECK(StageValue(r.trace, FfxivCombatStageId::DeterminationPermille) == 1000);
        CHECK(StageValue(r.trace, FfxivCombatStageId::BaseDamage) == 2017);
        CHECK(StageValue(r.trace, FfxivCombatStageId::TenacityPermille) == 1000);
        CHECK(StageValue(r.trace, FfxivCombatStageId::WeaponDamagePercent) == 182);
        CHECK(StageValue(r.trace, FfxivCombatStageId::TraitPercent) == 130);
        // D2 = floor(2017*1000*182*130 / 10 000 000)/100 = 4772/100 = 47
        CHECK(StageValue(r.trace, FfxivCombatStageId::Stage2Damage) == 47);
        CHECK(StageValue(r.trace, FfxivCombatStageId::Stage3Damage) == 47);
        CHECK(StageValue(r.trace, FfxivCombatStageId::LevelCorrectionPermille) == 1000);
        CHECK(r.value == 47);
        CHECK_FALSE(r.crit);
        CHECK_FALSE(r.directHit);
        CHECK(r.critChanceBp == 510); // F20: 5.1 % at CRIT 442
        CHECK(r.dhChanceBp == 40);    // F20: 0.4 % at DH 442
    }

    SECTION("crit only: roll 5 is under the 5.1 % crit chance and over the 0.4 % DH chance")
    {
        FfxivFixedRollSource critOnly(5, 100);
        FfxivDirectDamageResult const r =
            ComputeFfxivDirectDamage(stats, 100, 182, 100, FfxivPartyComposition(), Traits(), critOnly);
        CHECK(r.crit);
        CHECK_FALSE(r.directHit);
        CHECK(StageValue(r.trace, FfxivCombatStageId::CriticalMultiplierPermille) == 1401);
        // D3 = floor(47*1401*100 / (1000*100)) = floor(65.847) = 65
        CHECK(StageValue(r.trace, FfxivCombatStageId::Stage3Damage) == 65);
        CHECK(r.value == 65);
    }

    SECTION("crit and direct hit forced on")
    {
        FfxivFixedRollSource both(0, 100);
        FfxivDirectDamageResult const r =
            ComputeFfxivDirectDamage(stats, 100, 182, 100, FfxivPartyComposition(), Traits(), both);
        CHECK(r.crit);
        CHECK(r.directHit);
        CHECK(StageValue(r.trace, FfxivCombatStageId::DirectHitMultiplierPercent) == 125);
        // D3 = floor(47*1401*125 / (1000*100)) = floor(82.3) = 82
        CHECK(r.value == 82);
    }

    SECTION("determination +978 (DET 1420 -> f(DET) = 1000 + floor(140*980/2780) = 1049)")
    {
        FfxivDerivedStats const detStats = Whm100(1420);
        CHECK(detStats.rate.detBp == 10490);
        FfxivDirectDamageResult const r =
            ComputeFfxivDirectDamage(detStats, 100, 182, 100, FfxivPartyComposition(), Traits(), rolls);
        // D1 = floor(100*2017*1049/100)/1000 = 2 115 833/1000 = 2115
        CHECK(StageValue(r.trace, FfxivCombatStageId::BaseDamage) == 2115);
        // D2 = floor(2115*1000*182*130/10 000 000)/100 = 5004/100 = 50
        CHECK(r.value == 50);

        SECTION("potency 180")
        {
            FfxivDirectDamageResult const p180 =
                ComputeFfxivDirectDamage(detStats, 180, 182, 100, FfxivPartyComposition(), Traits(), rolls);
            // D1 = floor(180*2017*1049/100)/1000 = 3 808 499/1000 = 3808
            CHECK(StageValue(p180.trace, FfxivCombatStageId::BaseDamage) == 3808);
            // D2 = floor(3808*1000*182*130/10 000 000)/100 = 9009/100 = 90
            CHECK(p180.value == 90);
        }

        SECTION("potency 500 with crit and DH")
        {
            FfxivFixedRollSource both(0, 100);
            FfxivDirectDamageResult const p500 =
                ComputeFfxivDirectDamage(detStats, 500, 182, 100, FfxivPartyComposition(), Traits(), both);
            // D1 = floor(500*2017*1049/100)/1000 = 10 579 165/1000 = 10579
            CHECK(StageValue(p500.trace, FfxivCombatStageId::BaseDamage) == 10579);
            // D2 = floor(10579*1000*182*130/10 000 000)/100 = 25 029/100 = 250
            CHECK(StageValue(p500.trace, FfxivCombatStageId::Stage2Damage) == 250);
            // D3 = floor(250*1401*125/(1000*100)) = floor(437.8) = 437
            CHECK(p500.value == 437);
        }
    }

    SECTION("variance at both boundaries of rand[95,105]")
    {
        FfxivFixedRollSource low(99, 95);
        FfxivFixedRollSource high(99, 105);
        // floor(47*95/100) = 44
        CHECK(ComputeFfxivDirectDamage(stats, 100, 182, 100, FfxivPartyComposition(), Traits(), low).value == 44);
        // floor(47*105/100) = 49
        CHECK(ComputeFfxivDirectDamage(stats, 100, 182, 100, FfxivPartyComposition(), Traits(), high).value
              == 49);
    }

    SECTION("the trait is a switch, and buffs stack with one floor each")
    {
        FfxivFixedRollSource plain(99, 100);
        FfxivTraitState const noTrait{ false };
        // trait 100 instead of 130: D2 = floor(2017*1000*182*100/10 000 000)/100 = 3670/100 = 36
        CHECK(ComputeFfxivDirectDamage(stats, 100, 182, 100, FfxivPartyComposition(), noTrait, plain).value
              == 36);

        std::array<FfxivPercentFactor, 2> const buffs{ { { 130, 100 }, { 130, 100 } } };
        FfxivDirectDamageResult const buffed =
            ComputeFfxivDirectDamage(Whm100(1420), 100, 182, 100, FfxivPartyComposition(), Traits(), plain,
                                     buffs);
        // floor(50*130/100) = 65, then floor(65*130/100) = 84 (1.69 * 50 = 84.5 -> 84)
        CHECK(buffed.value == 84);
    }
}

TEST_CASE("FFXIV L80 tank: tenacity, level correction and high crit chance", "[ffxiv]")
{
    // PLD L80: STR 3000, WD 122, all secondaries 1500 except TNC 1000 (S is 380).
    // f(ATK) = trunc(115*(3000-340)/340) + 100 = 899 + 100 = 999
    // f(WD)   = floor(340*100/1000) + 122 = 156
    // f(DET)  = 1000 + floor(140*(1500-340)/1300) = 1124
    // f(TNC)  = 1000 + floor(112*(1000-380)/1300) = 1053
    FfxivDerivedStats const tank = Geared(PLD, 80, 3000, 1500, 1500, 1500, 1000, 1500, 1500);
    CHECK(tank.rate.detBp == 11240);
    CHECK(tank.rate.tncOutBp == 10530);
    CHECK(tank.rate.critBp == 2220); // 22.2 %
    CHECK(tank.rate.dhBp == 4730);   // 47.3 %

    FfxivFixedRollSource rolls(99, 100);
    FfxivDirectDamageResult const r =
        ComputeFfxivDirectDamage(tank, 180, 156, 80, FfxivPartyComposition(), Traits(), rolls);
    // D1 = floor(180*999*1124/100)/1000 = 2 021 176/1000 = 2021
    CHECK(StageValue(r.trace, FfxivCombatStageId::BaseDamage) == 2021);
    // D2 = floor(2021*1053*156*100/10 000 000)/100 = 3319/100 = 33
    CHECK(StageValue(r.trace, FfxivCombatStageId::Stage2Damage) == 33);
    CHECK(r.value == 33);
    CHECK_FALSE(r.crit); // roll 99 * 100 = 9900 >= 2220

    // Even at a 22.2 % chance the fixed 99 roll cannot crit -- the pin the tests rely on.
    FfxivFixedRollSource forced(0, 100);
    FfxivDirectDamageResult const crit =
        ComputeFfxivDirectDamage(tank, 180, 156, 80, FfxivPartyComposition(), Traits(), forced);
    CHECK(crit.crit);
    CHECK(crit.directHit);
    // D3 = floor(33*1572*125/(1000*100)) = floor(64.8) = 64
    CHECK(crit.value == 64);

    // Attacker 80 against target 82: correction 1000 - 25*2 = 950; floor(33*950/1000) = 31.
    FfxivDirectDamageResult const corrected =
        ComputeFfxivDirectDamage(tank, 180, 156, 82, FfxivPartyComposition(), Traits(), rolls);
    CHECK(StageValue(corrected.trace, FfxivCombatStageId::LevelCorrectionPermille) == 950);
    CHECK(corrected.value == 31);

    // Non-tank at L80 with TNC at the level base S: neutral tenacity, no role gate
    // is needed because slice 1 gives a non-tank TNC == S.
    FfxivDerivedStats const healer = Geared(WHM, 80, 3000, 442, 442, 442, 380, 442, 442);
    CHECK(healer.rate.tncOutBp == 10000);
    FfxivDirectDamageResult const healerHit =
        ComputeFfxivDirectDamage(healer, 180, 156, 80, FfxivPartyComposition(), Traits(), rolls);
    CHECK(StageValue(healerHit.trace, FfxivCombatStageId::TenacityPermille) == 1000);
}

TEST_CASE("FFXIV direct heal: Cure/Medica at L50, L90 and L100", "[ffxiv]")
{
    SECTION("L100 WHM: Cure 450, HMP 3000, WD 132")
    {
        // f(HMP) = floor(100*2560/304) + 100 = 942; f(DET) = 1000; f(TNC) = 1000;
        // f(WD) = 182; trait = 130.
        FfxivDerivedStats const stats = Geared(WHM, 100, 4000, 442, 442, 442, 442, 442, 442, 3000);
        FfxivFixedRollSource rolls(99, 100);
        FfxivHealResult const cure =
            ComputeFfxivHeal(stats, 450, 182, 100, FfxivPartyComposition(), Traits(), rolls);
        // H1 = floor(450*942*1000/100)/1000 = 4 239 000/1000 = 4239
        CHECK(StageValue(cure.trace, FfxivCombatStageId::BaseDamage) == 4239);
        // H2 = floor(4239*1000*182*130/10 000 000)/100 = 10 029/100 = 100
        CHECK(StageValue(cure.trace, FfxivCombatStageId::Stage2Damage) == 100);
        CHECK(cure.value == 100);
        CHECK_FALSE(cure.crit);

        // No Direct Hit term anywhere in healing (B §F10.1/F16.1).
        CHECK(StageIndex(cure.trace, FfxivCombatStageId::DirectHitRoll) == -1);
        CHECK(StageIndex(cure.trace, FfxivCombatStageId::DirectHitMultiplierPercent) == -1);

        FfxivFixedRollSource forcedCrit(0, 100);
        FfxivHealResult const crit =
            ComputeFfxivHeal(stats, 450, 182, 100, FfxivPartyComposition(), Traits(), forcedCrit);
        CHECK(crit.crit);
        // H3 = floor(100*1401/1000) = 140
        CHECK(crit.value == 140);

        FfxivHealResult const medica =
            ComputeFfxivHeal(stats, 300, 182, 100, FfxivPartyComposition(), Traits(), rolls);
        // H1 = floor(300*942*1000/100)/1000 = 2826; H2 = floor(2826*...)/100 = 66
        CHECK(StageValue(medica.trace, FfxivCombatStageId::BaseDamage) == 2826);
        CHECK(medica.value == 66);

        // Healing variance is +-3 %: floor(100*97/100) = 97 and floor(100*103/100) = 103.
        FfxivFixedRollSource low(99, 97);
        FfxivFixedRollSource high(99, 103);
        CHECK(ComputeFfxivHeal(stats, 450, 182, 100, FfxivPartyComposition(), Traits(), low).value == 97);
        CHECK(ComputeFfxivHeal(stats, 450, 182, 100, FfxivPartyComposition(), Traits(), high).value == 103);

        // The party bonus reaches healing through HMP (INFERRED: B §F24.2 names the
        // main stat; HMP is the heal chain's main-stat term). Five roles:
        // floor(3000*105/100) = 3150 -> f(HMP) = floor(100*2710/304)+100 = 991
        // H1 = floor(450*991*1000/100)/1000 = 4459; H2 = 10549/100 = 105.
        FfxivHealResult const boosted =
            ComputeFfxivHeal(stats, 450, 182, 100, AllRoles(), Traits(), rolls);
        CHECK(StageValue(boosted.trace, FfxivCombatStageId::HealingMagicPotencyPercent) == 991);
        CHECK(boosted.value == 105);
    }

    SECTION("job 24 at level 50: MND 1000, HMP 1000, WD 60, DET/CRIT 440")
    {
        // f(HMP) at the held 264 divisor = 402; f(WD) = floor(202*115/1000)+60 = 83;
        // f(DET) = 1000 + floor(140*238/341) = 1097; trait 130.
        FfxivDerivedStats const stats = Geared(WHM, 50, 1000, 440, 440, 440, 341, 341, 341, 1000);
        CHECK(stats.rate.detBp == 10970);
        FfxivFixedRollSource rolls(99, 100);
        FfxivHealResult const cure =
            ComputeFfxivHeal(stats, 450, 83, 50, FfxivPartyComposition(), Traits(), rolls);
        // H1 = floor(450*402*1097/100)/1000 = 1 984 473/1000 = 1984
        CHECK(StageValue(cure.trace, FfxivCombatStageId::BaseDamage) == 1984);
        // H2 = floor(1984*1000*83*130/10 000 000)/100 = 2140/100 = 21
        CHECK(cure.value == 21);

        FfxivHealResult const medica =
            ComputeFfxivHeal(stats, 300, 83, 50, FfxivPartyComposition(), Traits(), rolls);
        // H1 = floor(300*402*1097/100)/1000 = 1322; H2 = 1426/100 = 14
        CHECK(StageValue(medica.trace, FfxivCombatStageId::BaseDamage) == 1322);
        CHECK(medica.value == 14);
    }

    SECTION("job 24 at level 90: MND 3000, HMP 2500, WD 130 (held 304 divisor)")
    {
        // f(HMP) = floor(100*2110/304) + 100 = 794; f(WD) = floor(390*115/1000)+130 = 174;
        // f(DET) = 1000 + floor(140*52/1900) = 1003; trait 130.
        FfxivDerivedStats const stats = Geared(WHM, 90, 3000, 442, 442, 442, 400, 400, 400, 2500);
        CHECK(stats.rate.detBp == 10030);
        FfxivFixedRollSource rolls(99, 100);
        FfxivHealResult const cure =
            ComputeFfxivHeal(stats, 450, 174, 90, FfxivPartyComposition(), Traits(), rolls);
        // H1 = floor(450*794*1003/100)/1000 = 3 583 719/1000 = 3583
        CHECK(StageValue(cure.trace, FfxivCombatStageId::BaseDamage) == 3583);
        // H2 = floor(3583*1000*174*130/10 000 000)/100 = 8104/100 = 81
        CHECK(cure.value == 81);
    }
}

TEST_CASE("FFXIV DoT tick: physical and magical published orders", "[ffxiv]")
{
    SECTION("physical DoT (job-derived branch from a DoW job)")
    {
        // MNK L100: STR 4000, WD 132 -> f(WD) = 180; f(SPD) = 1001 (SKS 442);
        // f(ATK) = 2017; f(DET) = 1000; f(TNC) = 1000; trait 100.
        FfxivDerivedStats const stats = Geared(MNK, 100, 4000, 442, 442, 442, 442, 442, 442);
        CHECK(GetFfxivDotType(MNK) == FFXIV_DOT_PHYSICAL);
        FfxivFixedRollSource rolls(99, 100);
        FfxivCombatTrace trace;
        // D1 = floor(100*2017*1000/100)/1000 = 2017
        // D2 = floor(2017*1000*1001*180*100/10^10)/100 + 1 = 36 + 1 = 37
        int64 const tick =
            ComputeFfxivDotTick(stats, 100, 180, 100, FfxivPartyComposition(), Traits(), rolls, {}, &trace);
        CHECK(StageValue(trace, FfxivCombatStageId::BaseDamage) == 2017);
        CHECK(StageValue(trace, FfxivCombatStageId::SpeedPermille) == 1001);
        CHECK(StageValue(trace, FfxivCombatStageId::DotPlusOne) == 37);
        CHECK(tick == 37);
    }

    SECTION("magical DoT uses the other order and the WHM-130 trait")
    {
        // WHM L100 at baseline DET: f(WD) = 182, f(SPD) = 1001, f(ATK) = 2017.
        // Magical D1 = floor(100*182*2017*1001/(100*100*1000)) = floor(3674.6) = 3674
        // Magical D2 = floor(3674*1000*1000*130/(1000*1000*100))/100 + 1 = 47 + 1 = 48
        // Physical on the same numbers: D1 = 2017, D2 = 47 + 1 = 48.
        // The two published orders truncate at different points (D1 3674 vs 2017);
        // the final tick coincides on this fixture, which is exactly why the branch
        // has to come from the action's damage type and not be flattened.
        FfxivDerivedStats const stats = Whm100();
        CHECK(GetFfxivDotType(WHM) == FFXIV_DOT_MAGICAL);
        FfxivFixedRollSource rolls(99, 100);

        FfxivCombatTrace magicalTrace;
        int64 const magical = ComputeFfxivDotTickOfType(stats, 100, 182, FFXIV_DOT_MAGICAL, 100,
                                                        FfxivPartyComposition(), Traits(), rolls, {},
                                                        &magicalTrace);
        CHECK(StageValue(magicalTrace, FfxivCombatStageId::BaseDamage) == 3674);
        CHECK(magical == 48);

        FfxivCombatTrace physicalTrace;
        int64 const physical = ComputeFfxivDotTickOfType(stats, 100, 182, FFXIV_DOT_PHYSICAL, 100,
                                                         FfxivPartyComposition(), Traits(), rolls, {},
                                                         &physicalTrace);
        CHECK(StageValue(physicalTrace, FfxivCombatStageId::BaseDamage) == 2017);
        CHECK(physical == 48);
    }

    SECTION("speed enters the tick: SPS 420 -> f(SPD) 1000 vs SPS 442 -> 1001")
    {
        FfxivDerivedStats const neutral = Geared(WHM, 100, 4000, 442, 442, 1420, 442, 442, 420);
        FfxivDerivedStats const fast = Geared(WHM, 100, 4000, 442, 442, 1420, 442, 442, 442);
        CHECK(ComputeFfxivSpeedPermille(neutral, FFXIV_DOT_MAGICAL) == 1000);
        CHECK(ComputeFfxivSpeedPermille(fast, FFXIV_DOT_MAGICAL) == 1001);
        FfxivFixedRollSource rolls(99, 100);
        FfxivCombatTrace trace;
        // D1 = floor(100*182*2017*1000/(100*100*1000)) = 3670
        // D2 = floor(3670*1049*1000*130/10^8)/100 + 1 = 50 + 1 = 51
        CHECK(ComputeFfxivDotTickOfType(neutral, 100, 182, FFXIV_DOT_MAGICAL, 100, FfxivPartyComposition(),
                                        Traits(), rolls, {}, &trace) == 51);
        CHECK(StageValue(trace, FfxivCombatStageId::BaseDamage) == 3670);
    }
}

TEST_CASE("FFXIV auto-attack: 90 melee / 80 ranged, trait never applies", "[ffxiv]")
{
    SECTION("MNK L100, WD 132, 2.5 s delay")
    {
        // f(AUTO) = floor(floor(440*110/1000 + 132) * 2500/3000) = floor(180*0.8333) = 150
        // f(ATK) = 2017; f(DET) = 1000; potency 90; trait forced to 100.
        FfxivDerivedStats const stats = Geared(MNK, 100, 4000, 442, 442, 442, 442, 442, 442);
        FfxivFixedRollSource rolls(99, 100);
        FfxivDirectDamageResult const r =
            ComputeFfxivAutoAttackDamage(stats, 100, FfxivPartyComposition(), rolls, 132, 2500);
        CHECK(StageValue(r.trace, FfxivCombatStageId::AutoAttackPotency) == 90);
        CHECK(StageValue(r.trace, FfxivCombatStageId::WeaponDamagePercent) == 150);
        CHECK(StageValue(r.trace, FfxivCombatStageId::TraitPercent) == 100);
        // D1 = floor(90*2017*1000/100)/1000 = 1815
        CHECK(StageValue(r.trace, FfxivCombatStageId::BaseDamage) == 1815);
        // D2 = floor(1815*1000*150*100/10 000 000)/100 = 2722/100 = 27
        CHECK(r.value == 27);
    }

    SECTION("BRD L100, WD 132, 3 s delay: 80 potency and the 1.2 trait withheld")
    {
        // f(AUTO) = floor(182*3000/3000) = 182; potency 80.
        // D1 = floor(80*2017*1000/100)/1000 = 1613; D2 = floor(1613*1000*182*100/10^7)/100 = 29.
        // If the 1.2 ranged trait leaked in, D2 would be 35 -- the vector pins it out.
        FfxivDerivedStats const stats = Geared(BRD, 100, 4000, 442, 442, 442, 442, 442, 442);
        CHECK(ComputeFfxivDamageTraitPercent(BRD) == 120);
        FfxivFixedRollSource rolls(99, 100);
        FfxivDirectDamageResult const r =
            ComputeFfxivAutoAttackDamage(stats, 100, FfxivPartyComposition(), rolls, 132, 3000);
        CHECK(StageValue(r.trace, FfxivCombatStageId::AutoAttackPotency) == 80);
        CHECK(StageValue(r.trace, FfxivCombatStageId::TraitPercent) == 100);
        CHECK(r.value == 29);
        // ... while the same stats through the ordinary pipeline do carry the trait.
        FfxivDirectDamageResult const normal =
            ComputeFfxivDirectDamage(stats, 80, 182, 100, FfxivPartyComposition(), Traits(), rolls);
        CHECK(StageValue(normal.trace, FfxivCombatStageId::TraitPercent) == 120);
        CHECK(normal.value == 35);
    }
}

TEST_CASE("FFXIV stage order matches the published chains", "[ffxiv]")
{
    FfxivDerivedStats const stats = Whm100();
    FfxivFixedRollSource rolls(99, 100);

    SECTION("direct damage: crit/DH are applied before variance")
    {
        FfxivDirectDamageResult const r =
            ComputeFfxivDirectDamage(stats, 100, 182, 100, FfxivPartyComposition(), Traits(), rolls);
        int const crit = StageIndex(r.trace, FfxivCombatStageId::CriticalMultiplierPermille);
        int const dh = StageIndex(r.trace, FfxivCombatStageId::DirectHitMultiplierPercent);
        int const stage3 = StageIndex(r.trace, FfxivCombatStageId::Stage3Damage);
        int const variance = StageIndex(r.trace, FfxivCombatStageId::VariancePercent);
        int const final = StageIndex(r.trace, FfxivCombatStageId::FinalDamage);
        CHECK(crit > 0);
        CHECK(dh > crit);
        CHECK(stage3 > dh);
        CHECK(variance > stage3);
        CHECK(final > variance);
    }

    SECTION("DoT: variance rolls before crit/DH and the +1 lands before both")
    {
        FfxivDerivedStats const dotStats = Geared(MNK, 100, 4000, 442, 442, 442, 442, 442, 442);
        // floor(37*105/100) = 38 (the value the variance roll produces on its own)
        FfxivFixedRollSource varianceHigh(99, 105);
        FfxivCombatTrace trace;
        CHECK(ComputeFfxivDotTick(dotStats, 100, 180, 100, FfxivPartyComposition(), Traits(), varianceHigh,
                                  {}, &trace) == 38);
        // With crit and DH forced on: floor(38*1401*125/100000) = 66 -- i.e. both
        // multipliers are applied to the already-varied value, as B §F23.7 says.
        FfxivFixedRollSource critHigh(0, 105);
        FfxivCombatTrace critTrace;
        CHECK(ComputeFfxivDotTick(dotStats, 100, 180, 100, FfxivPartyComposition(), Traits(), critHigh, {},
                                  &critTrace) == 66);
        CHECK(StageValue(critTrace, FfxivCombatStageId::DotPlusOne) == 37);
        // Published DoT order: +1 and variance come before the crit/DH multipliers.
        CHECK(StageIndex(critTrace, FfxivCombatStageId::DotPlusOne)
              < StageIndex(critTrace, FfxivCombatStageId::CriticalRoll));
        CHECK(StageIndex(critTrace, FfxivCombatStageId::VariancePercent)
              < StageIndex(critTrace, FfxivCombatStageId::CriticalRoll));
    }
}

TEST_CASE("FFXIV roll sources are deterministic", "[ffxiv]")
{
    SECTION("fixed source")
    {
        FfxivFixedRollSource rolls(42, 97);
        CHECK(rolls.Roll100() == 42);
        CHECK(rolls.Roll100() == 42);
        CHECK(rolls.RollVariance(95, 105) == 97);
    }

    SECTION("seeded source (splitmix64, seed 42)")
    {
        FfxivSeededRollSource rolls(42);
        CHECK(rolls.Roll100() == 13);
        CHECK(rolls.Roll100() == 91);

        FfxivSeededRollSource variance(42);
        CHECK(variance.RollVariance(95, 105) == 104);
        CHECK(variance.RollVariance(95, 105) == 100);
        CHECK(variance.RollVariance(95, 105) == 97);
    }

    SECTION("same seed, same pipeline result")
    {
        FfxivDerivedStats const stats = Whm100();
        FfxivSeededRollSource first(1234);
        FfxivSeededRollSource second(1234);
        FfxivDirectDamageResult const a =
            ComputeFfxivDirectDamage(stats, 100, 182, 100, FfxivPartyComposition(), Traits(), first);
        FfxivDirectDamageResult const b =
            ComputeFfxivDirectDamage(stats, 100, 182, 100, FfxivPartyComposition(), Traits(), second);
        CHECK(a.value == b.value);
        CHECK(a.crit == b.crit);
        CHECK(a.directHit == b.directHit);
        // Seed 1234 draws 95 (no crit: 9500 >= 510), 64 (no DH: 6400 >= 40), then
        // variance 102 -> floor(47*102/100) = 47.
        CHECK_FALSE(a.crit);
        CHECK_FALSE(a.directHit);
        CHECK(StageValue(a.trace, FfxivCombatStageId::VariancePercent) == 102);
        CHECK(a.value == 47);
    }
}
