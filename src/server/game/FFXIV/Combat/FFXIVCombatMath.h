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

#ifndef TRINITYCORE_FFXIV_COMBAT_MATH_H
#define TRINITYCORE_FFXIV_COMBAT_MATH_H

#include "FFXIV/Combat/FFXIVCombatTables.h"
#include "FFXIV/Stats/FFXIVStatTypes.h"
#include <array>
#include <initializer_list>
#include <span>

/*
 * Pure FFXIV combat math -- the published damage / healing / DoT pipeline.
 *
 * Nothing here is applied to a spell, a Unit field or a packet. Every function is
 * a pure function of its arguments plus the generated constant tables: no Player,
 * no DB, no aura, no RNG owned by the function (variance and crit/DH rolls come
 * through an explicit FfxivRollSource, so a test can pin them).
 *
 * Sources, stage by stage (B = work/stat-research/B-FORMULA-REFERENCE.md,
 * I = work/stat-research/I-REFRESH-20261123.md):
 *
 *   f(ATK)  B §F6 / §F23.3 / §F24.7, table in FFXIVCombatTables.inc
 *           f(ATK)% = max(0, trunc(apMod * (ATK - MAIN) / MAIN) + 100)
 *                     with ATK already party-boosted and MAIN the level base
 *                     main stat (M). "Percent" means 100 == 1.0x.
 *   f(DET)  B §F8   (1000 + floor(140 * (DET - M) / D)) / 1000   [per mille]
 *   f(TNC)  B §F11  (1000 + floor(112 * (TNC - S) / D)) / 1000   [per mille]
 *   f(WD)   B §F6   floor(M * jobMainMod / 1000 + WD) / 100      [percent]
 *   f(CRIT) B §F9   damage (1400 + floor(200 * (CRIT - S) / D)) / 1000
 *   DH      B §F10  flat 1.25 multiplier, no healing analogue
 *   f(SPD)  B §F13  1000 + 130 * (SPD - S) / D, signed truncation per B §B.0
 *                                        [per mille]
 *   f(HMP)  B §F23.4 floor(100 * (HMP - MAIN) / divisor) + 100   [percent]
 *   trait   B §F24.2 / I §9.5   1.00 / 1.20 / 1.30 / 1.50, never on auto-attacks
 *   party   B §F24.2 / I §9.3   +1 % per unique party role, 0..5, pets excluded
 *   level   B §F23.5            -2.5 % per attacker level below the target
 *
 * THE PUBLISHED ORDERS
 *
 *   Direct damage (B §F15.1 / §F23.5):
 *     D1 = floor(Potency * f(ATK) * f(DET) / 100) / 1000
 *     D2 = floor(D1 * f(TNC)/1000 * f(WD)/100 * Trait) / 100
 *     D3 = floor(D2 * CRIT?/1000 * DH?/100)
 *     D  = floor(D3 * rand[95,105]/100 * buff1 * buff2 ...)
 *   Direct heal (B §F16.1 / §F23.6): same shape with f(HMP) in the f(ATK) slot,
 *     rand[97,103] and no Direct Hit term.
 *   DoT (B §F13.1 / §F15.2 / §F23.7): two branches. f(SPD) enters the tick, the
 *     `+1` lands before variance, and variance rolls *before* crit/DH.
 *
 * UNITS / SCALE -- READ THIS BEFORE USING THE NUMBERS
 * --------------------------------------------------
 * The chain is transcribed exactly as published, floors and divisors included.
 * The absolute magnitude therefore depends on the *units* of the published
 * factors, and one of them is ambiguous:
 *
 *   * B §F23.5 writes the second stage as `floor(...) / 100`. Read as a scale
 *     division it puts an L100 100-potency hit at D2 = 47 (see the fixture in
 *     tests/game/FFXIVCombatMath.cpp). Read as the two-decimal trim of B §B.0
 *     flp(2, x) -- the same notation §F8 uses for flp -- the same fixture gives
 *     D2 = 4772, which is the plausible retail scale.
 *   * This file implements the published divisor literally. The ambiguity is
 *     reported, not resolved by invention: see the stage-2 comment in
 *     FFXIVCombatMath.cpp and the UNKNOWN list in the report. Changing the
 *     reading is a one-line change in stage 2 of each pipeline plus the goldens.
 *
 * Rounding: every stage narrows exactly as B §B.0 prescribes -- fl() is floor for
 * the published floors, and the two speed/multiplier terms that can go negative
 * (f(ATK), f(SPD)) use signed truncation toward zero, which B §B.0 mandates for
 * negative intermediates. All arithmetic is int64 over integers, so no floating
 * point epsilon guard is needed (the model's fl() guard cannot fire on integer
 * numerators over these divisors).
 */

