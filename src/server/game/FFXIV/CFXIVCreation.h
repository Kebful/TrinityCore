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

class Player;

// Server glue for character creation: login snapshot/request, UPDATE persistence and
// the delete cleanup. The rules themselves live in CFXIVCreationLogic.h.
namespace FFXIV::Creation
{
    // Sends the stored tuple, or asks the client for one when none is stored or it is invalid.
    void OnPlayerLogin(Player* player);

    // Handles one UPDATE payload (protocol version, echoed GUID, tuple). Ignores bad input.
    void HandleUpdate(Player* player, uint8 const* data, std::size_t size);

    // Appends the appearance-row delete to a character delete transaction.
    void AppendDelete(CharacterDatabaseTransaction const& trans, ObjectGuid::LowType guid);
}

#endif // FFXIV_CREATION_H
