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
 * FFXIV derived-stat snapshot packet (phase 1c). Read-only presentation data:
 * no Unit field, no database, no gameplay value is written or read back here.
 * See FFXIVStatsSnapshot.h and work/ffxiv-stats-sheet-20261123/DESIGN.md.
 */

#include "FFXIVStatsSnapshot.h"
#include "CustomPacketDefines.h"
#include "CustomPacketWrite.h"
#include "Log.h"
#include "Player.h"
#include "WorldPacket.h"

#include <limits>

namespace FFXIV::Stats
{
namespace
{
    // Wire-width guard, not a game rule. Shipped slice-1 values are far below these
    // bounds (the largest is a level-80 main stat); a value outside the window is a data
    // bug, and saturating here keeps the byte image well-formed for the client.
    uint16 ToWireU16(int32 value)
    {
        if (value <= 0)
            return 0;
        if (value > std::numeric_limits<uint16>::max())
            return std::numeric_limits<uint16>::max();
        return uint16(value);
    }

    void AppendU16(std::vector<uint8>& out, uint16 value)
    {
        out.push_back(uint8(value & 0xFF));
        out.push_back(uint8((value >> 8) & 0xFF));
    }

    void AppendU32(std::vector<uint8>& out, uint32 value)
    {
        out.push_back(uint8(value & 0xFF));
        out.push_back(uint8((value >> 8) & 0xFF));
        out.push_back(uint8((value >> 16) & 0xFF));
        out.push_back(uint8((value >> 24) & 0xFF));
    }
} // namespace

std::vector<uint8> BuildFfxivStatsSnapshotPayload(FfxivDerivedStats const& stats)
{
    std::vector<uint8> payload;
    payload.reserve(FFXIV_STATS_SNAPSHOT_BYTES);

    payload.push_back(stats.jobId);                                 // 0  job
    payload.push_back(stats.level);                                 // 1  level

    for (int i = 0; i < 6; ++i)                                     // 2  base[6]
        AppendU16(payload, ToWireU16(stats.base[i]));

    for (int i = 0; i < 6; ++i)                                     // 14 secondaries[6]
        AppendU16(payload, ToWireU16(stats.param[FFXIV_STATS_SNAPSHOT_SECONDARIES[i]]));

    AppendU32(payload, stats.hpMax);                                // 26 hp
    AppendU32(payload, stats.mpMax);                                // 30 mp
    AppendU32(payload, stats.gcdMs);                                // 34 gcd (ms)
    AppendU32(payload, stats.provenance);                           // 38 provenance
    payload.push_back(FFXIV_STATS_SNAPSHOT_SCHEMA);                 // 42 schema

    return payload;
}

void SendFfxivStatsSnapshot(Player& player)
{
    if constexpr (kFfxivStatsSnapshotSendEnabled)
    {
        // GetFFXIVStats() rebuilds the cached snapshot if it is dirty and otherwise
        // returns it unchanged; it is the same value the rebuild hook just logged.
        FfxivDerivedStats const& stats = player.GetFFXIVStats();
        std::vector<uint8> const payload = BuildFfxivStatsSnapshotPayload(stats);
        if (payload.size() != FFXIV_STATS_SNAPSHOT_BYTES || payload.size() > std::numeric_limits<totalSize_t>::max())
            return;

        CustomPacketWrite writer(FFXIV_STATS_SNAPSHOT_OPCODE, MAX_FRAGMENT_SIZE, 0);
        writer.WriteBytes(static_cast<totalSize_t>(payload.size()),
                          reinterpret_cast<char const*>(payload.data()));

        for (CustomPacketChunk& chunk : writer.buildMessages())
        {
            WorldPacket packet(SERVER_TO_CLIENT_OPCODE, chunk.FullSize());
            packet.append(reinterpret_cast<uint8 const*>(chunk.Data()), chunk.FullSize());
            player.SendDirectMessage(&packet);
        }
        writer.Destroy();

        TC_LOG_DEBUG("ffxiv.stats",
                     "SendFfxivStatsSnapshot {} ({}) opcode 0x{:04X} schema {} job {} level {} "
                     "hp {} mp {} gcd {} ms provenance 0x{:X} bytes {}",
                     player.GetName(), player.GetGUID().ToString(), uint32(FFXIV_STATS_SNAPSHOT_OPCODE),
                     uint32(FFXIV_STATS_SNAPSHOT_SCHEMA), uint32(stats.jobId), uint32(stats.level),
                     stats.hpMax, stats.mpMax, stats.gcdMs, stats.provenance, payload.size());
    }
    else
    {
        (void)player;
    }
}
} // namespace FFXIV::Stats
