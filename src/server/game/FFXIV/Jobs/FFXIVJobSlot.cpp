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

#include "FFXIVJobSlot.h"
#include "DatabaseEnv.h"
#include "FFXIV/Mana/FFXIVClassMana.h"
#include "FFXIV/Stats/FFXIVStatTables.h"
#include "FFXIV/Stats/FFXIVStatTypes.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "Timer.h"
#include <algorithm>

namespace FFXIV::Jobs
{
namespace
{
std::vector<FfxivJobSlotRow> g_rows; // sorted by jobId

bool IsModuleClass(uint8 classId)
{
    return Stats::FindFfxivClassJobMapRow(classId) != nullptr;
}
}

void SetFfxivJobSlotRows(std::vector<FfxivJobSlotRow> rows)
{
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](FfxivJobSlotRow const& row)
        {
            // A hosted job is not a module class: its host must be. jobId 0 is the sheet's neutral
            // row, never a job, and an item entry of 0 cannot be equipped.
            return row.jobId == 0 || row.itemEntry == 0 || !IsModuleClass(row.classId);
        }), rows.end());
    std::sort(rows.begin(), rows.end(),
        [](FfxivJobSlotRow const& a, FfxivJobSlotRow const& b) { return a.jobId < b.jobId; });
    rows.erase(std::unique(rows.begin(), rows.end(),
        [](FfxivJobSlotRow const& a, FfxivJobSlotRow const& b) { return a.jobId == b.jobId; }), rows.end());
    g_rows = std::move(rows);
}

void ClearFfxivJobSlotRows()
{
    g_rows.clear();
}

void LoadFfxivJobSlotTable()
{
    uint32 const oldMSTime = getMSTime();

    ClearFfxivJobSlotRows();
    QueryResult result = WorldDatabase.Query("SELECT jobId, classId, itemEntry FROM ffxiv_job_slot");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 FFXIV hosted jobs. DB table `ffxiv_job_slot` is missing or "
                                      "empty; every player's job is their class's ClassJob row.");
        return;
    }

    std::vector<FfxivJobSlotRow> rows;
    do
    {
        Field* fields = result->Fetch();
        FfxivJobSlotRow row;
        row.jobId = fields[0].GetUInt8();
        row.classId = fields[1].GetUInt8();
        row.itemEntry = fields[2].GetUInt32();
        if (row.jobId == 0 || row.itemEntry == 0 || !IsModuleClass(row.classId) ||
            row.jobId >= Stats::FFXIV_JOB_COUNT)
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_job_slot` has job {} on class {} with item {}, ignored.",
                         uint32(row.jobId), uint32(row.classId), row.itemEntry);
            continue;
        }
        rows.push_back(row);
    } while (result->NextRow());

    SetFfxivJobSlotRows(std::move(rows));
    TC_LOG_INFO("server.loading", ">> Loaded {} FFXIV hosted jobs in {} ms", g_rows.size(),
                GetMSTimeDiffToNow(oldMSTime));
}

FfxivJobSlotRow const* GetFfxivJobSlotRow(uint8 jobId)
{
    auto const it = std::lower_bound(g_rows.begin(), g_rows.end(), jobId,
        [](FfxivJobSlotRow const& row, uint8 id) { return row.jobId < id; });
    return it != g_rows.end() && it->jobId == jobId ? &*it : nullptr;
}

uint8 ResolveFfxivJobSlot(uint8 classId, uint8 classJobId,
    std::array<uint32, FFXIV_JOB_SLOT_EQUIPPED> const& equippedEntries)
{
    // Only a module class can host a job; anything else (a stock WoW class, 0) keeps its answer.
    if (!IsModuleClass(classId) || classJobId == Stats::FFXIV_NEUTRAL_JOB_ID)
        return classJobId;
    return ResolveFfxivJobSlotFrom(g_rows, classId, classJobId, equippedEntries);
}

uint8 GetFfxivPlayerJobId(Player const& player)
{
    uint8 const classId = player.GetClass();
    std::array<uint32, FFXIV_JOB_SLOT_EQUIPPED> equipped = { 0, 0 };
    if (IsModuleClass(classId))
    {
        uint8 const slots[FFXIV_JOB_SLOT_EQUIPPED] = { EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2 };
        for (uint8 index = 0; index < FFXIV_JOB_SLOT_EQUIPPED; ++index)
            if (Item* item = player.GetItemByPos(INVENTORY_SLOT_BAG_0, slots[index]))
                equipped[index] = item->GetEntry();
    }
    return ResolveFfxivJobSlot(classId, Stats::GetFfxivClassJobId(classId), equipped);
}

uint8 GetFfxivJobDisplayPower(uint8 jobId, uint8 classDisplayPower)
{
    // A hosted MP job has a mana row of its own (ffxiv_class_mana keyed by its job id); the class's
    // row was written for the class, not for this job. Only a hosted job may be looked up by job id
    // (class ids and ClassJob rows overlap), and POWER_MANA is 0 (SharedDefines.h Powers).
    if (GetFfxivJobSlotRow(jobId) && Mana::GetFfxivClassManaRow(jobId))
        return 0;
    return classDisplayPower;
}
}
