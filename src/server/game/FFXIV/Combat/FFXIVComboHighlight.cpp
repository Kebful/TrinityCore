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
 * FFXIV combo highlight: the next chain action's spell id, and the 0x7A63 packet.
 *
 * The chain state and the two tables are the damage slice's (FFXIV/Combat/FFXIVActionDamage.h):
 * this file only reads them and turns the answer into one wire value. Nothing is stored beyond the
 * last value each player was sent -- the records are memory only, like the gauge's.
 */

#include "FFXIVComboHighlight.h"

#include "CustomPacketDefines.h"
#include "CustomPacketWrite.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "SpellInfo.h"
#include "WorldPacket.h"
#include "FFXIV/Combat/FFXIVActionDamage.h"

#include <limits>
#include <unordered_map>

namespace FFXIV::Combat
{
namespace
{
// One value per player, nonzero entries only: an absent row IS "no highlight".
std::unordered_map<ObjectGuid, uint32> g_highlight;

// The snapshot's sender is private to FFXIVStatsSnapshot.cpp; this is the same fragment-and-send.
void SendCustomPayload(Player& player, uint16 opcode, std::vector<uint8> const& payload)
{
    if (payload.empty() || payload.size() > std::numeric_limits<totalSize_t>::max())
        return;

    CustomPacketWrite writer(opcode, MAX_FRAGMENT_SIZE, 0);
    writer.WriteBytes(static_cast<totalSize_t>(payload.size()), reinterpret_cast<char const*>(payload.data()));
    for (CustomPacketChunk& chunk : writer.buildMessages())
    {
        WorldPacket packet(SERVER_TO_CLIENT_OPCODE, chunk.FullSize());
        packet.append(reinterpret_cast<uint8 const*>(chunk.Data()), chunk.FullSize());
        player.SendDirectMessage(&packet);
    }
    writer.Destroy();
}

void SendHighlight(Player& player, uint32 spellId)
{
    SendCustomPayload(player, FFXIV_COMBO_HIGHLIGHT_OPCODE, BuildFfxivComboHighlightPayload(spellId));
}

// The value this player should be showing right now. The chain is the FFXIV action window, not WoW
// combat: a neutral BattleNpc dummy is a legal target but cannot set WoW's combat state, so the
// combat flag must not clear the highlight.
uint32 DesiredHighlight(Player const& player, uint32 nowMs)
{
    return ResolveFfxivComboHighlightSpell(GetFfxivLastActionId(player.GetGUID(), nowMs));
}

// Stores `spellId` and sends it when it differs from what this player was last sent.
void StoreAndSend(Player& player, uint32 spellId)
{
    if (GetFfxivComboHighlightSpell(player.GetGUID()) == spellId)
        return;

    if (spellId == 0)
        g_highlight.erase(player.GetGUID());
    else
        g_highlight[player.GetGUID()] = spellId;

    TC_LOG_DEBUG("spells", "FFXIV combo highlight {} -> spell {}", player.GetName(), spellId);
    SendHighlight(player, spellId);
}
} // namespace

// --------------------------------------------------------------------------
// pure
// --------------------------------------------------------------------------

uint32 ResolveFfxivComboHighlightSpell(uint32 lastActionId)
{
    if (lastActionId == 0)
        return 0;

    // A chain action the module created no spell for cannot be drawn on a bar. Heavy Swing's sheet
    // successors start with Skull Sunder 35, which is not a generated spell, so the old
    // "lowest successor" lookup returned 0 and the ring never appeared. Skip to the first successor
    // that has a spell row (Maim 37), which is the chain a Marauder/Warrior actually has on the bar.
    return NextFfxivComboActionSpell(lastActionId);
}

std::vector<uint8> BuildFfxivComboHighlightPayload(uint32 spellId)
{
    std::vector<uint8> out;
    out.reserve(FFXIV_COMBO_HIGHLIGHT_BYTES);
    out.push_back(uint8(spellId & 0xFF));
    out.push_back(uint8((spellId >> 8) & 0xFF));
    out.push_back(uint8((spellId >> 16) & 0xFF));
    out.push_back(uint8((spellId >> 24) & 0xFF));
    out.push_back(FFXIV_COMBO_HIGHLIGHT_SCHEMA);
    return out;
}

// --------------------------------------------------------------------------
// state and wire
// --------------------------------------------------------------------------

void OnFfxivActionResolved(Player& caster, SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return;
    // Not an FFXIV action: the chain is not the caster's, and a WoW spell must not clear it.
    if (ResolveFfxivActionIdFromSpell(spellInfo->Id) == 0)
        return;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    uint32 const lastActionId = GetFfxivLastActionId(caster.GetGUID(), nowMs);
    TC_LOG_ERROR("sql.sql", "[ffxiv-debug] highlight last {} combat {} -> spell {}",
                lastActionId, caster.IsInCombat(),
                ResolveFfxivComboHighlightSpell(lastActionId));

    // The damage hook ran for every target of this cast in this same tick, so the recorded last
    // action already describes the chain after this action (including a broken one).
    StoreAndSend(caster, DesiredHighlight(caster, nowMs));
}

void UpdateFfxivComboHighlight(Player& player)
{
    if (GetFfxivComboHighlightSpell(player.GetGUID()) == 0)
        return; // nothing on screen: nothing to expire

    StoreAndSend(player, DesiredHighlight(player, GameTime::GetGameTimeMS()));
}

void SendFfxivComboHighlight(Player& player)
{
    if (!HasFfxivActionComboRows())
        return; // no chain data: no traffic, the client keeps its empty bar

    // Force a send: the client that just entered the world has no value, and one whose UI was
    // reloaded lost it, so "same as last sent" is not a reason to stay silent here.
    g_highlight.erase(player.GetGUID());
    StoreAndSend(player, DesiredHighlight(player, GameTime::GetGameTimeMS()));
}

void ClearFfxivComboHighlightState()
{
    g_highlight.clear();
}

uint32 GetFfxivComboHighlightSpell(ObjectGuid player)
{
    auto const it = g_highlight.find(player);
    return it == g_highlight.end() ? 0 : it->second;
}
} // namespace FFXIV::Combat
