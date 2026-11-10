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

#ifndef FFXIV_VOICE_STORE_H
#define FFXIV_VOICE_STORE_H

#include "FFXIVVoice.h"
#include "Player.h"
#include "TSPlayer.h"

/**
* @file FFXIVVoiceStore.h
* @brief Per-character FFXIV emote voice storage.
*
* Uses the existing per-character DB JSON (Player::m_db_json, a TSDBJson), which is
* loaded in Player::LoadFromDB and saved on logout, so no characters-table migration is
* required. Key: FFXIV::VoiceWire::DbKey ("ffxiv_voice"), value 1..12, default 1.
*
* The stored voice only participates for the ported FFXIV races (ChrRaces 12..20); the
* client derives the body index from the same race/sex pair it sees on the unit.
*/
namespace FFXIV
{
namespace VoiceStore
{
    inline bool IsFFXIVRace(Player* player)
    {
        if (!player)
            return false;

        std::uint32_t race = player->GetRace();
        return race >= 12 && race <= 20;
    }

    // Returns 0 when nothing valid is stored (no marker is sent in that case).
    inline std::uint32_t GetStoredVoice(Player* player)
    {
        if (!player)
            return 0;

        std::uint32_t voice = static_cast<std::uint32_t>(TSPlayer(player).GetDBUInt32(VoiceWire::DbKey, 0));
        return VoiceWire::IsValidVoice(voice) ? voice : 0;
    }

    inline void SetStoredVoice(Player* player, std::uint32_t voice)
    {
        if (!player || !VoiceWire::IsValidVoice(voice))
            return;

        TSPlayer tsPlayer(player);
        tsPlayer.SetDBUInt32(VoiceWire::DbKey, voice);
        tsPlayer.SaveDBJson();
    }

    // Login hook: materialise the documented default so the marker is sent from the
    // first emote on. Only FFXIV races get a row, stock races keep their DB JSON clean.
    inline void EnsureStoredVoice(Player* player)
    {
        if (!IsFFXIVRace(player))
            return;

        if (GetStoredVoice(player) == 0)
            SetStoredVoice(player, VoiceWire::FirstVoice);
    }

    // The emote_num to rebroadcast: 0 keeps the client's value (stock behaviour).
    inline std::uint32_t EmoteNumFor(Player* player)
    {
        if (!IsFFXIVRace(player))
            return 0;

        std::uint32_t voice = GetStoredVoice(player);
        return voice ? VoiceWire::Encode(voice) : 0;
    }
}
}

#endif // FFXIV_VOICE_STORE_H
