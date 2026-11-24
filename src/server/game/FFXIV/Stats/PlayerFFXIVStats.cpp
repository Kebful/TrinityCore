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
 * Player-side cache of the FFXIV derived-stat snapshot (slice 1, phase 1a).
 *
 * Nothing in this file writes a Unit field, a packet or the database while the
 * projection gate is off (the shipping default). The snapshot is a pure function
 * of (job, level, tribe, secondaries, equipment), all of which are either inputs
 * the core already has or explicitly absent today:
 *   - FFXIV job:      the player's module class (characters.class) is mapped to a
 *                     ClassJob row by name (FFXIVStatTables kClassJobMap): class
 *                     13 Marauder -> ClassJob 3, class 22 Warrior -> ClassJob 21.
 *                     A class outside the module (stock WoW) resolves to
 *                     ClassJob row 0, the sheet's neutral 100 % row.
 *   - FFXIV clan:     no clan is stored on Player yet, so Tribe row 0 (all
 *                     modifiers zero) is used.
 *   - secondaries:    left at -1, i.e. the naked level base S (U4).
 *   - equipment:      all zero; the snapshot's provenance bit says so.
 *   - level sync:     effectiveLevel == level (U7, not modelled).
 *
 * The HP/MP projection below is log-only while the projection gate
 * (FFXIV/Stats/FFXIVStatProjection.h, kFfxivStatProjectionEnabled) is off: it
 * compares the snapshot against GetMaxHealth()/GetMaxPower(POWER_MANA) and writes
 * neither back. With the gate off -- the shipping default -- no max-HP/MP or stat
 * setter is called anywhere in this path. Flipping the gate adds the phase-1
 * projection (five core stats + the two maxima) and, since phase 1b, the three crit
 * ratings with the core's own agility term removed; see the header for exactly what
 * that does and does not own.
 */

#include "Player.h"
#include "FFXIV/Gauge/FFXIVJobGauge.h"
#include "FFXIV/Stats/FFXIVStatEngine.h"
#include "FFXIV/Stats/FFXIVStatProjection.h"
#include "FFXIV/Stats/FFXIVStatTables.h"
#include "FFXIV/Stats/FFXIVStatsSnapshot.h"
#include "Log.h"
#include <algorithm>
#include <cmath>

