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

#ifndef TRINITYCORE_FFXIV_STAT_TYPES_H
#define TRINITYCORE_FFXIV_STAT_TYPES_H

#include "Define.h"
#include <array>

/*
 * FFXIV derived-stat slice 1 (work/stat-research/I-REFRESH-20261123.md §3.2).
 *
 * Read-only typed snapshot. Nothing here is written to a Unit field, a packet or
 * the character database, and no combat path consumes it yet.
 *
 * Units
 * -----
 * rate.*  is true basis points: 10000 == 1.0 == 100 %.
 *         B-FORMULA-REFERENCE F8-F11 publish per-mille numerators (1000 == 1.0)
 *         and F9 publishes the crit chance as a tenth of a percent; the engine
 *         converts once, at the assignment (see FFXIVStatEngine.cpp), so the
 *         whole snapshot speaks one scale. §3.4's table mixes the two scales
 *         (det "1010", tenacity "1005", crit damage "10409"); the value in
 *         true basis points is always x10 of the published per-mille number,
 *         with crit damage = 10000 + (critMultiPerMille - 1000) * 10.
 * gcdMs   is milliseconds, produced by the native stage of F12
 *         (trunc(B*F/1000) then trunc(x*Haste/100)).
 */

namespace FFXIV::Stats
{
// Neutral ids. ClassJob row 0 is a real shipped row whose percentages are all 100,
// and Tribe row 0 carries no modifiers, so both are neutral rather than invented.
//
// The job is no longer a placeholder: Player::RebuildFFXIVStats passes the ClassJob
// row resolved from the player's module class (GetFfxivClassJobId(GetClass()), see
// FFXIVStatTables.h). FFXIV_NEUTRAL_JOB_ID is what a class outside the module (a
// stock WoW class) resolves to. The clan/tribe is still a placeholder: no FFXIV
// tribe is stored on Player yet.
constexpr uint8 FFXIV_NEUTRAL_JOB_ID = 0;
constexpr uint8 FFXIV_NEUTRAL_TRIBE_ID = 0;

// BaseParam row ids (sheet "BaseParam"). 1..73 are the whole parameter space;
// param[0] is unused because no row 0 exists. Names are the shipped sheet's
// English names, sanitised to identifiers.
enum FfxivParam : uint8
{
    FFXIV_PARAM_NONE                         = 0,
    FFXIV_PARAM_STRENGTH                     = 1,
    FFXIV_PARAM_DEXTERITY                    = 2,
    FFXIV_PARAM_VITALITY                     = 3,
    FFXIV_PARAM_INTELLIGENCE                 = 4,
    FFXIV_PARAM_MIND                         = 5,
    FFXIV_PARAM_PIETY                        = 6,
    FFXIV_PARAM_HP                           = 7,
    FFXIV_PARAM_MP                           = 8,
    FFXIV_PARAM_TP                           = 9,
    FFXIV_PARAM_GP                           = 10,
    FFXIV_PARAM_CP                           = 11,
    FFXIV_PARAM_PHYSICAL_DAMAGE              = 12,
    FFXIV_PARAM_MAGIC_DAMAGE                 = 13,
    FFXIV_PARAM_DELAY                        = 14,
    FFXIV_PARAM_ADDITIONAL_EFFECT            = 15,
    FFXIV_PARAM_ATTACK_SPEED                 = 16,
    FFXIV_PARAM_BLOCK_RATE                   = 17,
    FFXIV_PARAM_BLOCK_STRENGTH               = 18,
    FFXIV_PARAM_TENACITY                     = 19,
    FFXIV_PARAM_ATTACK_POWER                 = 20,
    FFXIV_PARAM_DEFENSE                      = 21,
    FFXIV_PARAM_DIRECT_HIT_RATE              = 22,
    FFXIV_PARAM_EVASION                      = 23,
    FFXIV_PARAM_MAGIC_DEFENSE                = 24,
    FFXIV_PARAM_CRITICAL_HIT_POWER           = 25,
    FFXIV_PARAM_CRITICAL_HIT_RESILIENCE      = 26,
    FFXIV_PARAM_CRITICAL_HIT                 = 27,
    FFXIV_PARAM_CRITICAL_HIT_EVASION         = 28,
    FFXIV_PARAM_SLASHING_RESISTANCE          = 29,
    FFXIV_PARAM_PIERCING_RESISTANCE          = 30,
    FFXIV_PARAM_BLUNT_RESISTANCE             = 31,
    FFXIV_PARAM_PROJECTILE_RESISTANCE        = 32,
    FFXIV_PARAM_ATTACK_MAGIC_POTENCY         = 33,
    FFXIV_PARAM_HEALING_MAGIC_POTENCY        = 34,
    FFXIV_PARAM_ENHANCEMENT_MAGIC_POTENCY    = 35,
    FFXIV_PARAM_ELEMENTAL_BONUS              = 36,
    FFXIV_PARAM_FIRE_RESISTANCE              = 37,
    FFXIV_PARAM_ICE_RESISTANCE               = 38,
    FFXIV_PARAM_WIND_RESISTANCE              = 39,
    FFXIV_PARAM_EARTH_RESISTANCE             = 40,
    FFXIV_PARAM_LIGHTNING_RESISTANCE         = 41,
    FFXIV_PARAM_WATER_RESISTANCE             = 42,
    FFXIV_PARAM_MAGIC_RESISTANCE             = 43,
    FFXIV_PARAM_DETERMINATION                = 44,
    FFXIV_PARAM_SKILL_SPEED                  = 45,
    FFXIV_PARAM_SPELL_SPEED                  = 46,
    FFXIV_PARAM_HASTE                        = 47,
    FFXIV_PARAM_MORALE                       = 48,
    FFXIV_PARAM_ENMITY                       = 49,
    FFXIV_PARAM_ENMITY_REDUCTION             = 50,
    FFXIV_PARAM_DESYNTHESIS_SKILL_GAIN       = 51,
    FFXIV_PARAM_EXP_BONUS                    = 52,
    FFXIV_PARAM_REGEN                        = 53,
    FFXIV_PARAM_SPECIAL_ATTRIBUTE            = 54,
    FFXIV_PARAM_MAIN_ATTRIBUTE               = 55,
    FFXIV_PARAM_SECONDARY_ATTRIBUTE          = 56,
    FFXIV_PARAM_SLOW_RESISTANCE              = 57,
    FFXIV_PARAM_PETRIFICATION_RESISTANCE     = 58,
    FFXIV_PARAM_PARALYSIS_RESISTANCE         = 59,
    FFXIV_PARAM_SILENCE_RESISTANCE           = 60,
    FFXIV_PARAM_BLIND_RESISTANCE             = 61,
    FFXIV_PARAM_POISON_RESISTANCE            = 62,
    FFXIV_PARAM_STUN_RESISTANCE              = 63,
    FFXIV_PARAM_SLEEP_RESISTANCE             = 64,
    FFXIV_PARAM_BIND_RESISTANCE              = 65,
    FFXIV_PARAM_HEAVY_RESISTANCE             = 66,
    FFXIV_PARAM_DOOM_RESISTANCE              = 67,
    FFXIV_PARAM_REDUCED_DURABILITY_LOSS      = 68,
    FFXIV_PARAM_INCREASED_SPIRITBOND_GAIN    = 69,
    FFXIV_PARAM_CRAFTSMANSHIP                = 70,
    FFXIV_PARAM_CONTROL                      = 71,
    FFXIV_PARAM_GATHERING                    = 72,
    FFXIV_PARAM_PERCEPTION                   = 73,
    FFXIV_PARAM_COUNT                        = 74
};

// The six base attributes, in the packet order of the ingest (I §1.1).
enum class FfxivBaseStat : uint8
{
    Strength = 0,
    Dexterity,
    Vitality,
    Intelligence,
    Mind,
    Piety,
    Count
};

// snapshot provenance bits
enum FfxivProvenance : uint32
{
    // every equipment contribution was zero (no FFXIV gear model in slice 1)
    FFXIV_PROV_EQUIPMENT_PARAMS_ZERO = 0x1,
    // the level main stat comes from the model level table, not from a sheet
    FFXIV_PROV_BASE_MAIN_MODEL       = 0x2,
    // MAIN / base HP / VIT->HP scalars for this level are interpolated (B F1 gap)
    FFXIV_PROV_LEVEL_INTERPOLATED    = 0x4,
};

// Engine inputs. Slice 1 only ever fills the first three fields from a Player;
// the rest exist so future slices (gear, food, sync) feed the same calculator.
struct FfxivStatInputs
{
    uint8 jobId = 0;            // ClassJob row; 0 is the sheet's neutral 100 % row
    uint8 level = 1;            // 1..100 in the table; gameplay callers cap at 80
    uint8 tribeId = 0;          // Tribe row; 0 is neutral (no tribe bonus)
    uint8 effectiveLevel = 0;   // 0 = same as level (no sync in slice 1)

