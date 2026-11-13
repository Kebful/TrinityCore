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

#include "CFXIVCreation.h"
#include "TSPlayer.h"
#include "CustomPacketDefines.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "CustomPacketWrite.h"
#include "WorldPacket.h"

#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace FFXIV::Creation
{
namespace
{
    // Column order matches the tuple's wire order (see CFXIVCreationLogic.cpp).
    char const* const kColumns[TUPLE_BYTES] = {
        "version", "race", "clan", "sex", "face", "hairstyle", "hairColor", "highlightColor",
        "skin", "eyeLeft", "eyeRight", "lips", "tattooColor", "facialFeatures", "tattoos",
        "facePaint", "facePaintColor", "muscle", "bust", "height", "voice", "eyebrows", "nose",
        "jaw", "eyeShape", "irisSize", "raceFeature", "flags", "namedayMoon", "namedayDay", "guardian",
    };

    // Splits a custom packet into its world packets and hands each to `send`.
    template <class Send>
    void ForEachCustomPacket(uint16 opcode, std::vector<uint8> const& payload, Send send)
    {
        if (payload.size() > std::numeric_limits<totalSize_t>::max())
            return;

        CustomPacketWrite writer(opcode, MAX_FRAGMENT_SIZE, 0);
        if (!payload.empty())
            writer.WriteBytes(static_cast<totalSize_t>(payload.size()), reinterpret_cast<char const*>(payload.data()));

        for (CustomPacketChunk& chunk : writer.buildMessages())
        {
            WorldPacket packet(SERVER_TO_CLIENT_OPCODE, chunk.FullSize());
            packet.append(reinterpret_cast<uint8 const*>(chunk.Data()), chunk.FullSize());
            send(packet);
        }
        writer.Destroy();
    }

    void SendCustomPacket(Player const* player, uint16 opcode, std::vector<uint8> const& payload)
    {
        if (!player)
            return;
        ForEachCustomPacket(opcode, payload, [player](WorldPacket const& packet) { player->SendDirectMessage(&packet); });
    }

    // UNIT_TUPLE_OPCODE payload for `subject`: protocol version, the subject's raw GUID (the client looks the unit up
    // by it), the tuple.
    std::vector<uint8> UnitTuplePayload(Player const* subject, CustomizeTuple const& tuple)
    {
        std::vector<uint8> payload;
        payload.push_back(PROTOCOL_VERSION);
        uint64 const guid = subject->GetGUID().GetRawValue();
        for (int shift = 0; shift < 64; shift += 8)
            payload.push_back(static_cast<uint8>(guid >> shift));
        WriteTuple(tuple, payload);
        return payload;
    }

    // Stored tuples of FFXIV characters, so a visibility event never waits on the database. std::nullopt records a
    // character with no valid row. Filled at login and on update, loaded on first use otherwise.
    std::mutex s_tupleMutex;
    std::unordered_map<ObjectGuid::LowType, std::optional<CustomizeTuple>> s_tuples;

    void RememberTuple(ObjectGuid::LowType guid, std::optional<CustomizeTuple> const& tuple)
    {
        std::lock_guard<std::mutex> lock(s_tupleMutex);
        s_tuples[guid] = tuple;
    }

    // Protocol version and the echoed GUID, the prefix of every server-to-client creation packet.
    std::vector<uint8> Prefix(Player* player)
    {
        std::vector<uint8> payload;
        payload.push_back(PROTOCOL_VERSION);
        ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
        for (int shift = 0; shift < 64; shift += 8)
            payload.push_back(static_cast<uint8>(guid >> shift));
        return payload;
    }

    void SendSnapshot(Player* player, CustomizeTuple const& tuple)
    {
        std::vector<uint8> payload = Prefix(player);
        WriteTuple(tuple, payload);
        SendCustomPacket(player, SNAPSHOT_OPCODE, payload);
    }

    void SendRequest(Player* player)
    {
        SendCustomPacket(player, REQUEST_OPCODE, Prefix(player));
    }

    // False when no row exists for the character.
    bool LoadStored(ObjectGuid::LowType guid, CustomizeTuple& tuple)
    {
        std::string sql = "SELECT ";
        for (std::size_t i = 0; i < TUPLE_BYTES; ++i)
        {
            if (i)
                sql += ", ";
            sql += kColumns[i];
        }
        sql += " FROM `ffxivcharactercustomize` WHERE `guid` = " + std::to_string(guid);

        QueryResult result = CharacterDatabase.Query(sql.c_str());
        if (!result)
            return false;

        std::uint8_t bytes[TUPLE_BYTES];
        Field* fields = result->Fetch();
        for (std::size_t i = 0; i < TUPLE_BYTES; ++i)
            bytes[i] = fields[i].GetUInt8();
        return ReadTuple(bytes, TUPLE_BYTES, tuple);
    }

    // True when the tuple is valid and belongs to the subject's current race and sex (a race change at login
    // leaves the old row behind, so a cached tuple must be checked against the live character too).
    bool Matches(CustomizeTuple const& tuple, Player const* subject)
    {
        return tuple.race == subject->GetRace() && tuple.sex == static_cast<uint8>(subject->GetGender()) &&
            *ValidateTuple(tuple) == '\0';
    }

    // The subject's stored tuple when it is valid for the subject's race and sex.
    std::optional<CustomizeTuple> StoredTupleFor(Player const* subject)
    {
        ObjectGuid::LowType const guid = subject->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> lock(s_tupleMutex);
            auto found = s_tuples.find(guid);
            if (found != s_tuples.end() && (!found->second || Matches(*found->second, subject)))
                return found->second;
        }
        CustomizeTuple tuple;
        std::optional<CustomizeTuple> result;
        if (LoadStored(guid, tuple) && Matches(tuple, subject))
            result = tuple;
        RememberTuple(guid, result);
        return result;
    }

    void SaveStored(ObjectGuid::LowType guid, CustomizeTuple const& tuple)
    {
        std::vector<uint8> bytes;
        WriteTuple(tuple, bytes);

        std::string sql = "REPLACE INTO `ffxivcharactercustomize` (`guid`";
        for (std::size_t i = 0; i < TUPLE_BYTES; ++i)
        {
            sql += ", `";
            sql += kColumns[i];
            sql += "`";
        }
        sql += ") VALUES (" + std::to_string(guid);
        for (std::uint8_t value : bytes)
            sql += ", " + std::to_string(value);
        sql += ")";

        CharacterDatabase.Execute(sql.c_str());
    }
}

