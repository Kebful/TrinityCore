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

#ifndef TRINITYCORE_FFXIV_ACTION_CHARGES_H
#define TRINITYCORE_FFXIV_ACTION_CHARGES_H

#include "Define.h"
#include "ObjectGuid.h"
#include <vector>

class Player;
class SpellInfo;

/*
 * FFXIV action charges (per-action "recharge" pools).
 *
 * Why this exists
 * ---------------
 * WotLK has no spell-charge mechanic: there is no SpellCharges.dbc in the client's DBC set and the
 * core's SpellCharges fields are item charges. Retail's action charges are therefore module-owned,
 * like the job gauge: this file owns the pool, the cast gate and the packet; the client's existing
 * SpellCharge map (Duskhaven/Character/CharacterDefines.h) is what the packet fills.
 *
 * Data
 * ----
 * One world table, written by release/modules/ffxivhaven/datascripts/classes/spells/
 * ffxiv-action-charges.ts from tools/generate-ffxiv-action-charges.py:
 *
 *   ffxiv_action_charges(actionId, minLevel, maxCharges, rechargeMs)
 *
 * maxCharges is the action's "Maximum Charges: N" text at (its ClassJob, minLevel), else Action
 * col 43; rechargeMs is Action col 40 x 100 -- the *per-charge* recharge, not a cooldown. A row is
 * emitted at each level where maxCharges changes and the server picks the highest minLevel <= the
 * player's level (the same precedence as ffxiv_action_potency / ffxiv_action_gauge).
 *
 * A charged action has NO flat WoW cooldown: shared/ffxiv-action-spell.ts clears it for every action
 * this table names, so the pool is the only timing. An action whose pool never exceeds one charge is
 * not in the table and keeps the stock cooldown.
 *
 * An empty or missing table changes nothing: no row, no state, no packet, and the gate allows every
 * cast (the same rule every other FFXIV table follows).
 *
 * State
 * -----
 * Per player, per action: the remaining charges and the moment the current recharge started
 * (`anchorMs`). A pool that is full has no entry at all -- absence means full -- so the map only
 * holds pools that are actually recharging and is pruned as they fill.
 *
 * Regeneration is lazy and deterministic: one charge per `rechargeMs` from the anchor, up to
 * maxCharges (AvailableFfxivCharges). The per-tick update only *sends*; it never advances time.
 * Consuming the first charge of a full pool starts the recharge at that instant; consuming while a
 * recharge is already running keeps the anchor, so the next charge arrives on schedule.
 *
 * Memory only: not saved, so a server restart starts every pool full. Entries are keyed by guid with
 * server-clock anchors and are not cleared on logout, so a relog while the server stays up keeps its
 * recharge (and enter-world re-sends it); a pool that has refilled is erased the next time it is read.
 *
 * Hook
 * ----
 *   gate    Spell::CheckCast, next to the gauge gate: a pool at 0 refuses the cast with
 *           SPELL_FAILED_NOT_READY. The resolved replacement's pool is the one read (a proc
 *           replacement resolves to its own action id first), so a replacement at 0 is refused the
 *           same way. No row -> allowed, which is the empty-table rule.
 *   consume Spell::_handle_finish_phase, from OnFfxivActionResolved: the same once-per-successful-cast
 *           point the gauge delta uses, so an area action pays once and a missed action still pays.
 *           This is the only consume site in the tree.
 *
 * Wire
 * ----
 * Custom-packet sub-opcode 0x7A64 (0x7A60..0x7A63 are the stats sheet, the potency column, the job
 * gauge and the combo highlight), schema 1:
 *   count u8, then per entry, ascending by spellId:
 *     spellId u32, current u8, max u8, remainingMs u32, rechargeMs u32
 * Little-endian, packed. The whole snapshot is sent (never deltas): at enter-world, right after a
 * consumed charge, and from the per-tick update when the snapshot a client would draw changes.
 * `remainingMs` is quantised down to a whole second (the tooltip and the bar print whole seconds and
 * the client decays the value locally between packets) so a stable recharge sends one packet a
 * second instead of one per tick. Client counterpart:
 * repos/clientdecomp/WoW/Source/Duskhaven/FFXIV/Charges/FFXIVActionCharges.h
 */

