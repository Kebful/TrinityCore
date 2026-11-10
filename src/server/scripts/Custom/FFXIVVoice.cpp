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

/**
* @file FFXIVVoice.cpp
* @brief .ffxivvoice player command and the matching login default.
*
*   .ffxivvoice         show the current voice
*   .ffxivvoice 1..12   select a voice
*
* Replaces the retired livescript command of the same name
* (release/modules/ffxivhaven/livescripts/race-sounds/RaceSounds.ts, no longer
* registered). The selection is stored per character and rebroadcast by
* WorldSession::HandleTextEmoteOpcode; see FFXIVVoice.h / FFXIVVoiceStore.h.
*/

#include "ScriptMgr.h"
#include "Chat.h"
#include "TSPlayer.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldSession.h"
#include "FFXIVVoice.h"
#include "FFXIVVoiceStore.h"

using namespace Trinity::ChatCommands;

class ffxiv_voice_commandscript : public CommandScript
{
public:
    ffxiv_voice_commandscript() : CommandScript("ffxiv_voice_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        // Every account inherits RBAC_PERM_ROLE_PLAYER (199) through
        // rbac_default_permissions (sec 0 -> 195) and rbac_linked_permissions
        // (195 -> 199), so this exposes the command to all players without a new
        // RBAC permission row. See the report for the exact SQL lines.
        static ChatCommandTable ffxivVoiceCommandTable =
        {
            { "ffxivvoice", HandleFFXIVVoiceCommand, rbac::RBAC_ROLE_PLAYER, Console::No },
        };

        return ffxivVoiceCommandTable;
    }

    static bool HandleFFXIVVoiceCommand(ChatHandler* handler, Optional<uint32> voiceArg)
    {
        Player* player = handler->GetSession()->GetPlayer();

        if (!FFXIV::VoiceStore::IsFFXIVRace(player))
        {
            handler->GetSession()->SendNotification("FFXIV voice: only available for FFXIV races 12-20.");
            return true;
        }

        std::uint32_t current = FFXIV::VoiceStore::GetStoredVoice(player);
        if (current == 0)
            current = FFXIV::VoiceWire::FirstVoice;

        if (!voiceArg)
        {
            handler->GetSession()->SendNotification("FFXIV voice: %u/%u (use .ffxivvoice 1-%u).",
                current, FFXIV::VoiceWire::LastVoice, FFXIV::VoiceWire::LastVoice);
            return true;
        }

        std::uint32_t wanted = *voiceArg;
        if (!FFXIV::VoiceWire::IsValidVoice(wanted))
        {
            handler->GetSession()->SendNotification("FFXIV voice: pick a number from %u to %u.",
                FFXIV::VoiceWire::FirstVoice, FFXIV::VoiceWire::LastVoice);
            return true;
        }

        FFXIV::VoiceStore::SetStoredVoice(player, wanted);
        handler->GetSession()->SendNotification("FFXIV voice set to %u/%u.", wanted, FFXIV::VoiceWire::LastVoice);
        return true;
    }
};

class ffxiv_voice_playerscript : public PlayerScript
{
public:
    ffxiv_voice_playerscript() : PlayerScript("ffxiv_voice_playerscript") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        FFXIV::VoiceStore::EnsureStoredVoice(player);
    }
};

void AddSC_ffxiv_voice()
{
    new ffxiv_voice_commandscript();
    new ffxiv_voice_playerscript();
}