    // Secondary-stat values. -1 = the naked level base S of F1 (U4: retail
    // floors a naked character's secondaries at S -- INFERRED, unmeasured).
    int32 crit = -1;
    int32 directHit = -1;
    int32 determination = -1;
    int32 tenacity = -1;
    int32 skillSpeed = -1;
    int32 spellSpeed = -1;

    int32 haste = 100;          // BaseParam 47, raw percentage; 100 = no haste

    // Equipment / materia / food contribution per BaseParam row (F2 total()).
    // Always zero in slice 1; when any entry is non-zero the
    // FFXIV_PROV_EQUIPMENT_PARAMS_ZERO bit clears.
    std::array<int32, FFXIV_PARAM_COUNT> equipmentParams{};
};

struct FfxivDerivedStats
{
    uint32 rulesetVersion = 0;          // FNV-1a/32 of the table manifest
    uint8 jobId = 0;
    uint8 level = 0;
    uint8 effectiveLevel = 0;

    int32 base[6] = {};                 // STR DEX VIT INT MND PIE (F2: job % then tribe)
    int32 param[FFXIV_PARAM_COUNT] = {}; // FFXIV parameter space, index = BaseParam row id

    struct
    {
        int32 critBp = 0;               // F9 chance   (590 == 5.90 %)
        int32 critDamageBp = 0;         // F9 damage   (14090 == 1.409x)
        int32 dhBp = 0;                 // F10 chance  (260 == 2.60 %)
        int32 detBp = 0;                // F8          (10100 == 1.010x)
        int32 tncOutBp = 0;             // F11 outgoing(10050 == 1.005x)
        int32 tncInBp = 0;              // F11 incoming(9910 == 0.991x)
        int32 sksBp = 0;                // F12 factor  (9940 == 0.994x speed-up)
        int32 spsBp = 0;                // F12 factor
    } rate;

    uint32 gcdMs = 0;                   // F12, job's own speed stat
    uint32 gcdSksMs = 0;                // F12 with BaseParam 45
    uint32 gcdSpsMs = 0;                // F12 with BaseParam 46
    uint32 castSpeedBp = 0;             // F12 factor applied to casts (job's speed stat)
    int32  mpTick = 0;                  // F4 model, in-combat MP per tick

    uint32 hpMax = 0;                   // F3
    uint32 mpMax = 0;                   // F4: fixed 10000

    uint32 provenance = 0;              // FfxivProvenance bits
};

// True when the bit is present in a snapshot's provenance mask.
inline bool HasProvenance(FfxivDerivedStats const& stats, uint32 bit) { return (stats.provenance & bit) != 0; }
} // namespace FFXIV::Stats

#endif // TRINITYCORE_FFXIV_STAT_TYPES_H