namespace FFXIV::Combat
{
// Slice 1's typed snapshot is the only stat input these functions accept.
using Stats::FfxivDerivedStats;

// --------------------------------------------------------------------------
// published coefficients (B §F8-F11, §F15.1, §F16.1, §F23.5-F§23.7, §F24.2)
// --------------------------------------------------------------------------

// B §F10: the Direct Hit multiplier. DH? is 100 (no direct hit) or 125.
constexpr int32 FFXIV_DIRECT_HIT_MULTIPLIER_PERCENT = 125;

// B §F15.1 / §F23.5: uniform variance band for direct damage and DoT ticks.
constexpr int32 FFXIV_DAMAGE_VARIANCE_MIN_PERCENT = 95;
constexpr int32 FFXIV_DAMAGE_VARIANCE_MAX_PERCENT = 105;

// B §F16.1 / §F23.6: healing variance is +-3 %, not +-5 %.
constexpr int32 FFXIV_HEAL_VARIANCE_MIN_PERCENT = 97;
constexpr int32 FFXIV_HEAL_VARIANCE_MAX_PERCENT = 103;

// B §F23.5: attacker below the target deals -2.5 % per level of difference, in
// per mille per level (25 == 2.5 %). The application point is UNKNOWN (H.30 V4);
// this file applies it once, after the published variance + buff stages, as its
// own floored stage.
constexpr int32 FFXIV_LEVEL_CORRECTION_PERMILLE_PER_LEVEL = 25;

// B §F24.2 / I §9.3: +1 % per *unique* party role, 0..5 roles, pets excluded.
constexpr int32 FFXIV_PARTY_BONUS_PERCENT_PER_ROLE = 1;
constexpr int32 FFXIV_MAX_PARTY_ROLES = 5;

// F15 (model) floors a hit at 1; no published stage says it. Kept as an explicit
// knob so the deviation is visible rather than buried.
constexpr int32 FFXIV_MIN_DAMAGE = 1;

// Standard weapon delay used by the auto-attack helper when the caller has no
// weapon row. The auto-attack formula itself takes the delay as an input.
constexpr uint32 FFXIV_STANDARD_AUTO_ATTACK_DELAY_MS = 3000;

// --------------------------------------------------------------------------
// party composition
// --------------------------------------------------------------------------

// The five roles that count for the party bonus (B §F24.2: 0..5 roles). Pets are
// not roles and are simply absent from the mask.
enum class FfxivPartyRole : uint8
{
    Tank = 0,
    Healer,
    Melee,
    PhysicalRanged,
    Caster,
    Count
};

struct FfxivPartyComposition
{
    uint8 roleMask = 0;

    constexpr FfxivPartyComposition() = default;
    explicit constexpr FfxivPartyComposition(uint8 mask) : roleMask(mask) { }

    constexpr FfxivPartyComposition With(FfxivPartyRole role) const
    {
        return FfxivPartyComposition(uint8(roleMask | (1u << uint8(role))));
    }

    // Distinct roles present, 0..5. Bits above FfxivPartyRole::Caster are ignored.
    constexpr uint8 UniqueRoleCount() const
    {
        uint8 const mask = uint8(roleMask & ((1u << uint8(FfxivPartyRole::Count)) - 1));
        uint8 count = 0;
        for (uint8 bit = 0; bit < uint8(FfxivPartyRole::Count); ++bit)
            if (mask & (1u << bit))
                ++count;
        return count;
    }

