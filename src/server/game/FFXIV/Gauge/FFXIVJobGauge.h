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

#ifndef TRINITYCORE_FFXIV_JOB_GAUGE_H
#define TRINITYCORE_FFXIV_JOB_GAUGE_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class Player;
class SpellInfo;
class Unit;

/*
 * FFXIV job gauges (GATE 6: the framework; the one gauge today, Warrior's Beast Gauge, is owned by its
 * livescript -- see "Who decides").
 *
 * Data
 * ----
 * Two world tables, written by release/modules/ffxivhaven/datascripts/classes/spells/
 * ffxiv-job-gauges.ts from tools/generate-ffxiv-job-gauges.py:
 *
 *   ffxiv_job_gauge(jobId, gaugeId, name, max)
 *   ffxiv_action_gauge(actionId, jobId, gaugeId, minLevel, delta, comboOnly)
 *
 * gaugeId is the ActionCostType row the job's spenders carry in Action col 32 (Beast Gauge = 22).
 * A gain is the "Increases <gauge> by N" its ActionTransient text states, a spend is -(Action col 33).
 * comboOnly marks a gain printed under "Combo Bonus:". `max` is the module's number: no sheet
 * states it. Row precedence is the potency table's: highest minLevel <= the player's level.
 *
 * An empty or missing table changes nothing: no gauge row, no delta, no state, no packet.
 *
 * Hook
 * ----
 * OnFfxivActionResolved, called once per cast from Spell::_handle_finish_phase -- after every target
 * of the cast went through the action damage hook (FFXIV/Combat/FFXIVActionDamage.h), which this
 * slice reuses and does not duplicate: the spell -> action map is ResolveFfxivActionIdFromSpell and
 * "was this a correct combo" is WasFfxivCorrectComboStep. Finish phase rather than EffectDummy
 * because EffectDummy runs once per target (an area spender would pay N times) and the damage hook
 * returns before resolving an action with no potency (Infuriate).
 *
 * It fires the script event Spell.OnFfxivActionResolved(player, spellInfo, actionId, comboCorrect)
 * (keyed by spell id), then, for a job no script owns, applies the data rows below.
 *
 * Who decides (per job)
 * ---------------------
 * The core stores the value and speaks the protocol; WHO changes it is per job:
 *   - a job a livescript has claimed (ClaimFfxivJobGauge, SetFfxivJobGaugeScriptOwned) is the script's:
 *     the core applies none of its `ffxiv_action_gauge` rows, runs no `CanAffordFfxivActionGauge`
 *     refusal, and charges no WoW rage cost for its actions.
 *     The script moves the value with AddFfxivJobGauge / SetFfxivJobGauge (the Player API below) and
 *     refuses a cast it cannot afford from Spell.OnCheckCast. The data rows stay in the world database;
 *     they are what the client's table and the generator's checks are built from.
 *   - every other job is data-driven exactly as before: rows and the cost gate read by the core.
 * The claim is memory only: a restart without the script leaves every job data-driven.
 *
 * State
 * -----
 * One value per player, tagged with the job it belongs to; reading it as another job gives 0 and
 * writing it as another job starts from 0. Clamped to 0..max. Memory only: not saved, not decayed.
 *
 * Cost (the cast gate)
 * --------------------
 * (A script-owned job skips all of this: CanAffordFfxivActionGauge(Player) is true for it and its script
 * refuses in Spell.OnCheckCast. The rest of this section is the data-driven path.)
 * A spender's cost is its `ffxiv_action_gauge` row's negative delta (a spender row; `comboOnly` rows
 * are gains and never spends). `CanAffordFfxivActionGauge` is the pure check Spell::CheckCast runs
 * before a cast starts; ApplyFfxivActionGauge still clamps at 0 as a safety, but a cast that cannot
 * pay never reaches it. A status that makes an action free is an engine spell mod, not a table here:
 * Inner Release's aura carries ADD_PCT_MODIFIER / SPELLMOD_COST / -100 on the FFXIV Warrior family
 * plus the Fell Cleave/Decimate class-mask bits, so SpellInfo::CalcPowerCost already sees cost 0 and
 * `CanAffordFfxivActionGauge` sees a zero cost. The core reads no waiver row.
 *
 * Rage (the client's display of the gauge)
 * ----------------------------------------
 * The Beast Gauge is the authority and the player's `POWER_RAGE` is a pure display of it: the server
 * writes gauge -> rage and nothing writes rage back (SetFfxivRage below, called from every gauge
 * send). For that to be true, WoW's own rage sources are suppressed for FFXIV module classes
 * (SuppressesWowRage: Unit::RewardRage, Player::Regenerate/UpdatePowerRegen, the energize effects).
 * A job with no gauge row has rage pinned to 0, so the bar can never become a second resource.
 * The stock client then greys an action button whose `Spell.dbc` rage cost is above the player's
 * rage, and recovers when the gauge rises -- no second cost model anywhere.
 *
 * Not in this slice: persistence across logout, decay, and every other job's gauge.
 *
 * Wire
 * ----
 * Custom-packet sub-opcode 0x7A62 (the channel FFXIVStatsSnapshot.h documents), schema 1:
 *   jobId u8, gaugeId u8, value u16, max u16, nameLength u8, name[nameLength], schema u8
 * Little-endian, packed. gaugeId 0 / max 0 / no name means "this job has no gauge" (the client
 * clears its readout). Sent on every change, at enter-world and on every stat rebuild (a job change).
 * Client counterpart: repos/clientdecomp/WoW/Source/Duskhaven/FFXIV/Gauge/FFXIVJobGauge.h
 */

