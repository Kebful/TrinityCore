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

#include "FFXIV/Jobs/FFXIVJobSlot.h"

#include "DatabaseEnv.h"
#include "FFXIV/Stats/FFXIVStatTables.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Timer.h"

#include <unordered_map>

namespace FFXIV::Jobs
{
namespace
{
std::unordered_map<uint32, uint8> s_jobStoneClasses;
}
void LoadFfxivJobSlotTable()
{
    uint32 const start = getMSTime();
    QueryResult result = WorldDatabase.Query("SELECT jobId, classId, itemEntry FROM ffxiv_job_slot");
    s_jobStoneClasses.clear();
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            uint8 const jobId = fields[0].GetUInt8();
            uint8 const classId = fields[1].GetUInt8();
            uint32 const itemEntry = fields[2].GetUInt32();
            ItemTemplate const* item = sObjectMgr->GetItemTemplate(itemEntry);
            if (IsFfxivJobStoneMappingValid(jobId, Stats::GetFfxivClassJobId(classId),
                ClassMaskForId(classId), item ? item->AllowableClass : 0))
            {
                s_jobStoneClasses.emplace(itemEntry, classId);
            }
            else
            {
                TC_LOG_ERROR("sql.sql", "Table `ffxiv_job_slot` has a mismatched job {}, class {} or soul crystal {}.",
                    uint32(jobId), uint32(classId), itemEntry);
            }
        } while (result->NextRow());
    }
    TC_LOG_INFO("server.loading", ">> Validated {} FFXIV soul crystal mappings in {} ms", s_jobStoneClasses.size(), GetMSTimeDiffToNow(start));
}

uint8 GetFfxivJobStoneClassId(uint32 itemEntry)
{
    auto const itr = s_jobStoneClasses.find(itemEntry);
    return itr == s_jobStoneClasses.end() ? 0 : itr->second;
}

uint8 GetFfxivPlayerJobId(Player const& player)
{
    return Stats::GetFfxivClassJobId(player.GetClass());
}
}