void OnPlayerLogin(Player* player)
{
    if (!player)
        return;

    if (player->GetRace() < FIRST_PLAYABLE_RACE || player->GetRace() > LAST_PLAYABLE_RACE)
        return;

    CustomizeTuple tuple;
    ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
    // Drop whatever an earlier login cached: a race or sex change leaves the old tuple behind.
    RememberTuple(guid, std::nullopt);
    if (!LoadStored(guid, tuple))
    {
        SendRequest(player);
        return;
    }
    if (tuple.race != player->GetRace() || tuple.sex != static_cast<uint8>(player->GetGender()))
    {
        TC_LOG_INFO("entities.player.creation", "Stored tuple race/sex mismatch for {}, asking client", player->GetName());
        SendRequest(player);
        return;
    }
    if (*ValidateTuple(tuple) != '\0')
    {
        TC_LOG_INFO("entities.player.creation", "Invalid stored tuple for {}, asking client", player->GetName());
        SendRequest(player);
        return;
    }
    RememberTuple(guid, tuple);
    SendSnapshot(player, tuple);
}

void SendUnitTupleTo(Player const* observer, Player const* subject)
{
    if (!observer || !subject || observer == subject)
        return;
    if (subject->GetRace() < FIRST_PLAYABLE_RACE || subject->GetRace() > LAST_PLAYABLE_RACE)
        return;
    std::optional<CustomizeTuple> const tuple = StoredTupleFor(subject);
    if (!tuple)
        return;
    SendCustomPacket(observer, UNIT_TUPLE_OPCODE, UnitTuplePayload(subject, *tuple));
}

void HandleUpdate(Player* player, uint8 const* data, std::size_t size)
{
    if (!player)
        return;
    if (size < UPDATE_PAYLOAD_BYTES)
    {
        TC_LOG_DEBUG("entities.player.creation", "Short creation update from {} ignored", player->GetName());
        return;
    }
    if (data[0] != PROTOCOL_VERSION)
    {
        TC_LOG_DEBUG("entities.player.creation", "Creation protocol {} from {} ignored", data[0], player->GetName());
        return;
    }
    // data[1..8] is the client's echoed GUID; the player object is authoritative.

    CustomizeTuple tuple;
    if (!ReadTuple(data + 9, size - 9, tuple))
        return;
    if (tuple.race != player->GetRace() || tuple.sex != static_cast<uint8>(player->GetGender()))
    {
        TC_LOG_DEBUG("entities.player.creation", "Rejected update for race {}/{} sex {}/{}",
            tuple.race, player->GetRace(), tuple.sex, static_cast<uint8>(player->GetGender()));
        // The client's copy is for another race or sex: forget it and ask again, so it re-derives from the live character.
        RememberTuple(player->GetGUID().GetCounter(), std::nullopt);
        SendRequest(player);
        return;
    }
    if (char const* error = ValidateTuple(tuple); *error != '\0')
    {
        TC_LOG_DEBUG("entities.player.creation", "Rejected update ({})", error);
        return;
    }

    SaveStored(player->GetGUID().GetCounter(), tuple);
    RememberTuple(player->GetGUID().GetCounter(), tuple);
    // Echo the stored row so the client can mark its cached copy confirmed.
    SendSnapshot(player, tuple);
    // Everyone who can see the player gets the new look without relogging.
    ForEachCustomPacket(UNIT_TUPLE_OPCODE, UnitTuplePayload(player, tuple),
        [player](WorldPacket const& packet) { player->SendMessageToSet(&packet, false); });
    TC_LOG_DEBUG("entities.player.creation", "Saved race {} clan {} sex {} face {} for {}",
        tuple.race, tuple.clan, tuple.sex, tuple.face, player->GetName());
}

void AppendDelete(CharacterDatabaseTransaction const& trans, ObjectGuid::LowType guid)
{
    {
        std::lock_guard<std::mutex> lock(s_tupleMutex);
        s_tuples.erase(guid);
    }
    trans->PAppend("DELETE FROM `ffxivcharactercustomize` WHERE `guid` = {}", guid);
}

}  // namespace FFXIV::Creation
