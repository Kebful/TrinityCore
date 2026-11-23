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

#ifndef TRINITYCORE_FFXIV_STAT_PROJECTION_H
#define TRINITYCORE_FFXIV_STAT_PROJECTION_H

#include "FFXIVStatTypes.h"

class Player;

/*
 * FFXIV -> core (WoW) field projection, phase 1: the pure mapping, the gate and
 * the applier. SHIPPING DEFAULT IS OFF.
 *
 * The gate is a compile-time constant, not a cvar and not a database switch, on
 * purpose: flipping it changes which model owns the character sheet (the FFXIV
 * derived stats, not the core's class + item + aura formulas). That is a
 * modelling decision that has to be reviewed and rebuilt, not an operator knob;
 * a runtime switch would let two characters on one shard see different sheets for
 * the same class and level.
 *
 * With kFfxivStatProjectionEnabled == false, Player::RebuildFFXIVStats() keeps its
 * phase-1a behaviour byte for byte: it stores the snapshot and emits the
 * projection-comparison debug line, and nothing else. Every Unit setter in this
 * translation unit sits inside `if constexpr (kFfxivStatProjectionEnabled)`, so a
 * gate-off build contains no reachable write to a stat, max health or max power.
 *
 * Mapping (MapFfxivStatsToWow, pure and constexpr):
 *
 *   FFXIV base[]         WoW field                 note
 *   -------------------  -------------------------  ---------------------------
 *   base[STRENGTH]       STAT_STRENGTH              F2 (job % then tribe)
 *   base[DEXTERITY]      STAT_AGILITY               F2
 *   base[VITALITY]       STAT_STAMINA               F2; also feeds the core's
 *                                                   GetHealthBonusFromStamina()
 *   base[INTELLIGENCE]   STAT_INTELLECT             F2; also feeds the core's
 *                                                   GetManaBonusFromIntellect()
 *   base[MIND]           STAT_SPIRIT                F2
 *   base[PIETY]          -- none --                 see below
 *   hpMax (F3)           max health
 *   mpMax (F4, 10000)    max mana
 *
 * Piety has no core field, so it is dropped. The Mind/Piety collision is the
 * reason it cannot simply be folded elsewhere: in FFXIV, MND is the healing and
 * caster-main stat while PIE scales the MP pool and MP regen; in this core the
 * regen role belongs to Spirit, which MND already claims, and the mana pool is
 * derived from Intellect by GetManaBonusFromIntellect(). PIE's one measurable
 * effect today (the F4 MP tick, snapshot field mpTick) therefore has no WoW
 * counterpart and is not projected. Folding PIE into Spirit or Intellect would
 * count one FFXIV concept twice and is not done here.
 *
 * Deliberately NOT projected in this cut:
 *   - PIE (above).
 *   - Secondaries other than crit chance: direct hit / determination / tenacity /
 *     skill speed / spell speed have no core stat field. The speed pair is not
 *     synthesised as a rating either: even one that reproduced the FFXIV recast
 *     would not reproduce it *through* the core, whose cast/recast minima differ
 *     from the FFXIV floor of 1500 ms (FFXIV_MIN_GCD_MS). That waits for the
 *     combat-math slice, which owns the percentages directly.
 *   - Crit DAMAGE and the crit-taken ratings: see "Phase 1b" below.
 *   - WoW item/aura contributions are NOT stripped: SetStat() overwrites the value
 *     UpdateStats() derived from base + item + aura modifiers, while those
 *     modifiers stay loaded and the next UpdateStats()/aura refresh recomputes the
 *     field from them and wins. Making FFXIV own the sheet means teaching the stat
 *     system to ignore (or fold in) those contributions; that is the next cut.
 *
 * ---------------------------------------------------------------------------
 * Phase 1b: crit chance (CR_CRIT_MELEE / CR_CRIT_RANGED / CR_CRIT_SPELL)
 * ---------------------------------------------------------------------------
 *
 * Why a rating CAN carry the FFXIV percentage exactly: the core converts a rating
 * to a percentage in Player::GetRatingBonusValue() as
 * `PLAYER_FIELD_COMBAT_RATING_1[cr] * GetRatingMultiplier(cr)`, and that multiplier
 * is `classRating->Data / Rating->Data` from two DBC stores the server has loaded
 * (sGtCombatRatingsStore, level-indexed; sGtOCTClassCombatRatingScalarStore,
 * class-indexed; Player.cpp). Both factors are known at apply time, so the rating
 * that reproduces a chance is a division -- no coefficient is invented here.
 *
 * What the core adds on top of the rating, and what this projection does about it:
 *
 *   1. The agility term. UpdateAllCritPercentages() (StatSystem.cpp) writes
 *      GetMeleeCritFromAgility() into the base PCT mod of CRIT_PERCENTAGE,
 *      OFFHAND_CRIT_PERCENTAGE and RANGED_CRIT_PERCENTAGE, and
 *      UpdateCritPercentage() adds that mod to the field. FFXIV crit does not come
 *      from DEX, so the applier ZEROES those three mods before writing ratings.
 *      Zeroing goes through SetBaseModPctValue() so the mod's own refresh
 *      (UpdateBaseModGroup -> UpdateCritPercentage) runs.
 *
 *   2. The weapon-skill term. UpdateCritPercentage() also adds
 *      `(GetWeaponSkillValue(attType) - GetMaxSkillValueForLevel()) * 0.04`. It is
 *      NOT neutralised by zeroing a skill line: that line also drives miss/dodge
 *      and the equipped-weapon check, and zeroing it would be a -1-per-level lie
 *      that is larger than the term. The applier instead measures the term on the
 *      live player and SUBTRACTS it in the inversion, so the sum lands on the
 *      snapshot (FfxivCritCoreTerms::baseTermPercent).
 *
 *   3. The intellect term. UpdateSpellCritChance() starts from
 *      GetSpellCritFromIntellect(). Like the skill term it cannot be zeroed
 *      without falsifying a projected stat (INT is a real FFXIV attribute), so the
 *      spell rating is inverted against it.
 *
 *   4. Crit auras. SPELL_AURA_MOD_WEAPON_CRIT_PERCENT / MOD_CRIT_PCT flatten into
 *      the same base FLAT mod, and MOD_SPELL_CRIT_CHANCE(_SCHOOL) into the spell
 *      field. These are NOT compensated: they are stock WoW content the FFXIV
 *      sheet does not model, and enumerating them to cancel them would make an
 *      FFXIV crit chance depend on which auras happen to be up. They stay
 *      additive; the apply path verifies the field it produced and logs a warning
 *      when it disagrees with the snapshot.
 *
 * Resolution. The rating field is a uint32 of whole points, so one point is worth
 * `multiplier` percent of crit; rounding to the nearest point leaves at most half
 * a point of error. With the shipped gtCombatRatings rows (read 2026-11-23) that is
 * 0.93 % at L1, 0.044 % at L50, 0.0109 % at L80 and 0.0025 % at L100 -- which is
 * why "within 0.01 %" is a level-dependent bound, not a constant: it holds at L100
 * for any vector, at L80 only when the rounded residual happens to be small, and
 * never at L1. Anything tighter needs a percentage field, not a rating.
 *
 * The representation also floors at 0 %: UpdateCritPercentage() clamps the field at
 * zero, so a snapshot chance of 0 % or below becomes 0 and the verification reports
 * the difference. Below-zero chances are not reachable today (a naked character's
 * crit parameter equals the level base S, so F9's critDelta is 0 and the chance is
 * exactly 5.00 %), but a gear model that could push crit below S would hit this.
 *
 * CR_CRIT_TAKEN_*. Left untouched. They are the chance for THIS player to be crit
 * (Unit::RollMeleeOutcomeAgainst, Unit::SpellCriticalBonus), not the player's own
 * crit chance, so they are not a source of FFXIV crit; a fresh FFXIV character has
 * them at 0 and nothing writes them without a stock item/aura. Whether FFXIV's
 * incoming-crit model wants them is phase 4 (UNKNOWN).
 *
 * Crit DAMAGE is not touched here. WoW crits for 200 % (CRITICAL_HIT_BONUS); FFXIV
 * crits for 1.400x at 0 crit rating and scales with Critical Hit Power -- the
 * snapshot's own rate.critDamageBp (14000 == 1.400x). That is a multiplier on the
 * damage path, not a rating, and it belongs to the phase-4 damage slice. Nothing
 * in this file reads rate.critDamageBp; do not fold it into the rating inversion.
 *
 * Known gaps for the next cut:
 *   - A STAT_AGILITY change (Player::UpdateStats) and UpdateAllStats() both call
 *     UpdateAllCritPercentages(), which re-installs the agility term and recomputes
 *     the three percentages from it. UpdateStats marks the snapshot dirty, so the
 *     next GetFFXIVStats()/Rebuild re-applies this projection, but in between the
 *     crit chance carries the agility term again. Removing that term for good is
 *     the same phase-2 work as the item/aura contributions above.
 *   - The rating itself survives: UpdateAllRatings() rewrites the field from
 *     m_baseRatingValue[], and the projection leaves that at `rating - the
 *     SPELL_AURA_MOD_RATING_FROM_STAT bonus`, so the field is reproduced exactly.
 *     But an item/aura rating change (Player::ApplyRatingMod, from the equip path)
 *     adds its delta on top of that projected base, so stock crit rating still
 *     stacks with the FFXIV chance until those contributions are stripped too.
 */

