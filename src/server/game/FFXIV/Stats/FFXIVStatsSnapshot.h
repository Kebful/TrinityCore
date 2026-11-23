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

#ifndef TRINITYCORE_FFXIV_STATS_SNAPSHOT_H
#define TRINITYCORE_FFXIV_STATS_SNAPSHOT_H

#include "FFXIVStatTypes.h"
#include <cstddef>
#include <cstdint>
#include <vector>

class Player;

/*
 * FFXIV derived-stat snapshot to the client (phase 1c, design:
 * work/ffxiv-stats-sheet-20261123/DESIGN.md).
 *
 * The client has no WoW field for Piety, Determination, Direct Hit, Tenacity, either
 * speed or the GCD, so the panel can only show them if the server sends its snapshot.
 * This is a dedicated packet -- not update fields, not client-side recomputation -- so
 * the server snapshot stays the one source of truth.
 *
 * Transport
 * ---------
 * Custom-packet sub-opcode 0x7A60 inside the fork's existing custom channel:
 * the outer WorldPacket is SMSG 0x102 (SERVER_TO_CLIENT_OPCODE), the sub-opcode lives in
 * the CustomPacketHeader. A raw SMSG cannot be used: both opcode tables are dense up to
 * SMSG_MULTIPLE_MOVES = 0x51E, 0x51F is CMSG_CUSTOM, and the client only dispatches
 * msgId < NUM_MSG_TYPES (0x51F). See DESIGN.md §1.
 *
 * No-write proof
 * --------------
 * The payload is built from FfxivDerivedStats alone and sent. Nothing in this path
 * touches a Unit field, the database, or any gameplay value; the projection gate
 * (FFXIV/Stats/FFXIVStatProjection.h) is not consulted and every Unit-field write in
 * PlayerFFXIVStats.cpp stays inside `if constexpr (kFfxivStatProjectionEnabled)`.
 *
 * Client counterpart: repos/clientdecomp/WoW/Source/Duskhaven/FFXIV/Stats/FFXIVStatsSnapshot.h
 */

namespace FFXIV::Stats
{
// Custom-packet sub-opcode. Both ends carry this constant; see DESIGN.md §1.3.
constexpr uint16 FFXIV_STATS_SNAPSHOT_OPCODE = 0x7A60;

// Payload schema. A change in field set or width is a new number (the client rejects
// every other size/schema pair as a whole rather than parsing it partially).
constexpr uint8 FFXIV_STATS_SNAPSHOT_SCHEMA = 1;

// job u8, level u8, base[6] u16, secondaries[6] u16, hp u32, mp u32, gcd u32,
// provenance u32, schema u8. Packed, little-endian, no padding.
constexpr std::size_t FFXIV_STATS_SNAPSHOT_BYTES = 43;

// Traffic switch. ON: the packet is presentation data only -- it changes no gameplay
// value, so sending it is unconditional under this flag. Turning it off leaves the
// client with an empty snapshot and the panel blank (DESIGN.md §4), which is the
// intended failure mode, not a fallback.
constexpr bool kFfxivStatsSnapshotSendEnabled = true;

// Order of the six secondary values on the wire: CRIT, DIRECT HIT, DETERMINATION,
// TENACITY, SKILL SPEED, SPELL SPEED. BaseParam row ids from the shipped sheet.
constexpr FfxivParam FFXIV_STATS_SNAPSHOT_SECONDARIES[6] =
{
    FFXIV_PARAM_CRITICAL_HIT,
    FFXIV_PARAM_DIRECT_HIT_RATE,
    FFXIV_PARAM_DETERMINATION,
    FFXIV_PARAM_TENACITY,
    FFXIV_PARAM_SKILL_SPEED,
    FFXIV_PARAM_SPELL_SPEED,
};

// Trace field offsets, so the client reader and the tests can cite them by name.
enum FfxivStatsSnapshotOffset : std::size_t
{
    FFXIV_SNAPSHOT_OFF_JOB        = 0,
    FFXIV_SNAPSHOT_OFF_LEVEL      = 1,
    FFXIV_SNAPSHOT_OFF_BASE       = 2,   // 6 x u16
    FFXIV_SNAPSHOT_OFF_SECONDARY  = 14,  // 6 x u16
    FFXIV_SNAPSHOT_OFF_HP         = 26,  // u32
    FFXIV_SNAPSHOT_OFF_MP         = 30,  // u32
    FFXIV_SNAPSHOT_OFF_GCD        = 34,  // u32, milliseconds
    FFXIV_SNAPSHOT_OFF_PROVENANCE = 38,  // u32, FfxivProvenance bits
    FFXIV_SNAPSHOT_OFF_SCHEMA     = 42,  // u8
};

// Pure function: snapshot -> payload bytes. No Player, no DB, no logging, no I/O, so the
// unit test can pin the exact wire image. Little-endian by explicit byte writes rather
// than a struct overlay (the layout has no padding and does not depend on host
// endianness or packing rules).
std::vector<uint8> BuildFfxivStatsSnapshotPayload(FfxivDerivedStats const& stats);

// Fills the payload from Player::GetFFXIVStats() (the cached snapshot; it rebuilds once
// if dirty) and sends it over the custom channel. Discarded entirely when
// kFfxivStatsSnapshotSendEnabled is false.
void SendFfxivStatsSnapshot(Player& player);
} // namespace FFXIV::Stats

#endif // TRINITYCORE_FFXIV_STATS_SNAPSHOT_H
