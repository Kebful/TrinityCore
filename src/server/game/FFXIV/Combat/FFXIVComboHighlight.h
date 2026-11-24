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

#ifndef TRINITYCORE_FFXIV_COMBO_HIGHLIGHT_H
#define TRINITYCORE_FFXIV_COMBO_HIGHLIGHT_H

#include "Define.h"
#include "ObjectGuid.h"
#include <cstddef>
#include <vector>

class Player;
class SpellInfo;

/*
 * FFXIV combo highlight: "the next action of the chain you are in" as a wire value.
 *
 * Inputs, both already loaded and owned by the damage slice (FFXIVActionDamage.h) -- this slice adds
 * no table, no column and no data:
 *
 *   ffxiv_action_combo   the chain: actionId -> nextActionId
 *   ffxiv_action_spell   the module's action id -> WoW spell id map
 *   the per-player last action the damage hook records (GetFfxivLastActionId)
 *
 * The client's action bar is WoW's, so the only thing the server can say is which *spell* to draw a
 * highlight around; finding the button, the art and the pulse is the client's job.
 *
 * Rules
 * -----
 * The expected next action is NextFfxivComboActionId(lastAction) and its spell is
 * ResolveFfxivSpellFromActionId(next). Either lookup returning 0 means "nothing to highlight" and the
 * value is sent as 0: the chain is broken (a non-combo action, a step cast with the wrong
 * predecessor), the 12.5 s window has lapsed, the player left combat, the next action of the chain
 * has no spell row, or the tables are empty. There is no fallback to "the spell that was cast" and no
 * client-side computation: absent data shows nothing.
 *
 * The value is sent on change only, plus once at enter-world (which reconciles a client whose UI was
 * reloaded, and clears on a fresh session). A lapsed window is noticed by the per-tick check, because
 * nothing else would run if the player just stops acting.
 *
 * Wire
 * ----
 * Custom-packet sub-opcode 0x7A63 inside the fork's existing custom channel (the channel
 * FFXIVStatsSnapshot.h documents), schema 1:
 *   spellId u32, schema u8      (little-endian, 5 bytes)
 * spellId 0 = "no highlight". Client counterpart:
 * repos/clientdecomp/WoW/Source/Duskhaven/FFXIV/ComboHighlight/FFXIVComboHighlight.h
 */

namespace FFXIV::Combat
{
constexpr uint16 FFXIV_COMBO_HIGHLIGHT_OPCODE = 0x7A63;
constexpr uint8 FFXIV_COMBO_HIGHLIGHT_SCHEMA = 1;
constexpr std::size_t FFXIV_COMBO_HIGHLIGHT_BYTES = 5;

// --- pure ------------------------------------------------------------------------------------

// The spell to highlight after `lastActionId` was the chain's last action, or 0: the next action of
// the chain (NextFfxivComboActionId) mapped through ffxiv_action_spell. 0 when the chain does not
// continue, when the successor has no spell row, or when either table is empty.
uint32 ResolveFfxivComboHighlightSpell(uint32 lastActionId);

// The 0x7A63 payload: spellId then the schema byte. 0 is the "clear" record.
std::vector<uint8> BuildFfxivComboHighlightPayload(uint32 spellId);

// --- state and wire --------------------------------------------------------------------------

// Once per successful cast (Spell::_handle_finish_phase, after the damage hook ran for every target).
// Sends 0x7A63 only when the player's value changes. A spell that is not a mapped FFXIV action
// returns without touching anything.
void OnFfxivActionResolved(Player& caster, SpellInfo const* spellInfo);

// Per-player tick (Player::Update): sends 0 when the recorded action can no longer continue a chain.
// Inert while the player has no highlight.
void UpdateFfxivComboHighlight(Player& player);

// Enter-world: sends the value the server currently expects for this player (0 for a fresh session),
// so a reloaded UI and a re-logged character start from the same state. No traffic while the combo
// table is empty.
void SendFfxivComboHighlight(Player& player);

// The value this player is currently showing, as the server last sent it (0 = none).
uint32 GetFfxivComboHighlightSpell(ObjectGuid player);

void ClearFfxivComboHighlightState(); // tests
} // namespace FFXIV::Combat

#endif // TRINITYCORE_FFXIV_COMBO_HIGHLIGHT_H