void FFXIV::Stats::ApplyFfxivStatsToPlayer(Player& player, FFXIV::Stats::FfxivDerivedStats const& stats)
{
    // The gate is a compile-time constant, so with the shipping default this whole
    // body is a discarded statement and no setter below can ever run. Every write
    // in this file lives inside this `if constexpr` -- including the two crit
    // helpers below, which are lambdas rather than file-local functions precisely so
    // the gate-off object contains no trace of them (no emitted symbol, no
    // relocation).
    if constexpr (kFfxivStatProjectionEnabled)
    {
        // The core's weapon-skill term in Player::UpdateCritPercentage
        // (StatSystem.cpp): (GetWeaponSkillValue(attType) - GetMaxSkillValueForLevel())
        // * 0.04. Measured on the player, never assumed: it is 0 only when the skill
        // line already sits at level * 5, and the phase-1b inversion subtracts
        // whatever it actually is.
        auto const weaponSkillCritTerm = [](Player const& p, WeaponAttackType attType)
        {
            return (int32(p.GetWeaponSkillValue(attType)) - int32(p.GetMaxSkillValueForLevel())) * 0.04f;
        };

        // Pushes one crit rating to `rating` through the core's own rating path.
        // ApplyRatingMod() is the path item and aura ratings use: it moves
        // m_baseRatingValue and calls UpdateRating(), which writes
        // PLAYER_FIELD_COMBAT_RATING_1[cr] and runs the UpdateCritPercentage /
        // UpdateAllSpellCritChances hooks (the percentage part of UpdateRating is
        // skipped when CanModifyStats() is false -- the verification below logs that
        // case). The delta is taken from the field, not from m_baseRatingValue, so
        // the SPELL_AURA_MOD_RATING_FROM_STAT bonus UpdateRating() adds on top of the
        // base value is accounted for: without the 0 clamp the field lands on
        // `rating` on the first pass, and the second pass covers the clamped case.
        auto const writeCritRating = [](Player& p, CombatRating cr, uint32 rating)
        {
            for (uint8 pass = 0; pass < 2; ++pass)
            {
                int32 const delta = int32(rating) - int32(p.GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + AsUnderlyingType(cr)));
                if (delta == 0)
                    break;
                p.ApplyRatingMod(cr, delta, true);
            }
        };

        FfxivWowStats const mapped = MapFfxivStatsToWow(stats);

        player.SetStat(STAT_STRENGTH, mapped.strength);
        player.SetStat(STAT_AGILITY, mapped.agility);
        player.SetStat(STAT_STAMINA, mapped.stamina);
        player.SetStat(STAT_INTELLECT, mapped.intellect);
        player.SetStat(STAT_SPIRIT, mapped.spirit);

        // Core recompute first: it is what carries the group-update flags and the
        // FIRE(Player,OnUpdateMaxHealth/OnUpdateMaxPower) script hooks, and the
        // core expects it to follow a stat change. Then the FFXIV maxima overwrite
        // the result -- UpdateMaxHealth() would otherwise rebuild from
        // GetCreateHealth() + GetHealthBonusFromStamina() and discard F3's hpMax,
        // and UpdateMaxPower() adds GetManaBonusFromIntellect() to the create value.
        player.UpdateMaxHealth();
        player.UpdateMaxPower(POWER_MANA);
        player.SetMaxHealth(mapped.health);
        player.SetMaxPower(POWER_MANA, mapped.mana);

        // Phase 1b, crit chance. Every number here comes from the player or the
        // snapshot: the DBC-backed rating multiplier, the weapon-skill term actually
        // on the skill line, and GetSpellCritFromIntellect(). Nothing is a
        // coefficient of our own -- see FFXIV/Stats/FFXIVStatProjection.h.
        FfxivCritCoreTerms core;
        core.ratingMultiplier[FFXIV_CRIT_SLOT_MELEE] = player.GetRatingMultiplier(CR_CRIT_MELEE);
        core.baseTermPercent[FFXIV_CRIT_SLOT_MELEE] = weaponSkillCritTerm(player, BASE_ATTACK);
        core.ratingMultiplier[FFXIV_CRIT_SLOT_RANGED] = player.GetRatingMultiplier(CR_CRIT_RANGED);
        core.baseTermPercent[FFXIV_CRIT_SLOT_RANGED] = weaponSkillCritTerm(player, RANGED_ATTACK);
        core.ratingMultiplier[FFXIV_CRIT_SLOT_SPELL] = player.GetRatingMultiplier(CR_CRIT_SPELL);
        core.baseTermPercent[FFXIV_CRIT_SLOT_SPELL] = player.GetSpellCritFromIntellect();

        FfxivCritPlan const crit = PlanFfxivCritProjection(stats, core);

        // 1. Remove the core's agility term (GetMeleeCritFromAgility via
        //    UpdateAllCritPercentages) from the three crit PCT mods. Through
        //    SetBaseModPctValue so UpdateBaseModGroup -> UpdateCritPercentage still
        //    runs; a raw m_auraBasePctMod write would leave the fields stale.
        //    OFFHAND_CRIT_PERCENTAGE shares the melee slot: same rating, own mod.
        player.SetBaseModPctValue(CRIT_PERCENTAGE, crit.basePctMod[FFXIV_CRIT_SLOT_MELEE]);
        player.SetBaseModPctValue(OFFHAND_CRIT_PERCENTAGE, crit.basePctMod[FFXIV_CRIT_SLOT_MELEE]);
        player.SetBaseModPctValue(RANGED_CRIT_PERCENTAGE, crit.basePctMod[FFXIV_CRIT_SLOT_RANGED]);

        // 2. The ratings themselves. CR_CRIT_MELEE drives PLAYER_CRIT_PERCENTAGE and
        //    PLAYER_OFFHAND_CRIT_PERCENTAGE, CR_CRIT_RANGED the ranged field,
        //    CR_CRIT_SPELL all spell schools.
        writeCritRating(player, CR_CRIT_MELEE, crit.rating[FFXIV_CRIT_SLOT_MELEE]);
        writeCritRating(player, CR_CRIT_RANGED, crit.rating[FFXIV_CRIT_SLOT_RANGED]);
        writeCritRating(player, CR_CRIT_SPELL, crit.rating[FFXIV_CRIT_SLOT_SPELL]);

        // 3. Verify the round trip through the core's own getters. Half a rating
        //    point is the integer field's own resolution (see the header), plus a
        //    little float slack. A crit aura the FFXIV sheet does not model, a
        //    non-zero offhand/mainhand skill difference, or an unusable multiplier
        //    all surface here instead of silently skewing the roll.
        float const chance = FfxivCritChancePercent(stats);
        float const offhandResidual = weaponSkillCritTerm(player, OFF_ATTACK) - core.baseTermPercent[FFXIV_CRIT_SLOT_MELEE];
        auto const verify = [&player](char const* what, uint16 field, float expected, float multiplier)
        {
            float const actual = player.GetFloatValue(field);
            float const tolerance = 0.5f * multiplier + 0.01f;
            if (std::fabs(actual - expected) > tolerance)
                TC_LOG_WARN("ffxiv.stats",
                            "RebuildFFXIVStats {} ({}): {} crit is {:.4f} % but the FFXIV snapshot asks for {:.4f} % "
                            "(one rating point is {:.4f} %, tolerance {:.4f} %)",
                            player.GetName(), player.GetGUID().ToString(), what, actual, expected,
                            multiplier, tolerance);
        };
        verify("main hand", PLAYER_CRIT_PERCENTAGE, chance, core.ratingMultiplier[FFXIV_CRIT_SLOT_MELEE]);
        // The offhand shares CR_CRIT_MELEE, so it can only match when its own skill
        // term equals the main hand's (true whenever both lines sit at max).
        verify("off hand", PLAYER_OFFHAND_CRIT_PERCENTAGE, chance + offhandResidual, core.ratingMultiplier[FFXIV_CRIT_SLOT_MELEE]);
        verify("ranged", PLAYER_RANGED_CRIT_PERCENTAGE, chance, core.ratingMultiplier[FFXIV_CRIT_SLOT_RANGED]);
        // School 0 is forced to 0% by UpdateSpellCritChance; every real school gets
        // the same value, so checking one is enough.
        verify("spell", PLAYER_SPELL_CRIT_PERCENTAGE1 + 1, chance, core.ratingMultiplier[FFXIV_CRIT_SLOT_SPELL]);
    }
    else
    {
        (void)player;
        (void)stats;
    }
}

