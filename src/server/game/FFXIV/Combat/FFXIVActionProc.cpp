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
 * FFXIV status-gated action replacement: the table, the resolver and the cast gate.
 * See FFXIVActionProc.h for the sheet joins the rows come from.
 */

#include "FFXIVActionProc.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "FFXIV/Status/FFXIVStatus.h"

#include <algorithm>

namespace FFXIV::Combat
{
namespace
{
std::vector<FfxivActionProcRow> g_rows; // sorted by (jobId, baseActionId, statusId)

bool RowLess(FfxivActionProcRow const& left, FfxivActionProcRow const& right)
{
    if (left.jobId != right.jobId)
        return left.jobId < right.jobId;
    if (left.baseActionId != right.baseActionId)
        return left.baseActionId < right.baseActionId;
    return left.statusId < right.statusId;
}
} // namespace

void SetFfxivActionProcRows(std::vector<FfxivActionProcRow> rows)
{
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](FfxivActionProcRow const& row)
        {
            return row.statusId == 0 || row.baseActionId == 0 || row.procActionId == 0
                || row.baseActionId == row.procActionId;
        }), rows.end());
    std::sort(rows.begin(), rows.end(), RowLess);
    g_rows = std::move(rows);
}

void ClearFfxivActionProcRows()
{
    g_rows.clear();
}

bool HasFfxivActionProcRows()
{
    return !g_rows.empty();
}

std::vector<FfxivActionProcRow> const& GetFfxivActionProcRows()
{
    return g_rows;
}

void LoadFfxivActionProcTables()
{
    uint32 const oldMSTime = getMSTime();

    ClearFfxivActionProcRows();

    QueryResult result = WorldDatabase.Query(
        "SELECT statusId, jobId, baseActionId, procActionId, minLevel FROM ffxiv_action_proc");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 FFXIV action replacements. DB table `ffxiv_action_proc` is "
                                      "missing or empty; every action keeps its own id.");
        return;
    }

    std::vector<FfxivActionProcRow> rows;
    do
    {
        Field* fields = result->Fetch();
        FfxivActionProcRow row;
        row.statusId = fields[0].GetUInt32();
        row.jobId = fields[1].GetUInt8();
        row.baseActionId = fields[2].GetUInt32();
        row.procActionId = fields[3].GetUInt32();
        row.minLevel = fields[4].GetUInt8();
        rows.push_back(row);
    } while (result->NextRow());

    std::size_t const loaded = rows.size();
    SetFfxivActionProcRows(std::move(rows));
    if (g_rows.size() != loaded)
        TC_LOG_ERROR("sql.sql", "Table `ffxiv_action_proc`: {} row(s) with a zero id or a self-replacement ignored.",
                     loaded - g_rows.size());

    TC_LOG_INFO("server.loading", ">> Loaded {} FFXIV action replacements in {} ms", g_rows.size(),
                GetMSTimeDiffToNow(oldMSTime));
}

bool HasFfxivProcStatus(Player const& caster, uint32 statusId)
{
    if (statusId == 0)
        return false;
    FFXIV::Status::FfxivStatusRow const* status = FFXIV::Status::GetFfxivStatus(statusId);
    if (!status || status->spellId == 0)
        return false; // the status table does not carry it: no gate, no replacement
    return caster.HasAura(status->spellId);
}

FfxivActionProcRow const* FindFfxivActionProcRow(uint32 actionId, uint8 jobId, uint8 level)
{
    if (actionId == 0 || g_rows.empty())
        return nullptr;

    for (FfxivActionProcRow const& row : g_rows)
    {
        if (row.jobId != jobId || row.baseActionId != actionId)
            continue;
        if (row.minLevel > level)
            continue;
        return &row;
    }
    return nullptr;
}

bool IsFfxivActionProcResult(uint32 actionId, uint8 jobId)
{
    if (actionId == 0 || g_rows.empty())
        return false;
    for (FfxivActionProcRow const& row : g_rows)
        if (row.procActionId == actionId && row.jobId == jobId)
            return true;
    return false;
}

uint32 ResolveFfxivActionProcId(Player const& caster, uint32 actionId, uint8 jobId, uint8 level)
{
    FfxivActionProcRow const* row = FindFfxivActionProcRow(actionId, jobId, level);
    if (!row || !HasFfxivProcStatus(caster, row->statusId))
        return actionId;
    return row->procActionId;
}

bool IsFfxivActionProcGateSatisfied(Player const& caster, uint32 actionId, uint8 jobId)
{
    if (!IsFfxivActionProcResult(actionId, jobId))
        return true; // an action that no replacement produces is not gated here

    for (FfxivActionProcRow const& row : g_rows)
        if (row.procActionId == actionId && row.jobId == jobId
            && HasFfxivProcStatus(caster, row.statusId))
            return true;
    return false;
}

} // namespace FFXIV::Combat