    // 100 + 1 per unique role (100 == no bonus). This is the multiplier the
    // published model applies to the main stat, floor'd at the application.
    constexpr int32 BonusPercent() const
    {
        return 100 + FFXIV_PARTY_BONUS_PERCENT_PER_ROLE * int32(UniqueRoleCount());
    }
};

// --------------------------------------------------------------------------
// traits
// --------------------------------------------------------------------------

// B §F24.2 / I §9.5: the job's damage trait multiplier (100/120/130/150) is a
// property of the job+level, so the only thing the caller decides is whether the
// actor has the trait. Auto-attacks never get it, whatever this says.
struct FfxivTraitState
{
    bool damageTrait = true;
};

// --------------------------------------------------------------------------
// rolls
// --------------------------------------------------------------------------

// Every random draw in the pipeline goes through this, so a test can pin crit,
// direct hit and variance independently.
//
// Roll100() is a uniform integer in [0, 99] compared against a chance in basis
// points as `roll * 100 < chanceBp`. Consequences worth knowing:
//   * a chance below 1 % (e.g. the 0.4 % direct hit of B §F20) is only reachable
//     on roll 0, and rolls 1..99 never proc it -- whole-percent resolution;
//   * a fixed roll of 0 therefore forces crit and DH *on* whenever the chance is
//     non-zero, and 99 forces them off. That is how the tests pin both.
// Retail's roll resolution is UNKNOWN (H.14 measures variance, not the crit roll).
//
// RollVariance(min,max) is a uniform integer in [min, max] inclusive, in percent.
struct FfxivRollSource
{
    virtual ~FfxivRollSource() = default;
    virtual uint32 Roll100() = 0;
    virtual int32 RollVariance(int32 minPercent, int32 maxPercent) = 0;
};

// Deterministic source for tests and fixtures: every draw returns the pinned
// value. See the roll-0/roll-99 note above for forcing crit/DH.
class FfxivFixedRollSource final : public FfxivRollSource
{
public:
    constexpr FfxivFixedRollSource(uint32 roll100, int32 variancePercent)
        : _roll100(roll100), _variancePercent(variancePercent) { }

    uint32 Roll100() override { return _roll100; }
    int32 RollVariance(int32 /*minPercent*/, int32 /*maxPercent*/) override { return _variancePercent; }

private:
    uint32 _roll100;
    int32 _variancePercent;
};

// Seeded source (splitmix64). Same seed -> same sequence on every platform, which
// is what makes a fixture reproducible; it is NOT a claim about retail's RNG.
class FfxivSeededRollSource final : public FfxivRollSource
{
public:
    explicit FfxivSeededRollSource(uint64 seed) : _state(seed) { }

    uint32 Roll100() override;
    int32 RollVariance(int32 minPercent, int32 maxPercent) override;

private:
    uint64 Next();
    uint64 _state;
};

// --------------------------------------------------------------------------
// multiplicative stages that carry no stat (buff 1, buff 2, ...)
// --------------------------------------------------------------------------

// One multiplicative factor, applied as floor(stage * numerator / denominator).
// B §F23.5: buffs stack multiplicatively, one floor per buff (1.3 * 1.3 = 1.69).
struct FfxivPercentFactor
{
    int32 numerator = 100;
    int32 denominator = 100;
};

// --------------------------------------------------------------------------
// stage trace
// --------------------------------------------------------------------------

enum class FfxivCombatStageId : uint8
{
    AttackPowerPercent = 0,     // f(ATK), the percent value that entered D1
    DeterminationPermille,      // f(DET)
    BaseDamage,                 // D1 / H1
    TenacityPermille,           // f(TNC) outgoing
    WeaponDamagePercent,        // f(WD)
    TraitPercent,               // 100 / 120 / 130 / 150
    Stage2Damage,               // D2 / H2
    CriticalRoll,               // the 0..99 draw
    DirectHitRoll,              // the 0..99 draw
    CriticalMultiplierPermille, // 1000 (no crit) or f(CRIT)
    DirectHitMultiplierPercent, // 100 (no DH) or 125
    Stage3Damage,               // D3 / H3
    VariancePercent,            // 95..105 / 97..103
    LevelCorrectionPermille,    // 1000 at or above the target's level
    BuffPercent,                // numerator of each buff factor
    SpeedPermille,              // f(SPD), DoT/HoT only
    DotPlusOne,                 // the published `+ 1` of the DoT branches
    HealingMagicPotencyPercent, // f(HMP), healing only
    AutoAttackPotency,          // 90/80 when the auto-attack helper is used
    FinalDamage,
};

constexpr uint8 FFXIV_COMBAT_MAX_STAGES = 24;

struct FfxivCombatStage
{
    FfxivCombatStageId id;
    int64 value;
};

// Fixed-size, allocation-free stage record. `count <= FFXIV_COMBAT_MAX_STAGES`.
struct FfxivCombatTrace
{
    std::array<FfxivCombatStage, FFXIV_COMBAT_MAX_STAGES> stages{};
    uint8 count = 0;

