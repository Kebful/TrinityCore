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
 * The published damage / healing / DoT pipeline, transcribed stage by stage.
 *
 * Read FFXIVCombatMath.h first: it carries the source map, the units of every
 * factor and the one scale ambiguity (the `/ 100` of the second stage). This file
 * is only the arithmetic.
 *
 * Preconditions on `stats` (checked nowhere, documented here):
 *   - it came from FFXIV::Stats::ComputeFfxivStats, so rate.* is consistent with
 *     param[] at the same level (the combat math reads the published per-mille
 *     multipliers from rate.* and does not re-derive them);
 *   - stats.level is the level the rates were computed at (slice 1 has no sync:
 *     effectiveLevel == level);
 *   - for healing, param[BaseParam 34 Healing Magic Potency] is filled. Slice 1
 *     leaves it at 0, so a caller must supply it (see the header).
 */

#include "FFXIVCombatMath.h"
#include "FFXIV/Stats/FFXIVStatTables.h"
#include <algorithm>

namespace FFXIV::Combat
{
namespace
{
// B §B.0: floor(value / divisor) for divisor > 0. Every published fl() is this.
int64 FloorDiv(int64 value, int64 divisor)
{
    int64 quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0)))
        --quotient;
    return quotient;
}

// B §B.0: signed truncation toward zero, for the terms B §B.0 says keep it
// (f(ATK)'s trunc(...) and anything that can go negative).
int64 TruncDiv(int64 value, int64 divisor) { return value / divisor; }

// The ClassJob sheet's own percentage for the attribute that feeds f(WD)
// (ClassJob columns 9..16, e.g. WHM MND 115). 100 is neutral.
int32 GetJobMainAttributePercent(uint8 jobId, uint8 attributeParam)
{
    Stats::FfxivJobRow const& job = Stats::GetFfxivJobRow(jobId);
    switch (attributeParam)
    {
        case Stats::FFXIV_PARAM_STRENGTH: return job.strPct;
        case Stats::FFXIV_PARAM_DEXTERITY: return job.dexPct;
        case Stats::FFXIV_PARAM_INTELLIGENCE: return job.intPct;
        case Stats::FFXIV_PARAM_MIND: return job.mndPct;
        default: return 100;
    }
}

// The level base main stat (M of B §F6) the stat and combat tables share.
int32 GetLevelMainStat(uint8 level) { return Stats::GetFfxivLevelRow(level).mainStat; }

// Shared by every pipeline: attack stat after the party bonus, f(ATK), f(DET),
// f(TNC), f(WD) and the trait, in one place so damage/DoT/auto cannot drift.
struct FfxivAttackTerms
{
    int32 mainStat = 0;         // party-boosted
    int32 attackPercent = 0;    // f(ATK)
    int32 detPermille = 0;      // f(DET)
    int32 tncPermille = 0;      // f(TNC) outgoing
    int32 weaponPercent = 0;    // f(WD)
    int32 traitPercent = 100;
};

FfxivAttackTerms BuildAttackTerms(FfxivDerivedStats const& stats, int32 weaponDamage,
                                  FfxivPartyComposition party, FfxivTraitState traits)
{
    FfxivAttackTerms terms;
    uint8 const attribute = GetFfxivAttackPowerAttribute(stats.jobId);
    terms.mainStat = ComputeFfxivPartyBoostedMainStat(stats.param[attribute], party);
    terms.attackPercent = ComputeFfxivAttackPowerPercent(stats, terms.mainStat, IsFfxivTankJob(stats.jobId));
    terms.detPermille = stats.rate.detBp / 10;
    terms.tncPermille = stats.rate.tncOutBp / 10;
    terms.weaponPercent = weaponDamage;
    terms.traitPercent = traits.damageTrait ? ComputeFfxivDamageTraitPercent(stats.jobId) : 100;
    return terms;
}

