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
 * Accessors and integrity checks for the generated combat-constant tables
 * (FFXIVCombatTables.inc). Same discipline as the stat tables: pin the anchors,
 * validate the row shape once, abort on drift.
 */

#include "FFXIVCombatTables.h"
#include "Errors.h"
#include <iterator>
#include <type_traits>

#include "FFXIVCombatTables.inc"

namespace FFXIV::Combat
{
namespace
{
// The anchors the generator pins. If a regenerated table disagrees, the server
// stops instead of shipping different damage. Values: B §F23.3 (Lv1-50 band, the
// 60/70/80/90/100 `other` anchors), B §F24.1 (tank 115 at 80, 190 at 100, from
// xivgear + Amas), data/ffxiv-combat-constants-20261123.tsv for the rest.
struct FfxivAttackPowerAnchor
{
    uint8 level;
    int16 nonTank;
    int16 tank;
};

constexpr FfxivAttackPowerAnchor kAttackPowerAnchors[] =
{
    {  50,  75,  56 },
    {  60, 114,  91 },
    {  70, 125, 105 },
    {  80, 165, 115 },
    {  90, 195, 156 },
    { 100, 237, 190 },
};

struct FfxivHealingPotencyAnchor
{
    uint8 level;
    uint16 divisor;
};

// B §F23.4: f(HMP) = floor(100*(HMP-MAIN)/304)+100 at Lv80, /264 at Lv70.
// L100 is UNKNOWN -- deliberately absent rather than guessed.
constexpr FfxivHealingPotencyAnchor kHealingPotencyAnchors[] =
{
    { 70, 264 },
    { 80, 304 },
};

constexpr bool ProvenanceIsOneOf(uint8 value, uint8 allowed)
{
    return (value != 0) && ((value & ~allowed) == 0);
}

void ValidateOrAbort()
{
    static_assert(std::size(Tables::kAttackPowerTable) == FFXIV_COMBAT_LEVEL_COUNT,
                  "FFXIV combat table: the AP table is not one row per level 1..100");
    static_assert(std::size(Tables::kHealingPotencyTable) == FFXIV_COMBAT_LEVEL_COUNT,
                  "FFXIV combat table: the f(HMP) table is not one row per level 1..100");
    static_assert(std::size(Tables::kJobCombatTable) == FFXIV_COMBAT_JOB_COUNT,
                  "FFXIV combat table: the job table does not list every ClassJob row");

    for (uint32 i = 0; i < FFXIV_COMBAT_LEVEL_COUNT; ++i)
    {
        FfxivAttackPowerRow const& row = Tables::kAttackPowerTable[i];
        if (row.level != i + 1)
            ABORT_MSG("FFXIV combat table: AP row %u has level %u (table is not contiguous 1..%u)",
                      i, uint32(row.level), uint32(FFXIV_COMBAT_LEVEL_COUNT));
        if (row.nonTank <= 0 || row.tank <= 0)
            ABORT_MSG("FFXIV combat table: level %u has a non-positive f(AP) coefficient (%d/%d)",
                      uint32(row.level), int32(row.nonTank), int32(row.tank));
        if (!ProvenanceIsOneOf(row.provenance, FFXIV_COMBAT_AP_SOURCED | FFXIV_COMBAT_AP_PUBLISHED_BAND
                                                | FFXIV_COMBAT_AP_INTERPOLATED | FFXIV_COMBAT_AP_HELD))
            ABORT_MSG("FFXIV combat table: level %u has unexpected non-tank AP provenance 0x%X",
                      uint32(row.level), uint32(row.provenance));
        if (!ProvenanceIsOneOf(row.tankProvenance, FFXIV_COMBAT_AP_SOURCED | FFXIV_COMBAT_AP_INTERPOLATED
                                                  | FFXIV_COMBAT_AP_HELD))
            ABORT_MSG("FFXIV combat table: level %u has unexpected tank AP provenance 0x%X",
                      uint32(row.level), uint32(row.tankProvenance));

        FfxivHealingPotencyRow const& hmp = Tables::kHealingPotencyTable[i];
        if (hmp.level != i + 1)
            ABORT_MSG("FFXIV combat table: f(HMP) row %u has level %u", i, uint32(hmp.level));
        if (hmp.divisor == 0)
            ABORT_MSG("FFXIV combat table: level %u has a zero f(HMP) divisor", uint32(hmp.level));
        if (!ProvenanceIsOneOf(hmp.provenance, FFXIV_COMBAT_HMP_SOURCED | FFXIV_COMBAT_HMP_HELD))
            ABORT_MSG("FFXIV combat table: level %u has unexpected f(HMP) provenance 0x%X",
                      uint32(hmp.level), uint32(hmp.provenance));
    }

    for (FfxivAttackPowerAnchor const& anchor : kAttackPowerAnchors)
    {
        FfxivAttackPowerRow const& row = Tables::kAttackPowerTable[anchor.level - 1];
        if (row.nonTank != anchor.nonTank || row.tank != anchor.tank)
            ABORT_MSG("FFXIV combat table: level %u f(AP) is %d/%d but the anchor requires %d/%d -- "
                      "regenerate deliberately, never hand-edit",
                      uint32(anchor.level), int32(row.nonTank), int32(row.tank),
                      int32(anchor.nonTank), int32(anchor.tank));
        if (row.provenance != FFXIV_COMBAT_AP_SOURCED || row.tankProvenance != FFXIV_COMBAT_AP_SOURCED)
            ABORT_MSG("FFXIV combat table: level %u is an anchor but is not flagged SOURCED (0x%X/0x%X)",
                      uint32(anchor.level), uint32(row.provenance), uint32(row.tankProvenance));
    }

    for (FfxivHealingPotencyAnchor const& anchor : kHealingPotencyAnchors)
    {
        FfxivHealingPotencyRow const& row = Tables::kHealingPotencyTable[anchor.level - 1];
        if (row.divisor != anchor.divisor)
            ABORT_MSG("FFXIV combat table: level %u f(HMP) divisor is %u but the anchor requires %u",
                      uint32(anchor.level), uint32(row.divisor), uint32(anchor.divisor));
        if (row.provenance != FFXIV_COMBAT_HMP_SOURCED)
            ABORT_MSG("FFXIV combat table: level %u is a published f(HMP) anchor but is flagged 0x%X",
                      uint32(anchor.level), uint32(row.provenance));
    }

    for (uint32 i = 0; i < FFXIV_COMBAT_JOB_COUNT; ++i)
    {
        FfxivJobCombatRow const& row = Tables::kJobCombatTable[i];
        if (row.jobId != i)
            ABORT_MSG("FFXIV combat table: job row %u has jobId %u", i, uint32(row.jobId));
        if (!row.abbreviation || !*row.abbreviation)
            ABORT_MSG("FFXIV combat table: job %u has no abbreviation", uint32(row.jobId));
        uint8 const attribute = row.attributeParam;
        if (attribute != 1 && attribute != 2 && attribute != 4 && attribute != 5)
            ABORT_MSG("FFXIV combat table: job %u reads attack power from BaseParam %u, which is not "
                      "STR/DEX/INT/MND (1/2/4/5)", uint32(row.jobId), uint32(attribute));
        if (row.damageTraitPercent != 100 && row.damageTraitPercent != 120
            && row.damageTraitPercent != 130 && row.damageTraitPercent != 150)
            ABORT_MSG("FFXIV combat table: job %u has unknown damage trait %u (B §F24.2 allows "
                      "100/120/130/150)", uint32(row.jobId), uint32(row.damageTraitPercent));
        if (row.autoAttackPotency != 80 && row.autoAttackPotency != 90)
            ABORT_MSG("FFXIV combat table: job %u has auto-attack potency %u (B §F24.2 allows 80/90)",
                      uint32(row.jobId), uint32(row.autoAttackPotency));
        if (row.dotType != FFXIV_DOT_PHYSICAL && row.dotType != FFXIV_DOT_MAGICAL)
            ABORT_MSG("FFXIV combat table: job %u has unknown DoT type %u",
                      uint32(row.jobId), uint32(row.dotType));
    }
}

void EnsureValidated()
{
    static bool const validated = [] { ValidateOrAbort(); return true; }();
    (void)validated;
}
} // namespace

void ValidateFfxivCombatTables() { EnsureValidated(); }

FfxivAttackPowerRow const& GetFfxivAttackPowerRow(uint8 level)
{
    EnsureValidated();
    if (level < 1 || level > FFXIV_COMBAT_LEVEL_COUNT)
        ABORT_MSG("FFXIV combat table: level %u is outside 1..%u", uint32(level),
                  uint32(FFXIV_COMBAT_LEVEL_COUNT));
    return Tables::kAttackPowerTable[level - 1];
}

FfxivHealingPotencyRow const& GetFfxivHealingPotencyRow(uint8 level)
{
    EnsureValidated();
    if (level < 1 || level > FFXIV_COMBAT_LEVEL_COUNT)
        ABORT_MSG("FFXIV combat table: level %u is outside 1..%u", uint32(level),
                  uint32(FFXIV_COMBAT_LEVEL_COUNT));
    return Tables::kHealingPotencyTable[level - 1];
}

FfxivJobCombatRow const& GetFfxivJobCombatRow(uint8 jobId)
{
    EnsureValidated();
    if (jobId >= FFXIV_COMBAT_JOB_COUNT)
        ABORT_MSG("FFXIV combat table: jobId %u is outside 0..%u", uint32(jobId),
                  uint32(FFXIV_COMBAT_JOB_COUNT - 1));
    return Tables::kJobCombatTable[jobId];
}
} // namespace FFXIV::Combat