namespace FFXIV::Charges
{
constexpr uint16 FFXIV_ACTION_CHARGES_OPCODE = 0x7A64;
constexpr uint8 FFXIV_ACTION_CHARGES_SCHEMA = 1;
constexpr std::size_t FFXIV_ACTION_CHARGES_MAX_ENTRIES = 255; // the count byte
constexpr std::size_t FFXIV_ACTION_CHARGES_ENTRY_BYTES = 14; // u32 + u8 + u8 + u32 + u32

// One ffxiv_action_charges row.
struct FfxivActionChargeRow
{
    uint32 actionId = 0;
    uint8 minLevel = 0;
    uint8 maxCharges = 1;
    uint32 rechargeMs = 0;
};

// One entry of the 0x7A64 snapshot, already resolved to the WoW spell the client keys its map by.
struct FfxivActionChargeEntry
{
    uint32 spellId = 0;
    uint8 current = 0;
    uint8 max = 0;
    uint32 remainingMs = 0;
    uint32 rechargeMs = 0;
};

// --- table access ---------------------------------------------------------------------------

// Startup load from the world database. A missing or empty table logs once and leaves it empty.
void LoadFfxivActionChargesTable();

// Replace / clear the table (the loader and tests). Rows with a zero action, a zero recharge or
// maxCharges < 1 are dropped; duplicate (actionId, minLevel) keeps the first.
void SetFfxivActionChargeRows(std::vector<FfxivActionChargeRow> rows);
void ClearFfxivActionChargeRows();

// Whether any ffxiv_action_charges row is loaded (the "feature off" predicate).
bool HasFfxivActionChargeRows();

// The row that governs (actionId, level): the highest minLevel <= level. nullptr when the action has
// no pool at that level (or no row at all).
FfxivActionChargeRow const* FindFfxivActionChargeRow(uint32 actionId, uint8 level);

// The level the action's own pool starts at (its lowest minLevel), 0 when it has no row. Used to
// decide whether the player may use the action at all when a snapshot is built.
uint8 GetFfxivActionChargeLevel(uint32 actionId);

// --- state (keyed by player guid and action id; a full pool has no entry) ---------------------

// The charges available right now, after lazily regenerating from the anchor. 0 for an action with
// no row (that is "no pool", never "empty": the gate asks HasFfxivChargeRow first).
uint8 AvailableFfxivCharges(ObjectGuid player, uint32 actionId, uint8 level, uint32 nowMs);

// The same for a player's own job/level.
uint8 AvailableFfxivCharges(Player const& player, uint32 actionId, uint32 nowMs);

// Milliseconds until the next charge, 0 when the pool is full or the action has no row.
uint32 GetFfxivChargeRemainingMs(ObjectGuid player, uint32 actionId, uint8 level, uint32 nowMs);

// True when the action has no pool, or its pool has at least one charge. The cast gate's predicate:
// true is "allowed" for every action this table does not name.
bool HasFfxivChargeAvailable(ObjectGuid player, uint32 actionId, uint8 level, uint32 nowMs);
bool HasFfxivChargeAvailable(Player const& player, uint32 actionId);

// Takes one charge and starts/keeps the recharge. False (and nothing changes) when the action has no
// row or the pool is empty -- the gate refuses those first. The Player overload sends the 0x7A64
// snapshot on success; the guid one is the state change alone (the tests use it).
bool ConsumeFfxivCharge(ObjectGuid player, uint32 actionId, uint8 level, uint32 nowMs);
bool ConsumeFfxivCharge(Player& player, uint32 actionId);

void ClearFfxivActionChargeState(); // tests

// --- the hooks ------------------------------------------------------------------------------

// Spell::_handle_finish_phase, once per successful cast (the gauge's hook point, and the only place a
// charge is consumed). Resolves the spell to its action id and then to a status-gated replacement
// before consuming, so the pool that pays is the one the cast actually used.
void OnFfxivActionResolved(Player& caster, SpellInfo const* spellInfo);

// Player::Update: sends a snapshot when the pool a client would draw changed (a charge came back).
// Inert while the player has no recharging pool.
void UpdateFfxivActionCharges(Player& player);

// Enter-world: force-sends the snapshot so a reloaded UI and a re-logged character start from the
// server's pools. No traffic while the table is empty.
void SendFfxivActionCharges(Player& player);

// --- wire -----------------------------------------------------------------------------------

// Pure: the 0x7A64 payload for these entries, sorted by spellId and truncated to
// FFXIV_ACTION_CHARGES_MAX_ENTRIES. A max of 0 or a current above max is rejected by the builder
// (it would be a server bug), an empty list is the "clear" snapshot (count 0).
std::vector<uint8> BuildFfxivActionChargesPayload(std::vector<FfxivActionChargeEntry> entries);

// Every charged action the player can use (learned and level-qualified), as the snapshot entries.
std::vector<FfxivActionChargeEntry> CollectFfxivActionCharges(Player& player, uint32 nowMs);
} // namespace FFXIV::Charges

#endif // TRINITYCORE_FFXIV_ACTION_CHARGES_H
