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

#ifndef TRINITYCORE_FFXIV_JOB_SLOT_H
#define TRINITYCORE_FFXIV_JOB_SLOT_H

#include "Define.h"
#include <array>
#include <vector>

class Player;

/*
 * FFXIV job -> class slot: which job a player actually has.
 *
 * The problem this solves
 * -----------------------
 * The module creates ChrClasses rows for the eight base classes and the ten ARR jobs (12..30), so
 * the server's class -> ClassJob map (FFXIV/Stats/FFXIVStatTables.inc kClassJobMap) can only ever
 * name those jobs. The later jobs (Machinist, Dark Knight, Samurai, Red Mage, Gunbreaker, Dancer,
 * Reaper, Sage, Viper, Pictomancer) have no class slot: their kits are learned on a base class slot
 * (datascripts/classes/spells/jobs, generate-ffxiv-class-spells.py JOBS parent). Without a job of
 * their own a gauge row keyed by their ClassJob id is never read and an MP action they host can
 * never charge MP.
 *
 * Where the choice already lives
 * ------------------------------
 * The choice is the retail one: the Soul of <Job> crystal the player wears. The class switch already
 * stores it, per class slot, in the equipment profile it validates and restores
 * (PlayerClassSwitch.cpp StoredClassProfile::Equipment / ValidateEquipment: the stone's
 * AllowableClass is the base class, so switching to another slot takes it off). This module only
 * reads that equipment -- it adds no player field and no second "current job" state; the class
 * (characters.class) stays the host slot.
 *
 * Data
 * ----
 * One world table, written by release/modules/ffxivhaven/datascripts/items/job-stones.ts from the
 * same JOB_STONES list that creates the items:
 *
 *   ffxiv_job_slot(jobId, classId, itemEntry)
 *
 * A row means "wearing itemEntry on classId makes the player jobId". A missing or empty table
 * changes nothing: every player's job stays GetFfxivClassJobId(GetClass()).
 *
 * What reads it
 * -------------
 *   - Player::RebuildFFXIVStats (FFXIV/Stats/PlayerFFXIVStats.cpp): the snapshot's jobId, so gauges,
 *     action potency, party role and every ClassJob percentage follow the job.
 *   - Unit::UpdateDisplayPower / Player::RefreshFFXIVJob: the power bar of a hosted MP job (Dark
 *     Knight on Marauder shows MP) and the stats rebuild when a stone is equipped.
 *   - FFXIV::Mana::GetFfxivManaRow: an MP job's row is found by job id when the class has none.
 */

namespace FFXIV::Jobs
{
// The two trinket slots the resolver reads (the stones are OTHER_MISC trinkets).
constexpr uint8 FFXIV_JOB_SLOT_EQUIPPED = 2;

// One ffxiv_job_slot row.
struct FfxivJobSlotRow
{
    uint8 jobId = 0;      // ClassJob row of the hosted job (32 Dark Knight)
    uint8 classId = 0;    // module class slot the kit is learned on (13 Marauder)
    uint32 itemEntry = 0; // the Soul of <Job> item template
};

// Reads ffxiv_job_slot. A missing table leaves every player on their class's ClassJob row.
void LoadFfxivJobSlotTable();
// Test/reload seam: replaces the loaded rows. Rows outside the module classes or with jobId 0 are dropped.
void SetFfxivJobSlotRows(std::vector<FfxivJobSlotRow> rows);
void ClearFfxivJobSlotRows();

// The row for a hosted job, or nullptr.
FfxivJobSlotRow const* GetFfxivJobSlotRow(uint8 jobId);

// Pure: the job a (class, equipped trinkets) pair resolves to, over an explicit row list. A matching
// stone's jobId wins, otherwise the class's own row is returned unchanged. Header-only and free of
// engine types so the host test (tests/job-slot/job_slot_test.cpp) can call it with a fixture.
inline uint8 ResolveFfxivJobSlotFrom(std::vector<FfxivJobSlotRow> const& rows, uint8 classId,
    uint8 classJobId, std::array<uint32, FFXIV_JOB_SLOT_EQUIPPED> const& equippedEntries)
{
    for (FfxivJobSlotRow const& row : rows)
    {
        if (row.classId != classId)
            continue;
        for (uint32 entry : equippedEntries)
            if (entry != 0 && entry == row.itemEntry)
                return row.jobId;
    }
    return classJobId;
}

// Pure: the same over the loaded table. `classJobId` is GetFfxivClassJobId(classId).
uint8 ResolveFfxivJobSlot(uint8 classId, uint8 classJobId,
    std::array<uint32, FFXIV_JOB_SLOT_EQUIPPED> const& equippedEntries);

// The player's FFXIV job: their class's ClassJob row, or the hosted job whose stone they wear.
uint8 GetFfxivPlayerJobId(Player const& player);

// The power bar of `jobId`: POWER_MANA when the job has an FFXIV MP row of its own
// (ffxiv_class_mana keyed by the job id, e.g. Dark Knight 32), otherwise the class's own power.
uint8 GetFfxivJobDisplayPower(uint8 jobId, uint8 classDisplayPower);
}

#endif
