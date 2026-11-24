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

#ifndef TRINITYCORE_FFXIV_ACTION_PROC_H
#define TRINITYCORE_FFXIV_ACTION_PROC_H

#include "Define.h"
#include <vector>

class Player;

/*
 * FFXIV status-gated action replacement ("this action becomes that one while the status is up").
 *
 * The sheet states the relation in two joins, and the data table is that join, not a hand-written
 * Warrior list (release/modules/ffxivhaven/tools/generate-ffxiv-action-procs.py):
 *
 *   ActionIndirection     (action, ClassJob, the action it replaces):
 *                           16465 Inner Chaos      21 Warrior 3549 Fell Cleave
 *                           16463 Chaotic Cyclone  21 Warrior 3550 Decimate
 *                           36924 Primal Wrath     21 Warrior 7389 Inner Release
 *                           36925 Primal Ruination 21 Warrior 25753 Primal Rend
 *                         The replacement's own ActionTransient text says the same thing
 *                         ("※Fell Cleave changes to Inner Chaos when requirements for execution are
 *                         met."), and the generator fails the build when the two disagree.
 *   Action col 46         ActionProcStatus slot -> the Status id that must be on the bearer:
 *                           16465 -> slot 189 -> Status 1897 Nascent Chaos
 *                           25753 -> slot  61 -> Status 2624 Primal Rend Ready
 *
 * World table `ffxiv_action_proc(statusId, jobId, baseActionId, procActionId, minLevel)`, one row per
 * replacement. `minLevel` is the replacement action's own level, so a replacement never applies below
 * the level its action unlocks at.
 *
 * Server rule
 * -----------
 * ResolveFfxivActionProcId: a cast of `baseActionId` while its gate status is on the caster resolves
 * to `procActionId` for the whole cast -- potency, combo, gauge (FFXIV/Gauge/FFXIVJobGauge.h) and the
 * statuses the action applies. The client shows the replacement on the action bar
 * (Spells/FFXIVActionProc.h) and therefore usually sends the proc spell already; resolving again
 * here makes the server correct even without the client swap (a macro, a trainer, an old bar).
 *
 * IsFfxivActionProcGateSatisfied: the proc action itself may not be cast without the status. Only
 * actions that are the *result* of a replacement are gated; the sheet's other "cost type 10" status
 * gates (Thunderhead and the rest) are not enforced yet, because the granting side of most of them is
 * not implemented and enforcing them early would make those actions uncastable.
 *
 * An empty or missing table changes nothing: every lookup misses and every cast keeps its action id.
 */

namespace FFXIV::Combat
{
// One ffxiv_action_proc row.
struct FfxivActionProcRow
{
    uint32 statusId = 0;      // the Status that must be on the caster
    uint8 jobId = 0;          // ClassJob the replacement belongs to
    uint32 baseActionId = 0;  // the action the player casts
    uint32 procActionId = 0;  // the action it resolves to while the status is up
    uint8 minLevel = 0;       // the replacement action's level
};

// --- table access ---------------------------------------------------------------------------

// Startup load from the world database. A missing or empty table logs once and leaves it empty.
void LoadFfxivActionProcTables();

// Replace / drop the rows (the loader and tests); sorted by (jobId, baseActionId, statusId).
void SetFfxivActionProcRows(std::vector<FfxivActionProcRow> rows);
void ClearFfxivActionProcRows();
bool HasFfxivActionProcRows();

// Every row, in table order. Read-only; the client's table is generated from the same sheets.
std::vector<FfxivActionProcRow> const& GetFfxivActionProcRows();

// --- the rule --------------------------------------------------------------------------------

// The action a cast of `actionId` resolves to for this caster: `procActionId` when a row for
// (caster's job, actionId) names a status the caster carries and `level` is at least the row's
// minLevel, else `actionId` unchanged. `actionId` 0 stays 0.
uint32 ResolveFfxivActionProcId(Player const& caster, uint32 actionId, uint8 jobId, uint8 level);

// Pure: the replacement row for (actionId, jobId) at `level`, or nullptr. The player-specific part
// (is the row's status on the caster) is HasFfxivProcStatus; the host test drives this one.
FfxivActionProcRow const* FindFfxivActionProcRow(uint32 actionId, uint8 jobId, uint8 level);

// Pure: `actionId` is the result of a replacement for the job (so it may be status-gated).
bool IsFfxivActionProcResult(uint32 actionId, uint8 jobId);

// True when casting `actionId` is allowed: it is not the result of a replacement, or the status that
// replacement requires is on the caster. An empty table allows everything.
bool IsFfxivActionProcGateSatisfied(Player const& caster, uint32 actionId, uint8 jobId);

// True when `caster` carries the status (the aura the server's ApplyFfxivStatus writes).
bool HasFfxivProcStatus(Player const& caster, uint32 statusId);

} // namespace FFXIV::Combat

#endif // TRINITYCORE_FFXIV_ACTION_PROC_H
