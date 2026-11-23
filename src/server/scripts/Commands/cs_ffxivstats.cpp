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

/* ScriptData
Name: ffxivstats_commandscript
%Complete: 100
Comment: Print the read-only FFXIV derived-stat snapshot (slice 1)
Category: commandscripts
EndScriptData */

#include "ScriptMgr.h"
#include "Chat.h"
#include "FFXIV/Stats/FFXIVStatEngine.h"
#include "FFXIV/Stats/FFXIVStatTables.h"
#include "Player.h"
#include "RBAC.h"

using namespace Trinity::ChatCommands;

// Read-only diagnostics. Reuses an existing GM permission on purpose: adding a
// new RBAC_PERM_COMMAND_* id would need a rbac_permissions row, i.e. a database
// change, and slice 1 is explicitly schema-free.
class ffxivstats_commandscript : public CommandScript
{
public:
    ffxivstats_commandscript() : CommandScript("ffxivstats_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "ffxivstats", HandleFFXIVStatsCommand, rbac::RBAC_PERM_COMMAND_SERVER_DEBUG, Console::Yes },
        };
        return commandTable;
    }

    // .ffxivstats [jobId]
    // No argument: the player's cached snapshot (the job resolved from the player's
    // module class through the generated class map) plus a log-only projection line
    // comparing the snapshot's HP/MP against the live fields. With a job id: the
    // same level evaluated for another ClassJob row, without touching the cache and
    // without the live projection (the live fields belong to the player's own job).
    static bool HandleFFXIVStatsCommand(ChatHandler* handler, Optional<uint8> jobId)
    {
        Player* target = handler->getSelectedPlayerOrSelf();
        if (!target)
        {
            handler->SendSysMessage("No player selected and no player attached to this session.");
            return false;
        }

        FFXIV::Stats::FfxivDerivedStats stats = target->GetFFXIVStats();

        uint8 const moduleClassId = target->GetClass();
        if (FFXIV::Stats::FfxivClassJobMapRow const* mapRow =
                FFXIV::Stats::FindFfxivClassJobMapRow(moduleClassId))
            handler->PSendSysMessage("class {} ({}) -> ClassJob {} {} (module class map)",
                                     uint32(mapRow->classId), mapRow->name, uint32(mapRow->classJobId),
                                     FFXIV::Stats::GetFfxivJobRow(mapRow->classJobId).abbreviation);
        else
            handler->PSendSysMessage("class {} is not an FFXIV module class -> ClassJob 0 (neutral)",
                                     uint32(moduleClassId));

        if (jobId)
        {
            if (*jobId >= FFXIV::Stats::FFXIV_JOB_COUNT)
            {
                handler->PSendSysMessage("Unknown FFXIV job id {} (valid 0..{}).", uint32(*jobId),
                                         uint32(FFXIV::Stats::FFXIV_JOB_COUNT - 1));
                return false;
            }

            FFXIV::Stats::FfxivStatInputs inputs;
            inputs.level = stats.level;
            inputs.effectiveLevel = stats.effectiveLevel;
            inputs.jobId = *jobId;
            stats = FFXIV::Stats::ComputeFfxivStats(inputs);
        }

        handler->SendSysMessage(FFXIV::Stats::FormatFfxivStatsBlock(stats));

        // Log-only projection check (phase 1a): the snapshot's HP/MP next to the
        // fields the core actually uses. Read-only -- nothing is written back.
        if (!jobId)
            handler->PSendSysMessage("projection: hp {} (live {}) mp {} (live {})",
                                     stats.hpMax, target->GetMaxHealth(), stats.mpMax,
                                     target->GetMaxPower(POWER_MANA));
        return true;
    }
};

void AddSC_ffxivstats_commandscript()
{
    new ffxivstats_commandscript();
}
