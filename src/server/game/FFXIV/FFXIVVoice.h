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

#ifndef FFXIV_VOICE_H
#define FFXIV_VOICE_H

#include <cstdint>

/**
* @file FFXIVVoice.h
* @brief Server-authoritative FFXIV emote voice wire format.
*
* The world server replaces SMSG_TEXT_EMOTE's emote_num with
* FFXIV::VoiceWire::Marker | voice when the sender has a voice selected. Nearby
* clients decode it in CGUnit_C::PlayTextEmoteSound
* (repos/clientdecomp/WoW/Source/Object/ObjectClient/UnitSound_C.cpp) through the
* matching constants in
* repos/clientdecomp/WoW/Source/Duskhaven/FFXIV/CFXIVVoiceSlots.h.
*
* This header is intentionally free of core, TSWoW and Windows dependencies so the
* encode/decode contract can be unit tested on any host compiler.
*/
namespace FFXIV
{
namespace VoiceWire
{
    // Marker placed in SMSG_TEXT_EMOTE emote_num. The stock value is a sound-file
    // index (or 0xFFFFFFFF for "random"), so a set high bit is unambiguous.
    inline constexpr std::uint32_t Marker = 0x80000000u;

    inline constexpr std::uint32_t FirstVoice = 1;
    inline constexpr std::uint32_t LastVoice = 12;

    // Per-character DB JSON key (Player::m_db_json / TSDBJsonProvider).
    inline constexpr char const* DbKey = "ffxiv_voice";

    inline constexpr bool IsValidVoice(std::uint32_t voice)
    {
        return voice >= FirstVoice && voice <= LastVoice;
    }

    inline constexpr std::uint32_t Encode(std::uint32_t voice)
    {
        return Marker | (voice & 0xFFu);
    }

    // Returns false when the value carries no usable marker, leaving the caller on the
    // stock (client-supplied) path.
    inline constexpr bool Decode(std::uint32_t emoteNum, std::uint32_t& voice)
    {
        if ((emoteNum & Marker) == 0)
            return false;

        std::uint32_t decoded = emoteNum & 0xFFu;

        if (!IsValidVoice(decoded))
            return false;

        voice = decoded;
        return true;
    }
}
}

#endif // FFXIV_VOICE_H
