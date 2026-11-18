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

#ifndef FFXIV_CREATION_H
#define FFXIV_CREATION_H

#include "CFXIVCreationLogic.h"
#include "DatabaseEnvFwd.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class Player;
class WorldSession;

// Server glue for character creation: login snapshot/request, UPDATE persistence and
// the delete cleanup. The rules themselves live in CFXIVCreationLogic.h.
namespace FFXIV::Creation
{
    // Sends the stored tuple, or asks the client for one when none is stored or it is invalid.
    void OnPlayerLogin(Player* player);

    // Handles one UPDATE payload (protocol version, echoed GUID, tuple). Ignores bad input.
    void HandleUpdate(Player* player, uint8 const* data, std::size_t size);

    // Sends `subject`'s stored tuple to `observer` (UNIT_TUPLE_OPCODE: protocol version, the subject's raw GUID,
    // tuple) so the observer's client draws the subject in its own look. Called when the subject becomes visible.
    void SendUnitTupleTo(Player const* observer, Player const* subject);

    // CREATE_OPCODE, received before the player is in the world (Create, Customize): remembers the tuple the
    // client chose for `name` on this account until the character is saved. False for malformed or invalid input.
    bool HandleCreate(uint32 accountId, std::vector<uint8> const& payload);

    // Registers CREATE_OPCODE with the not-in-world custom packet path (RegisterPacketForNotInWorld +
    // events.CustomPacket.OnReceiveNotInWorld). Safe to call repeatedly; the character list calls it before any client
    // can create a character.
    void RegisterPackets();

    // Adds the tuple remembered for this account to the transaction that saves a new or customized character, so the
    // row exists the moment the character does. Used only when the name, race and sex match the remembered one.
    bool AppendCreate(CharacterDatabaseTransaction const& trans, uint32 accountId, ObjectGuid::LowType guid,
                      std::string const& name, uint8 race, uint8 sex);

    struct ListedCharacter
    {
        ObjectGuid::LowType guid;
        uint8 race;
        uint8 sex;
    };

    // Sends the stored tuple of every listed FFXIV character with the character list (CHAR_LIST_OPCODE), so the
    // select screen draws what the server holds instead of a local copy.
    void SendCharacterList(WorldSession* session, std::vector<ListedCharacter> const& characters);

    // Appends the appearance-row delete to a character delete transaction.
    void AppendDelete(CharacterDatabaseTransaction const& trans, ObjectGuid::LowType guid);
}

#endif // FFXIV_CREATION_H