// B §F23.5: the variance roll is its own floored stage (rand[95,105] for damage,
// rand[97,103] for healing).
int64 ApplyVariance(int64 value, int32 variancePercent, FfxivCombatTrace& trace)
{
    trace.Add(FfxivCombatStageId::VariancePercent, variancePercent);
    return FloorDiv(value * variancePercent, 100);
}

// B §F23.5: buffs stack multiplicatively, one floor per buff.
int64 ApplyBuffs(int64 value, std::span<FfxivPercentFactor const> buffs, FfxivCombatTrace& trace)
{
    for (FfxivPercentFactor const& buff : buffs)
    {
        trace.Add(FfxivCombatStageId::BuffPercent, buff.numerator);
        value = FloorDiv(value * buff.numerator, buff.denominator);
    }
    return value;
}
} // namespace

// --------------------------------------------------------------------------
// trace
// --------------------------------------------------------------------------

void FfxivCombatTrace::Add(FfxivCombatStageId id, int64 value)
{
    if (count >= FFXIV_COMBAT_MAX_STAGES)
        return; // fixed capacity; a longer chain is a programming error, not data loss
    stages[count].id = id;
    stages[count].value = value;
    ++count;
}

int64 const* FfxivCombatTrace::Find(FfxivCombatStageId id) const
{
    for (uint8 i = count; i > 0; --i)
        if (stages[i - 1].id == id)
            return &stages[i - 1].value;
    return nullptr;
}

// --------------------------------------------------------------------------
// rolls
// --------------------------------------------------------------------------