    void Add(FfxivCombatStageId id, int64 value);
    int64 const* Find(FfxivCombatStageId id) const; // last recorded value, or nullptr
};

struct FfxivDirectDamageResult
{
    int64 value = 0;        // final integer damage
    bool crit = false;
    bool directHit = false;
    int32 critChanceBp = 0; // F9 chance in true basis points (510 == 5.10 %)
    int32 dhChanceBp = 0;   // F10 chance in true basis points
    FfxivCombatTrace trace;
};

struct FfxivHealResult
{
    int64 value = 0;
    bool crit = false;
    int32 critChanceBp = 0;
    FfxivCombatTrace trace;
};

// --------------------------------------------------------------------------
// table / term accessors (pure; the tables validate themselves on first use)
// --------------------------------------------------------------------------

// f(AP) coefficient for a level and role (the number that is 237 / 190 at L100).
// `provenance` receives the row's FfxivCombatProvenance bit: SOURCED, the
// published Lv1-50 band, INTERPOLATED or HELD.
int32 GetFfxivAttackPowerCoefficient(uint8 level, bool tank, uint8* provenance = nullptr);

// f(HMP) divisor for a level (264 at Lv70, 304 at Lv80) + provenance.
int32 GetFfxivHealingPotencyDivisor(uint8 level, uint8* provenance = nullptr);

bool IsFfxivTankJob(uint8 jobId);

// The party role a job counts as for the +1 %/role bonus (B §F24.2). ClassJob
// role 3 covers casters and physical ranged alike, so the job type separates
// them. Returns false for a job with no combat role (DoH/DoL, the neutral row),
// which must not add a role to a party.
bool GetFfxivPartyRole(uint8 jobId, FfxivPartyRole& role);

// The BaseParam row the job's attack power reads (B §F23.13).
uint8 GetFfxivAttackPowerAttribute(uint8 jobId);

// The job's published damage trait multiplier, 100/120/130/150 (B §F24.2).
int32 ComputeFfxivDamageTraitPercent(uint8 jobId);

// The job's auto-attack potency: 90 melee (incl. healer/caster/DNC) / 80 ranged.
int32 ComputeFfxivAutoAttackPotency(uint8 jobId);

// The job's default DoT branch. B §F23.7 splits by damage type; the job is a
// stand-in until actions carry their own type (INFERRED, see the table header).
FfxivDotDamageType GetFfxivDotType(uint8 jobId);

// f(ATK) in percent: max(0, trunc(apMod * (stat - MAIN)/MAIN) + 100). `mainStat`
// is the already party-boosted attack stat; MAIN is the level base main stat.
int32 ComputeFfxivAttackPowerPercent(FfxivDerivedStats const& stats, int32 mainStat, bool tank);

// Party-boosted main stat: floor(stat * (100 + 1*roles) / 100) (B §F24.2).
int32 ComputeFfxivPartyBoostedMainStat(int32 mainStat, FfxivPartyComposition party);

// f(WD) in percent: floor(MAIN * jobMainMod / 1000 + weaponDamage). `jobMainMod`
// is the job's own percentage for the attack attribute from the ClassJob sheet
// (ClassJob c9..c16, 100 = neutral), so a WHM at L100 with WD 132 gets
// floor(440*115/1000) + 132 == 182. Pets use job mod 100 (B §F6).
int32 ComputeFfxivWeaponDamagePercent(FfxivDerivedStats const& stats, int32 weaponDamage);

// f(AUTO) in percent: floor(f(WD) * weaponDelayMs / 3000) (B §F14 / §F23.4).
int32 ComputeFfxivAutoAttackWeaponDamagePercent(FfxivDerivedStats const& stats, int32 weaponDamage,
                                                uint32 weaponDelayMs);

// f(HMP) in percent: floor(100 * (healingMagicPotency - MAIN)/divisor) + 100.
// NOTE: slice 1 does not populate BaseParam 34 (FFXIVStatEngine only carries the
// six base stats, the six secondaries and haste), so `healingMagicPotency` has to
// be supplied by the caller until a stat slice derives it.
int32 ComputeFfxivHealingMagicPotencyPercent(FfxivDerivedStats const& stats, int32 healingMagicPotency);

// f(SPD) in per mille for a DoT/HoT tick: 1000 + trunc(130*(SPD - S)/D). Skill
// Speed for physical, Spell Speed for magical (B §F13). Signed truncation per
// B §B.0 (a below-baseline speed keeps a positive step; floor() would differ by 1).
int32 ComputeFfxivSpeedPermille(FfxivDerivedStats const& stats, FfxivDotDamageType type);

// 1000 - 25*max(0, targetLevel - attackerLevel), floored at 0 (B §F23.5). 1000
// when the attacker is at or above the target's level.
int32 ComputeFfxivLevelCorrectionPermille(uint8 attackerLevel, uint8 targetLevel);

// --------------------------------------------------------------------------
// the pipelines
// --------------------------------------------------------------------------

// Direct damage, published order of B §F15.1 / §F23.5. `potency` is the action's
// potency; `weaponDamage` is f(WD) in percent (see
// ComputeFfxivWeaponDamagePercent). `targetLevel` drives only the level
// correction; it does not change any stat term.
FfxivDirectDamageResult ComputeFfxivDirectDamage(FfxivDerivedStats const& stats, int32 potency,
                                                 int32 weaponDamage, uint8 targetLevel,
                                                 FfxivPartyComposition party, FfxivTraitState traits,
                                                 FfxivRollSource& rolls,
                                                 std::span<FfxivPercentFactor const> buffs = {});

// Direct heal, published order of B §F16.1 / §F23.6 (f(HMP), +-3 %, no DH).
// `weaponDamage` is f(WD) in percent, exactly as for direct damage: the published
// heal chain multiplies by f(WD) too, so the argument cannot be omitted without
// silently assuming weapon damage 0.
FfxivHealResult ComputeFfxivHeal(FfxivDerivedStats const& stats, int32 curePotency, int32 weaponDamage,
                                 uint8 targetLevel, FfxivPartyComposition party, FfxivTraitState traits,
                                 FfxivRollSource& rolls,
                                 std::span<FfxivPercentFactor const> buffs = {});

// One DoT tick. The damage type comes from the caster's job
// (GetFfxivDotType) -- INFERRED; use the explicit overload when the action's own
// type is known. B §F23.7: f(SPD) is inside the tick (its exact position is
// SOURCE-SAYS-UNCONFIRMED), the `+1` lands before variance, variance rolls
// before crit/DH. `weaponDamage` is f(WD) in percent, as for direct damage.
// `trace`, when given, receives the stage record (the value return is the API
// contract; the trace exists so a test can pin the published order).
int64 ComputeFfxivDotTick(FfxivDerivedStats const& stats, int32 potency, int32 weaponDamage,
                          uint8 targetLevel, FfxivPartyComposition party, FfxivTraitState traits,
                          FfxivRollSource& rolls, std::span<FfxivPercentFactor const> buffs = {},
                          FfxivCombatTrace* trace = nullptr);

// Same, with the damage type supplied by the caller (the published split is by
// damage type, not by job).
int64 ComputeFfxivDotTickOfType(FfxivDerivedStats const& stats, int32 potency, int32 weaponDamage,
                                FfxivDotDamageType type, uint8 targetLevel, FfxivPartyComposition party,
                                FfxivTraitState traits, FfxivRollSource& rolls,
                                std::span<FfxivPercentFactor const> buffs = {},
                                FfxivCombatTrace* trace = nullptr);

// Auto-attack: direct damage with the job's auto potency, the auto f(WD) and the
// trait forced to 1.0 (B §F24.2: "never applied to auto-attacks"). B §F14's
// model additionally applies the Skill Speed tick multiplier to auto-attacks;
// that term is NOT applied here (no published direct-damage order includes it) --
// see the report's UNKNOWN list.
FfxivDirectDamageResult ComputeFfxivAutoAttackDamage(FfxivDerivedStats const& stats, uint8 targetLevel,
                                                     FfxivPartyComposition party, FfxivRollSource& rolls,
                                                     int32 weaponDamage,
                                                     uint32 weaponDelayMs = FFXIV_STANDARD_AUTO_ATTACK_DELAY_MS,
                                                     std::span<FfxivPercentFactor const> buffs = {});
} // namespace FFXIV::Combat

#endif // TRINITYCORE_FFXIV_COMBAT_MATH_H