namespace FFXIV::Gauge
{
constexpr uint16 FFXIV_JOB_GAUGE_OPCODE = 0x7A62;
constexpr uint8 FFXIV_JOB_GAUGE_SCHEMA = 1;
constexpr std::size_t FFXIV_JOB_GAUGE_FIXED_BYTES = 8; // every field but the name
constexpr std::size_t FFXIV_JOB_GAUGE_MAX_NAME = 32;   // the table's VARCHAR(32)

// The Beast Gauge row's `max` is the module's 100 (INFERRED from the retail HUD). WoW stores rage in
// tenths -- UNIT_FIELD_MAXPOWER is 1000 and the client displays field/10 (Spell_C_GetPowerDisplayMod
// rage = 10) -- so the mirror writes gauge * FFXIV_RAGE_TENTHS and Spell.dbc rage costs are the gauge
// cost * the same factor.
constexpr uint16 FFXIV_RAGE_MAX = 100;
constexpr uint16 FFXIV_RAGE_TENTHS = 10;
constexpr uint16 FFXIV_RAGE_MAX_FIELD = FFXIV_RAGE_MAX * FFXIV_RAGE_TENTHS; // 1000

// One ffxiv_job_gauge row.
struct FfxivJobGaugeRow
{
    uint8 jobId = 0;
    uint8 gaugeId = 0;
    std::string name;
    uint16 max = 0;
};

// One ffxiv_action_gauge row.
struct FfxivActionGaugeRow
{
    uint32 actionId = 0;
    uint8 jobId = 0;
    uint8 gaugeId = 0;
    uint8 minLevel = 0;
    int16 delta = 0;
    bool comboOnly = false;
};

// --- table access ---------------------------------------------------------------------------

// Startup load from the world database. A missing or empty table logs once and leaves it empty.
void LoadFfxivJobGaugeTables();

// Replace / clear the tables (the loader and tests). Rows whose gauge the gauge table does not
// carry are dropped, as the loader does.
void SetFfxivJobGaugeTables(std::vector<FfxivJobGaugeRow> gauges, std::vector<FfxivActionGaugeRow> deltas);
void ClearFfxivJobGaugeTables();

// The job's gauge, or nullptr when the job has none (or the table is empty).
FfxivJobGaugeRow const* GetFfxivJobGaugeRow(uint8 jobId);

// The delta one resolved cast applies: the plain row plus, on a correct combo, the comboOnly row,
// each the highest minLevel <= level. 0 when the job or action has no row.
int32 ResolveFfxivActionGaugeDelta(uint32 actionId, uint8 jobId, uint8 level, bool comboCorrect);

// The gauge one cast of the action spends: the magnitude of its negative plain delta (the highest
// minLevel <= level), 0 when it costs nothing. A comboOnly row is a gain, never a spend.
uint16 GetFfxivActionGaugeCost(uint32 actionId, uint8 jobId, uint8 level);

// --- state (keyed by player guid and the job the value belongs to) --------------------------

uint16 GetFfxivGauge(ObjectGuid player, uint8 jobId);

// Adds `amount` (negative subtracts), clamped to 0..max. Returns the new value; 0 and no state when
// the job has no gauge.
uint16 AddFfxivGauge(ObjectGuid player, uint8 jobId, int32 amount);

// All or nothing: false, value unchanged, when the gauge holds less than `amount` or the job has no
// gauge.
bool SpendFfxivGauge(ObjectGuid player, uint8 jobId, uint16 amount);

void ClearFfxivGaugeState(); // tests

// Applies ResolveFfxivActionGaugeDelta. True when the value changed.
bool ApplyFfxivActionGauge(ObjectGuid player, uint8 jobId, uint8 level, uint32 actionId,
                           bool comboCorrect);

// Pure: the gauge holds at least the action's cost. True for a free action, for a job without a
// gauge and for a zero cost (an empty table therefore changes nothing).
bool CanAffordFfxivActionGauge(ObjectGuid player, uint32 actionId, uint8 jobId, uint8 level);

// --- script ownership ------------------------------------------------------------------------

// Marks the job's gauge as owned by a livescript (see "Who decides" above). Idempotent; memory only,
// and independent of the tables, so a claim made before or after LoadFfxivJobGaugeTables holds.
void SetFfxivJobGaugeScriptOwned(uint8 jobId, bool owned);
bool IsFfxivJobGaugeScriptOwned(uint8 jobId);
void ClearFfxivJobGaugeScriptOwners(); // tests

// --- Player API (the player's current job and level from GetFFXIVStats) ----------------------

uint16 GetFfxivGauge(Player const& player);
uint16 GetFfxivGaugeMax(Player const& player);        // 0 when the player's job has no gauge
uint16 AddFfxivGauge(Player& player, int32 amount);   // clamps to 0..max; mirrors rage, sends 0x7A62 on a change
uint16 SetFfxivGauge(Player& player, int32 value);    // same, to an absolute value
bool SpendFfxivGauge(Player& player, uint16 amount);  // sends 0x7A62 on success

// The cast gate, from the player's own job and level. False means the cast must not start.
bool CanAffordFfxivActionGauge(Player const& player, uint32 actionId);

// True when the core must not charge the action's WoW rage cost (Spell::CheckPower / TakePower)
// because the player's job is script-owned and the script is the cost authority. The rage bar is
// only the gauge's display; nothing is lost, the script spends through AddFfxivJobGauge. A status
// that makes an action free is an engine spell mod (SpellInfo::CalcPowerCost already returns 0), so
// it needs no branch here.
bool IsFfxivActionRageCostWaived(Player const& player);

// True when the player's current job is script-owned.
bool IsFfxivGaugeScriptOwned(Player const& player);

// --- rage: the gauge's display ---------------------------------------------------------------

// True when the unit is a player of an FFXIV module class and the gauge table is loaded. False for a
// stock class, a creature, and for every unit while the table is empty (the "no data changes
// nothing" rule); the suppression below is this predicate.
bool IsFfxivRageOwner(Unit const& unit);
bool SuppressesWowRage(Unit const& unit);

// Writes the player's current gauge into POWER_RAGE (clamped to FFXIV_RAGE_MAX), marks the client to
// stop predicting rage regen, and returns the value written. One way: the gauge is never read from
// rage. No-op (0) when the unit is not an FFXIV rage owner.
uint16 SyncFfxivRage(Player& player);

// --- the hook ---------------------------------------------------------------------------------

// Spell::_handle_finish_phase, once per successful cast. Not an FFXIV action: returns. Otherwise
// fires Spell.OnFfxivActionResolved (player, spellInfo, actionId, comboCorrect); then, unless the
// caster's job is script-owned, applies the data rows (no gauge or no delta: nothing changes).
void OnFfxivActionResolved(Player& caster, SpellInfo const* spellInfo);

// --- wire ---------------------------------------------------------------------------------------

// Pure: the 0x7A62 payload for `gauge` at `value` (nullptr = "no gauge"). A name above
// FFXIV_JOB_GAUGE_MAX_NAME is truncated, a value above max is clamped.
std::vector<uint8> BuildFfxivJobGaugePayload(FfxivJobGaugeRow const* gauge, uint16 value);

// Sends the player's gauge for the current job (or "no gauge"). Nothing when the gauge table is empty.
void SendFfxivJobGauge(Player& player);
} // namespace FFXIV::Gauge

#endif // TRINITYCORE_FFXIV_JOB_GAUGE_H