namespace FFXIV::Stats
{
// SHIPPING DEFAULT. Flipping this to true makes the FFXIV sheet own the
// character's five core stats and its two maxima; the core's own derivations stop
// being authoritative. Do it only together with the phase-2 work (stripping the
// core's item/aura stat contributions) and a rebuild -- nothing here is a cvar or
// a database switch, so a running server will never pick it up from a config file.
constexpr bool kFfxivStatProjectionEnabled = false;

// The core fields the FFXIV sheet can fill today. Health/mana are the projected
// maxima (F3/F4), not the live values.
struct FfxivWowStats
{
    int32 strength = 0;     // <- base STR
    int32 agility = 0;      // <- base DEX
    int32 intellect = 0;    // <- base INT
    int32 spirit = 0;       // <- base MND
    int32 stamina = 0;      // <- base VIT
    uint32 health = 0;      // <- hpMax
    uint32 mana = 0;        // <- mpMax
};

constexpr int32 FfxivBaseValue(FfxivDerivedStats const& stats, FfxivBaseStat which)
{
    return stats.base[static_cast<uint8>(which)];
}

constexpr FfxivWowStats MapFfxivStatsToWow(FfxivDerivedStats const& stats)
{
    FfxivWowStats mapped;
    mapped.strength  = FfxivBaseValue(stats, FfxivBaseStat::Strength);
    mapped.agility   = FfxivBaseValue(stats, FfxivBaseStat::Dexterity);
    mapped.stamina   = FfxivBaseValue(stats, FfxivBaseStat::Vitality);
    mapped.intellect = FfxivBaseValue(stats, FfxivBaseStat::Intelligence);
    mapped.spirit    = FfxivBaseValue(stats, FfxivBaseStat::Mind);
    // Piety is intentionally absent: no core field exists for it (see above).
    mapped.health    = stats.hpMax;
    mapped.mana      = stats.mpMax;
    return mapped;
}

// The numbers the phase-1a log line compares: snapshot minus live, so a negative
// delta means the FFXIV model is below the field the core currently uses. Kept as
// a pure function so the log path's arithmetic can be pinned by a test without a
// Player.
struct FfxivProjectionDelta
{
    int64 hp = 0;
    int64 mp = 0;
};

constexpr FfxivProjectionDelta ComputeFfxivProjectionDelta(FfxivDerivedStats const& stats, uint32 liveHp, uint32 liveMp)
{
    FfxivProjectionDelta delta;
    delta.hp = int64(stats.hpMax) - int64(liveHp);
    delta.mp = int64(stats.mpMax) - int64(liveMp);
    return delta;
}

// ---------------------------------------------------------------------------
// Crit chance, the pure part. Split out exactly like MapFfxivStatsToWow /
// ComputeFfxivProjectionDelta: the applier needs a live Player, so everything that
// *decides* what to write is a constexpr function of the snapshot and the core's
// own factors, and the test pins those decisions without a Player.
// ---------------------------------------------------------------------------

// Slots, in the order the applier writes them:
//   0 melee   -> CR_CRIT_MELEE   + CRIT_PERCENTAGE (and OFFHAND_CRIT_PERCENTAGE,
//                which shares the rating but has its own base mod and skill term)
//   1 ranged  -> CR_CRIT_RANGED  + RANGED_CRIT_PERCENTAGE
//   2 spell   -> CR_CRIT_SPELL   (no base mod group; intellect is a base term)
constexpr uint8 FFXIV_CRIT_SLOT_MELEE  = 0;
constexpr uint8 FFXIV_CRIT_SLOT_RANGED = 1;
constexpr uint8 FFXIV_CRIT_SLOT_SPELL  = 2;
constexpr uint8 FFXIV_CRIT_SLOT_COUNT  = 3;

// Ceiling for a projected rating. Player::m_baseRatingValue[] is int16 (Player.h) and
// ApplyRatingMod() -- the path this projection writes through -- accumulates into it,
// so a rating above INT16_MAX would overflow the core's own storage before it ever
// reached the field. INT16_MAX still covers every real case with room: reproducing a
// 100 % chance needs 54 points at level 1 (the coarsest multiplier in the shipped
// table, 1.857 %/point) and 19 849 at level 100 (0.005 %/point). The clamp therefore
// only bites on a corrupt or zero-ish DBC row.
constexpr uint32 kFfxivMaxProjectedRating = 32767u;

// The snapshot's crit chance as a percentage. rate.critBp is true basis points
// (10000 == 100 %), so this only changes scale; the percentage itself is never
// recomputed here.
constexpr float FfxivCritChancePercent(FfxivDerivedStats const& stats)
{
    return float(stats.rate.critBp) / 100.0f;
}

// Inverse of `rating * ratingMultiplier + baseTermPercent`: the whole rating point
// closest to the percentage the snapshot asks for. Returns 0 when no rating can
// express it (multiplier <= 0 or NaN, or the base term already exceeds the target),
// and clamps to kFfxivMaxProjectedRating. Rounding is `+ 0.5` on a non-negative
// value, i.e. round-half-up, so the worst residual is half a rating point.
constexpr uint32 FfxivRatingForPercent(float percent, float ratingMultiplier, float baseTermPercent)
{
    // NaN fails both comparisons, which is deliberate: an unusable multiplier means
    // "no rating value can produce a percentage", not "pick one".
    if (!(ratingMultiplier > 0.0f))
        return 0;

    float const percentFromRating = percent - baseTermPercent;
    if (!(percentFromRating > 0.0f))
        return 0;

    float const scaled = percentFromRating / ratingMultiplier;
    if (!(scaled < float(kFfxivMaxProjectedRating)))     // also catches inf/NaN
        return kFfxivMaxProjectedRating;

    return uint32(scaled + 0.5f);
}

// The core-side inputs of the inversion, one entry per slot. `ratingMultiplier` is
// Player::GetRatingMultiplier(cr) for the slot's rating; `baseTermPercent` is the
// non-rating term the core's own formula adds for that slot's attack type -- the
// weapon-skill term for melee/ranged, GetSpellCritFromIntellect() for spell, 0 if
// the core adds none. Auras are deliberately absent (see phase 1b above).
struct FfxivCritCoreTerms
{
    float ratingMultiplier[FFXIV_CRIT_SLOT_COUNT] = {};
    float baseTermPercent[FFXIV_CRIT_SLOT_COUNT] = {};
};

// What the applier writes for the three ratings, plus the three base PCT mod values
// it writes first (always 0 -- the core's agility term, which FFXIV does not have).
// Compensated terms are carried for the log: they are what the inversion removed.
struct FfxivCritPlan
{
    uint32 rating[FFXIV_CRIT_SLOT_COUNT] = {};
    float basePctMod[FFXIV_CRIT_SLOT_COUNT] = {};
    float compensatedPercent[FFXIV_CRIT_SLOT_COUNT] = {};
};

constexpr FfxivCritPlan PlanFfxivCritProjection(FfxivDerivedStats const& stats, FfxivCritCoreTerms const& core)
{
    FfxivCritPlan plan;
    float const chance = FfxivCritChancePercent(stats);
    for (uint8 slot = 0; slot < FFXIV_CRIT_SLOT_COUNT; ++slot)
    {
        plan.rating[slot] = FfxivRatingForPercent(chance, core.ratingMultiplier[slot], core.baseTermPercent[slot]);
        plan.basePctMod[slot] = 0.0f;
        plan.compensatedPercent[slot] = core.baseTermPercent[slot];
    }
    return plan;
}

// Writes the five mapped core stats, the two projected maxima and the three crit
// ratings onto the player. A no-op unless kFfxivStatProjectionEnabled. The snapshot
// is passed in rather than re-read through Player::GetFFXIVStats() so this path can
// never trigger the lazy rebuild it is called from.
//
// Order inside: the five SetStat() calls, then the core's own UpdateMaxHealth()/
// UpdateMaxPower() (so their side effects -- group updates and the script hooks
// FIRE(Player,OnUpdateMaxHealth/OnUpdateMaxPower) -- still run after a stat
// change), then SetMaxHealth()/SetMaxPower(), then the crit projection. The maxima
// come late because Player::UpdateMaxHealth() recomputes from GetCreateHealth() +
// GetHealthBonusFromStamina() and would otherwise discard hpMax (F3), and
// UpdateMaxPower() adds GetManaBonusFromIntellect() on top of the create value. The
// FFXIV sheet owns the two maxima, so it wins. Crit goes last so nothing in this
// function can recompute a percentage after it was written.
//
// Crit is written in two passes: the base PCT mods are zeroed first, then the
// ratings are pushed through Player::ApplyRatingMod(), the core's own rating path
// (it updates m_baseRatingValue and calls UpdateRating(), which writes
// PLAYER_FIELD_COMBAT_RATING_1 and runs UpdateCritPercentage /
// UpdateAllSpellCritChances). A raw SetUInt32 on the rating field would leave
// PLAYER_CRIT_PERCENTAGE and the spell crit fields stale. The function then re-reads
// those fields and logs a warning if they disagree with the snapshot by more than
// half a rating point -- the check runs against the core's own getters, so a
// mismatch between the inversion and GetRatingMultiplier shows up in the log
// instead of silently skewing crit.
//
// Re-entrancy: none of SetStat/SetMaxHealth/SetMaxPower/UpdateMaxHealth/
// UpdateMaxPower/SetBaseModPctValue/ApplyRatingMod touches Player::_ffxivStatsDirty
// (only UpdateStats() does, from the equip/aura path), so the caller clears the
// dirty flag after this returns and no rebuild can be triggered from here.
void ApplyFfxivStatsToPlayer(Player& player, FfxivDerivedStats const& stats);
} // namespace FFXIV::Stats

#endif // TRINITYCORE_FFXIV_STAT_PROJECTION_H