uint64 FfxivSeededRollSource::Next()
{
    // splitmix64: tiny, deterministic, platform independent. NOT retail's RNG.
    _state += 0x9E3779B97F4A7C15ull;
    uint64 z = _state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint32 FfxivSeededRollSource::Roll100() { return uint32(Next() % 100); }

int32 FfxivSeededRollSource::RollVariance(int32 minPercent, int32 maxPercent)
{
    if (maxPercent <= minPercent)
        return minPercent;
    return minPercent + int32(Next() % uint64(maxPercent - minPercent + 1));
}

// --------------------------------------------------------------------------
// table / term accessors
// --------------------------------------------------------------------------

int32 GetFfxivAttackPowerCoefficient(uint8 level, bool tank, uint8* provenance)
{
    FfxivAttackPowerRow const& row = GetFfxivAttackPowerRow(level);
    if (provenance)
        *provenance = tank ? row.tankProvenance : row.provenance;
    return tank ? row.tank : row.nonTank;
}

int32 GetFfxivHealingPotencyDivisor(uint8 level, uint8* provenance)
{
    FfxivHealingPotencyRow const& row = GetFfxivHealingPotencyRow(level);
    if (provenance)
        *provenance = row.provenance;
    return row.divisor;
}

bool IsFfxivTankJob(uint8 jobId)
{
    return Stats::GetFfxivJobRow(jobId).role == Stats::FFXIV_JOB_ROLE_TANK;
}

bool GetFfxivPartyRole(uint8 jobId, FfxivPartyRole& role)
{
    Stats::FfxivJobRow const& job = Stats::GetFfxivJobRow(jobId);
    switch (job.role)
    {
        case Stats::FFXIV_JOB_ROLE_TANK:
            role = FfxivPartyRole::Tank;
            return true;
        case Stats::FFXIV_JOB_ROLE_HEALER:
            role = FfxivPartyRole::Healer;
            return true;
        case Stats::FFXIV_JOB_ROLE_MELEE:
            role = FfxivPartyRole::Melee;
            return true;
        case Stats::FFXIV_JOB_ROLE_RANGED:
            role = job.jobType == Stats::FFXIV_JOB_TYPE_DOM ? FfxivPartyRole::Caster
                                                            : FfxivPartyRole::PhysicalRanged;
            return true;
        default:
            return false; // DoH/DoL and the neutral row have no combat role
    }
}

uint8 GetFfxivAttackPowerAttribute(uint8 jobId) { return GetFfxivJobCombatRow(jobId).attributeParam; }

int32 ComputeFfxivDamageTraitPercent(uint8 jobId) { return GetFfxivJobCombatRow(jobId).damageTraitPercent; }

int32 ComputeFfxivAutoAttackPotency(uint8 jobId) { return GetFfxivJobCombatRow(jobId).autoAttackPotency; }

FfxivDotDamageType GetFfxivDotType(uint8 jobId)
{
    return GetFfxivJobCombatRow(jobId).dotType == FFXIV_DOT_MAGICAL ? FFXIV_DOT_MAGICAL : FFXIV_DOT_PHYSICAL;
}

int32 ComputeFfxivPartyBoostedMainStat(int32 mainStat, FfxivPartyComposition party)
{
    // B §F24.2 / §F23.5: floor(main * (1 + 0.01 * roles)).
    return int32(FloorDiv(int64(mainStat) * party.BonusPercent(), 100));
}

int32 ComputeFfxivAttackPowerPercent(FfxivDerivedStats const& stats, int32 mainStat, bool tank)
{
    int32 const main = GetLevelMainStat(stats.level);
    if (main <= 0)
        return 0;
    int32 const coefficient = GetFfxivAttackPowerCoefficient(stats.level, tank);
    // B §F6: max(0, (trunc(apMod * (stat - MAIN) / MAIN) + 100) / 100), kept in
    // percent here (100 == 1.0x), with signed truncation inside the parenthesis.
    int64 const term = TruncDiv(int64(coefficient) * (int64(mainStat) - main), main);
    return int32(std::max<int64>(0, term + 100));
}

int32 ComputeFfxivWeaponDamagePercent(FfxivDerivedStats const& stats, int32 weaponDamage)
{
    // B §F6: f(WD) = floor(MAIN * jobMainMod / 1000 + WD), MAIN = level base main.
    int32 const main = GetLevelMainStat(stats.level);
    int32 const jobMod = GetJobMainAttributePercent(stats.jobId, GetFfxivAttackPowerAttribute(stats.jobId));
    return int32(FloorDiv(int64(main) * jobMod, 1000) + weaponDamage);
}

int32 ComputeFfxivAutoAttackWeaponDamagePercent(FfxivDerivedStats const& stats, int32 weaponDamage,
                                                uint32 weaponDelayMs)
{
    // B §F14/F§23.4: fl(fl(MAIN * jobAutoMod / 1000 + WD) * (delay / 3)) / 100.
    int32 const inner = ComputeFfxivWeaponDamagePercent(stats, weaponDamage);
    if (weaponDelayMs == 0)
        return inner;
    return int32(FloorDiv(int64(inner) * weaponDelayMs, 3000));
}

int32 ComputeFfxivHealingMagicPotencyPercent(FfxivDerivedStats const& stats, int32 healingMagicPotency)
{
    // B §F23.4: f(HMP) = floor(100 * (HMP - MAIN) / divisor) + 100, in percent.
    int32 const main = GetLevelMainStat(stats.level);
    int32 const divisor = GetFfxivHealingPotencyDivisor(stats.level);
    if (divisor <= 0)
        return 100;
    return int32(FloorDiv(100 * (int64(healingMagicPotency) - main), divisor) + 100);
}

int32 ComputeFfxivSpeedPermille(FfxivDerivedStats const& stats, FfxivDotDamageType type)
{
    // B §F13: 1000 + fl(130 * (SPD - S) / D). `rate.*Bp` is the *GCD* direction
    // (1000 + trunc(130*(S-SPD)/D)), i.e. the exact negation of the tick term, so
    // f(SPD) = 2000 - rate factor keeps the sign convention B §B.0 asks for: a
    // below-baseline speed is a signed truncation, not a floor of a negative.
    int32 const speedFactor = (type == FFXIV_DOT_MAGICAL ? stats.rate.spsBp : stats.rate.sksBp) / 10;
    return 2000 - speedFactor;
}

int32 ComputeFfxivLevelCorrectionPermille(uint8 attackerLevel, uint8 targetLevel)
{
    // B §F23.5: 1 - 0.025 * max(0, target - attacker), in per mille.
    int32 const difference = std::max<int32>(0, int32(targetLevel) - int32(attackerLevel));
    return std::max<int32>(0, 1000 - FFXIV_LEVEL_CORRECTION_PERMILLE_PER_LEVEL * difference);
}

// --------------------------------------------------------------------------
// direct damage (B §F15.1 / §F23.5)
// --------------------------------------------------------------------------

FfxivDirectDamageResult ComputeFfxivDirectDamage(FfxivDerivedStats const& stats, int32 potency,
                                                 int32 weaponDamage, uint8 targetLevel,
                                                 FfxivPartyComposition party, FfxivTraitState traits,
                                                 FfxivRollSource& rolls,
                                                 std::span<FfxivPercentFactor const> buffs)
{
    FfxivDirectDamageResult result;
    FfxivAttackTerms const terms = BuildAttackTerms(stats, weaponDamage, party, traits);

    result.trace.Add(FfxivCombatStageId::AttackPowerPercent, terms.attackPercent);
    result.trace.Add(FfxivCombatStageId::DeterminationPermille, terms.detPermille);

    // D1 = floor(Potency * f(ATK) * f(DET) / 100) / 1000
    int64 const damage1 = FloorDiv(int64(potency) * terms.attackPercent * terms.detPermille, 100) / 1000;
    result.trace.Add(FfxivCombatStageId::BaseDamage, damage1);

    result.trace.Add(FfxivCombatStageId::TenacityPermille, terms.tncPermille);
    result.trace.Add(FfxivCombatStageId::WeaponDamagePercent, terms.weaponPercent);
    result.trace.Add(FfxivCombatStageId::TraitPercent, terms.traitPercent);

    // D2 = floor(D1 * f(TNC)/1000 * f(WD)/100 * Trait) / 100
    // The trailing / 100 is the published scale division; see the header's
    // UNITS / SCALE note (the alternative flp(2) reading of B §B.0 is 100x larger).
    int64 const damage2 = FloorDiv(damage1 * terms.tncPermille * terms.weaponPercent * terms.traitPercent,
                                   int64(1000) * 100 * 100)
        / 100;
    result.trace.Add(FfxivCombatStageId::Stage2Damage, damage2);

    // F9 / F10 rolls. Order matters for a seeded source: crit first, then DH.
    result.critChanceBp = stats.rate.critBp;
    result.dhChanceBp = stats.rate.dhBp;
    uint32 const critRoll = rolls.Roll100();
    result.trace.Add(FfxivCombatStageId::CriticalRoll, critRoll);
    result.crit = int64(critRoll) * 100 < result.critChanceBp;
    int64 const critPermille = result.crit ? stats.rate.critDamageBp / 10 : 1000;
    result.trace.Add(FfxivCombatStageId::CriticalMultiplierPermille, critPermille);

    uint32 const dhRoll = rolls.Roll100();
    result.trace.Add(FfxivCombatStageId::DirectHitRoll, dhRoll);
    result.directHit = int64(dhRoll) * 100 < result.dhChanceBp;
    int64 const dhPercent = result.directHit ? FFXIV_DIRECT_HIT_MULTIPLIER_PERCENT : 100;
    result.trace.Add(FfxivCombatStageId::DirectHitMultiplierPercent, dhPercent);

    // D3 = floor(D2 * CRIT?/1000 * DH?/100)
    int64 const damage3 = FloorDiv(damage2 * critPermille * dhPercent, int64(1000) * 100);
    result.trace.Add(FfxivCombatStageId::Stage3Damage, damage3);

    // D = floor(D3 * rand[95,105] / 100 * buffs ...)
    int32 const variance = rolls.RollVariance(FFXIV_DAMAGE_VARIANCE_MIN_PERCENT, FFXIV_DAMAGE_VARIANCE_MAX_PERCENT);
    int64 value = ApplyVariance(damage3, variance, result.trace);
    value = ApplyBuffs(value, buffs, result.trace);

    // B §F23.5 level correction. The published text fixes multiplier and slope
    // but not the application point (H.30 V4); this applies it once, after the
    // published variance + buff stages, as its own floored stage.
    int32 const correction = ComputeFfxivLevelCorrectionPermille(stats.level, targetLevel);
    result.trace.Add(FfxivCombatStageId::LevelCorrectionPermille, correction);
    value = FloorDiv(value * correction, 1000);

    value = std::max<int64>(value, FFXIV_MIN_DAMAGE);
    result.trace.Add(FfxivCombatStageId::FinalDamage, value);
    result.value = value;
    return result;
}

// --------------------------------------------------------------------------
// direct heal (B §F16.1 / §F23.6)
// --------------------------------------------------------------------------

FfxivHealResult ComputeFfxivHeal(FfxivDerivedStats const& stats, int32 curePotency, int32 weaponDamage,
                                 uint8 targetLevel, FfxivPartyComposition party, FfxivTraitState traits,
                                 FfxivRollSource& rolls, std::span<FfxivPercentFactor const> buffs)
{
    FfxivHealResult result;

    // f(HMP) replaces f(ATK); there is no Direct Hit term anywhere in healing.
    // B §F24.2 says the party bonus applies to the main stat and VIT. HMP is the
    // healing chain's main-stat term (slice 1 has no HMP derivation at all), so the
    // same +1 %/role is applied to it -- INFERRED, and it must not be applied twice
    // once a later slice derives HMP from an already-boosted MND.
    int32 const hmp = ComputeFfxivPartyBoostedMainStat(
        stats.param[Stats::FFXIV_PARAM_HEALING_MAGIC_POTENCY], party);
    int32 const hmpPercent = ComputeFfxivHealingMagicPotencyPercent(stats, hmp);
    int32 const detPermille = stats.rate.detBp / 10;
    int32 const tncPermille = stats.rate.tncOutBp / 10;
    int32 const weaponPercent = weaponDamage;
    int32 const traitPercent = traits.damageTrait ? ComputeFfxivDamageTraitPercent(stats.jobId) : 100;

    result.trace.Add(FfxivCombatStageId::HealingMagicPotencyPercent, hmpPercent);
    result.trace.Add(FfxivCombatStageId::DeterminationPermille, detPermille);

    // H1 = floor(Potency * f(HMP) * f(DET) / 100) / 1000
    int64 const heal1 = FloorDiv(int64(curePotency) * hmpPercent * detPermille, 100) / 1000;
    result.trace.Add(FfxivCombatStageId::BaseDamage, heal1);

    result.trace.Add(FfxivCombatStageId::TenacityPermille, tncPermille);
    result.trace.Add(FfxivCombatStageId::WeaponDamagePercent, weaponPercent);
    result.trace.Add(FfxivCombatStageId::TraitPercent, traitPercent);

    // H2 = floor(H1 * f(TNC)/1000 * f(WD)/100 * Trait) / 100
    int64 const heal2 = FloorDiv(heal1 * tncPermille * weaponPercent * traitPercent,
                                 int64(1000) * 100 * 100)
        / 100;
    result.trace.Add(FfxivCombatStageId::Stage2Damage, heal2);

    result.critChanceBp = stats.rate.critBp;
    uint32 const critRoll = rolls.Roll100();
    result.trace.Add(FfxivCombatStageId::CriticalRoll, critRoll);
    result.crit = int64(critRoll) * 100 < result.critChanceBp;
    int64 const critPermille = result.crit ? stats.rate.critDamageBp / 10 : 1000;
    result.trace.Add(FfxivCombatStageId::CriticalMultiplierPermille, critPermille);

    // H3 = floor(H2 * CRIT? / 1000)   -- crit once, no DH
    int64 const heal3 = FloorDiv(heal2 * critPermille, 1000);
    result.trace.Add(FfxivCombatStageId::Stage3Damage, heal3);

    // H = floor(H3 * rand[97,103] / 100 * buffs ...)
    int32 const variance = rolls.RollVariance(FFXIV_HEAL_VARIANCE_MIN_PERCENT, FFXIV_HEAL_VARIANCE_MAX_PERCENT);
    int64 value = ApplyVariance(heal3, variance, result.trace);
    value = ApplyBuffs(value, buffs, result.trace);

    // The published level-correction text talks about damage dealt; whether it
    // also reduces heals to a higher-level target is UNKNOWN. Applied for
    // symmetry with the damage chain and flagged.
    int32 const correction = ComputeFfxivLevelCorrectionPermille(stats.level, targetLevel);
    result.trace.Add(FfxivCombatStageId::LevelCorrectionPermille, correction);
    value = FloorDiv(value * correction, 1000);

    result.trace.Add(FfxivCombatStageId::FinalDamage, value);
    result.value = value;
    return result;
}

// --------------------------------------------------------------------------
// DoT tick (B §F13.1 / §F15.2 / §F23.7)
// --------------------------------------------------------------------------

namespace
{
int64 ComputeFfxivDotTickInternal(FfxivDerivedStats const& stats, int32 potency, int32 weaponDamage,
                                  FfxivDotDamageType type, uint8 targetLevel, FfxivPartyComposition party,
                                  FfxivTraitState traits, FfxivRollSource& rolls,
                                  std::span<FfxivPercentFactor const> buffs, FfxivCombatTrace& trace)
{
    FfxivAttackTerms const terms = BuildAttackTerms(stats, weaponDamage, party, traits);
    // f(SPD) is a per-tick term; its exact position is what B §F23.7 marks
    // "unconfirmed" -- these are the two published placements.
    int32 const speedPermille = ComputeFfxivSpeedPermille(stats, type);

    trace.Add(FfxivCombatStageId::AttackPowerPercent, terms.attackPercent);
    trace.Add(FfxivCombatStageId::DeterminationPermille, terms.detPermille);
    trace.Add(FfxivCombatStageId::SpeedPermille, speedPermille);

    int64 damage1;
    int64 damage2;
    if (type == FFXIV_DOT_PHYSICAL)
    {
        // D1 = floor(Potency * f(ATK) * f(DET) / 100) / 1000
        damage1 = FloorDiv(int64(potency) * terms.attackPercent * terms.detPermille, 100) / 1000;
        trace.Add(FfxivCombatStageId::BaseDamage, damage1);
        trace.Add(FfxivCombatStageId::TenacityPermille, terms.tncPermille);
        trace.Add(FfxivCombatStageId::WeaponDamagePercent, terms.weaponPercent);
        trace.Add(FfxivCombatStageId::TraitPercent, terms.traitPercent);
        // D2 = floor(D1 * f(TNC)/1000 * f(SPD)/1000 * f(WD)/100 * Trait) / 100 + 1
        damage2 = FloorDiv(damage1 * terms.tncPermille * speedPermille * terms.weaponPercent * terms.traitPercent,
                           int64(1000) * 1000 * 100 * 100)
            / 100;
    }
    else
    {
        // D1 = floor(Potency * f(WD)/100 * f(ATK)/100 * f(SPD)/1000)
        damage1 = FloorDiv(int64(potency) * terms.weaponPercent * terms.attackPercent * speedPermille,
                           int64(100) * 100 * 1000);
        trace.Add(FfxivCombatStageId::BaseDamage, damage1);
        trace.Add(FfxivCombatStageId::TenacityPermille, terms.tncPermille);
        trace.Add(FfxivCombatStageId::WeaponDamagePercent, terms.weaponPercent);
        trace.Add(FfxivCombatStageId::TraitPercent, terms.traitPercent);
        // D2 = floor(D1 * f(DET)/1000 * f(TNC)/1000 * Trait) / 100 + 1
        damage2 = FloorDiv(damage1 * terms.detPermille * terms.tncPermille * terms.traitPercent,
                           int64(1000) * 1000 * 100)
            / 100;
    }
    damage2 += 1; // the published +1 lands before variance, unlike direct damage
    trace.Add(FfxivCombatStageId::DotPlusOne, damage2);

    // Variance rolls before crit/DH for DoTs (B §F23.7, measured from ~80k samples).
    int32 const variance = rolls.RollVariance(FFXIV_DAMAGE_VARIANCE_MIN_PERCENT, FFXIV_DAMAGE_VARIANCE_MAX_PERCENT);
    int64 value = ApplyVariance(damage2, variance, trace);

    uint32 const critRoll = rolls.Roll100();
    trace.Add(FfxivCombatStageId::CriticalRoll, critRoll);
    bool const crit = int64(critRoll) * 100 < stats.rate.critBp;
    int64 const critPermille = crit ? stats.rate.critDamageBp / 10 : 1000;
    trace.Add(FfxivCombatStageId::CriticalMultiplierPermille, critPermille);

    uint32 const dhRoll = rolls.Roll100();
    trace.Add(FfxivCombatStageId::DirectHitRoll, dhRoll);
    bool const directHit = int64(dhRoll) * 100 < stats.rate.dhBp;
    int64 const dhPercent = directHit ? FFXIV_DIRECT_HIT_MULTIPLIER_PERCENT : 100;
    trace.Add(FfxivCombatStageId::DirectHitMultiplierPercent, dhPercent);

    // D = floor(D3 * CRIT?/1000 * DH?/100 * buffs)
    value = FloorDiv(value * critPermille * dhPercent, int64(1000) * 100);
    value = ApplyBuffs(value, buffs, trace);

    int32 const correction = ComputeFfxivLevelCorrectionPermille(stats.level, targetLevel);
    trace.Add(FfxivCombatStageId::LevelCorrectionPermille, correction);
    value = FloorDiv(value * correction, 1000);

    trace.Add(FfxivCombatStageId::FinalDamage, value);
    return value;
}
} // namespace

int64 ComputeFfxivDotTick(FfxivDerivedStats const& stats, int32 potency, int32 weaponDamage, uint8 targetLevel,
                          FfxivPartyComposition party, FfxivTraitState traits, FfxivRollSource& rolls,
                          std::span<FfxivPercentFactor const> buffs, FfxivCombatTrace* trace)
{
    FfxivCombatTrace ignored;
    return ComputeFfxivDotTickInternal(stats, potency, weaponDamage, GetFfxivDotType(stats.jobId), targetLevel,
                                       party, traits, rolls, buffs, trace ? *trace : ignored);
}

int64 ComputeFfxivDotTickOfType(FfxivDerivedStats const& stats, int32 potency, int32 weaponDamage,
                                FfxivDotDamageType type, uint8 targetLevel, FfxivPartyComposition party,
                                FfxivTraitState traits, FfxivRollSource& rolls,
                                std::span<FfxivPercentFactor const> buffs, FfxivCombatTrace* trace)
{
    FfxivCombatTrace ignored;
    return ComputeFfxivDotTickInternal(stats, potency, weaponDamage, type, targetLevel, party, traits, rolls,
                                       buffs, trace ? *trace : ignored);
}

// --------------------------------------------------------------------------
// auto-attack (B §F14 / §F24.2)
// --------------------------------------------------------------------------

FfxivDirectDamageResult ComputeFfxivAutoAttackDamage(FfxivDerivedStats const& stats, uint8 targetLevel,
                                                     FfxivPartyComposition party, FfxivRollSource& rolls,
                                                     int32 weaponDamage, uint32 weaponDelayMs,
                                                     std::span<FfxivPercentFactor const> buffs)
{
    int32 const potency = ComputeFfxivAutoAttackPotency(stats.jobId);
    int32 const weaponPercent = ComputeFfxivAutoAttackWeaponDamagePercent(stats, weaponDamage, weaponDelayMs);

    // Traits never apply to auto-attacks (B §F24.2), whatever the caller passed.
    FfxivTraitState const noTraits{ false };
    FfxivDirectDamageResult result =
        ComputeFfxivDirectDamage(stats, potency, weaponPercent, targetLevel, party, noTraits, rolls, buffs);
    result.trace.Add(FfxivCombatStageId::AutoAttackPotency, potency);
    return result;
}
} // namespace FFXIV::Combat
