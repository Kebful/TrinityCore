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

#include "FFXIVClassMana.h"
#include "DatabaseEnv.h"
#include "FFXIV/Jobs/FFXIVJobSlot.h"
#include "Log.h"
#include "Player.h"
#include "Timer.h"
#include <algorithm>

namespace FFXIV::Mana
{
namespace
{
std::vector<FfxivClassManaRow> g_rows; // sorted by classId
}

void SetFfxivClassManaRows(std::vector<FfxivClassManaRow> rows)
{
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](FfxivClassManaRow const& row) { return row.tickMs == 0; }),
               rows.end());
    std::sort(rows.begin(), rows.end(),
              [](FfxivClassManaRow const& a, FfxivClassManaRow const& b) { return a.classId < b.classId; });
    g_rows = std::move(rows);
}

void ClearFfxivClassManaRows()
{
    g_rows.clear();
}

void LoadFfxivClassManaTable()
{
    uint32 const oldMSTime = getMSTime();

    ClearFfxivClassManaRows();
    QueryResult result = WorldDatabase.Query(
        "SELECT classId, tickMs, inCombatPerTick, outOfCombatPerTick FROM ffxiv_class_mana");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 FFXIV MP classes. DB table `ffxiv_class_mana` is missing or empty; "
                                      "every class keeps WoW's mana rules.");
        return;
    }

    std::vector<FfxivClassManaRow> rows;
    do
    {
        Field* fields = result->Fetch();
        FfxivClassManaRow row;
        row.classId = fields[0].GetUInt8();
        row.tickMs = fields[1].GetUInt16();
        row.inCombatPerTick = fields[2].GetUInt16();
        row.outOfCombatPerTick = fields[3].GetUInt16();
        if (row.tickMs == 0)
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_class_mana` has class {} with tickMs 0, ignored.", uint32(row.classId));
            continue;
        }
        rows.push_back(row);
    } while (result->NextRow());

    SetFfxivClassManaRows(std::move(rows));
    TC_LOG_INFO("server.loading", ">> Loaded {} FFXIV MP classes in {} ms", g_rows.size(),
                GetMSTimeDiffToNow(oldMSTime));
}

FfxivClassManaRow const* GetFfxivClassManaRow(uint8 classId)
{
    auto const it = std::lower_bound(g_rows.begin(), g_rows.end(), classId,
        [](FfxivClassManaRow const& row, uint8 id) { return row.classId < id; });
    return it != g_rows.end() && it->classId == classId ? &*it : nullptr;
}

FfxivClassManaRow const* GetFfxivManaRow(Player const& player)
{
    // Only a hosted job may be looked up by job id: the ids of the module classes and of the ARR
    // jobs overlap the ClassJob rows (class 21 is Paladin, ClassJob 21 is Warrior), so a class's own
    // job id must never be read as a class id. A hosted job has an ffxiv_job_slot row; only then is
    // its own row authoritative, and the class's row is the fallback (Sage on Conjurer is mana
    // through the class).
    uint8 const jobId = Jobs::GetFfxivPlayerJobId(player);
    if (Jobs::GetFfxivJobSlotRow(jobId))
        if (FfxivClassManaRow const* row = GetFfxivClassManaRow(jobId))
            return row;
    return GetFfxivClassManaRow(player.GetClass());
}

bool HasFfxivMana(Player const& player)
{
    return GetFfxivManaRow(player) != nullptr;
}

float FfxivManaRegenPerSecond(FfxivClassManaRow const& row, bool inCombat)
{
    if (row.tickMs == 0)
        return 0.f;
    uint16 const perTick = inCombat ? row.inCombatPerTick : row.outOfCombatPerTick;
    return float(perTick) * 1000.f / float(row.tickMs);
}
}