void Player::RebuildFFXIVStats()
{
    uint8 const moduleClassId = GetClass();

    FFXIV::Stats::FfxivStatInputs inputs;
    inputs.level = uint8(std::clamp<uint32>(GetLevel(), 1, FFXIV::Stats::FFXIV_MAX_LEVEL));
    inputs.effectiveLevel = inputs.level;
    inputs.jobId = FFXIV::Stats::GetFfxivClassJobId(moduleClassId);
    inputs.tribeId = FFXIV::Stats::FFXIV_NEUTRAL_TRIBE_ID;

    _ffxivStats = FFXIV::Stats::ComputeFfxivStats(inputs);
    _ffxivStatsDirty = false;

    // Phase 1 projection: gated off by default. With the gate false this is a
    // discarded statement, so the rebuild keeps its log-only behaviour: the
    // snapshot is stored, compared against the live fields below and written
    // nowhere. See FFXIV/Stats/FFXIVStatProjection.h.
    if constexpr (FFXIV::Stats::kFfxivStatProjectionEnabled)
    {
        FFXIV::Stats::ApplyFfxivStatsToPlayer(*this, _ffxivStats);
        // Re-entrancy guard: re-clear the flag after the apply. Nothing in the
        // apply path dirties the snapshot (SetStat/SetMaxHealth/SetMaxPower and
        // both UpdateMax* leave the flag alone; only UpdateStats does, from the
        // equip/aura path), so this is the invariant rather than a repair: the
        // snapshot the apply used is the current one, and the apply path never
        // calls back into Rebuild. A FIRE(Player,OnUpdateMaxHealth/OnUpdateMaxPower)
        // hook that changed a stat while the gate is on would have its dirtying
        // undone here; the next UpdateStats (equip/aura) sets it again.
        _ffxivStatsDirty = false;
    }

    // Phase 1a projection check: the FFXIV HP/MP model against the fields the core
    // actually uses, logged once per rebuild and written nowhere. Deltas are
    // snapshot - live, so a negative value means the model is below the live field.
    uint32 const liveHp = GetMaxHealth();
    uint32 const liveMp = GetMaxPower(POWER_MANA);
    FFXIV::Stats::FfxivProjectionDelta const delta =
        FFXIV::Stats::ComputeFfxivProjectionDelta(_ffxivStats, liveHp, liveMp);

    TC_LOG_DEBUG("ffxiv.stats",
                 "RebuildFFXIVStats {} ({}) class {} -> classjob {} ({}) level {} ruleset {:08X} "
                 "projection hp {} (live {}, delta {:+}) mp {} (live {}, delta {:+}) gcd {} ms provenance 0x{:X}",
                 GetName(), GetGUID().ToString(), uint32(moduleClassId), uint32(inputs.jobId),
                 FFXIV::Stats::GetFfxivJobRow(inputs.jobId).abbreviation, uint32(inputs.level),
                 _ffxivStats.rulesetVersion, _ffxivStats.hpMax, liveHp, delta.hp, _ffxivStats.mpMax, liveMp,
                 delta.mp, _ffxivStats.gcdMs, _ffxivStats.provenance);

    // Phase 1c: hand the same snapshot to the client's sheet. Presentation data only --
    // the packet carries no field write and changes no gameplay value, so it is sent
    // whenever the flag is on (the flag only exists so the traffic can be cut without a
    // revert; a client that never receives it shows a blank panel, never a fallback).
    // Pre-world rebuilds (LoadFromDB / InitStatsForLevel) must not send: they run before
    // the client has the player, and SendInitialPacketsAfterAddToMap() sends the first
    // packet instead.
    if (IsInWorld())
    {
        FFXIV::Stats::SendFfxivStatsSnapshot(*this);
        // A rebuild is where a job change lands: the gauge readout follows the new job (or clears).
        FFXIV::Gauge::SendFfxivJobGauge(*this);
    }
}

FFXIV::Stats::FfxivDerivedStats const& Player::GetFFXIVStats() const
{
    if (_ffxivStatsDirty)
        const_cast<Player*>(this)->RebuildFFXIVStats();
    return _ffxivStats;
}
