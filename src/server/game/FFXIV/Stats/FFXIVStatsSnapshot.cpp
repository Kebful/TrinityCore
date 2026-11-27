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
 * FFXIV derived-stat snapshot packet (phase 1c) and the tooltip action potency list (GATE 7).
 * Read-only presentation data: no Unit field, no database, no gameplay value is written or read
 * back here -- the multiplier is computed from the same snapshot the packet already carries.
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

    // One custom-packet message: fragmented by CustomPacketWrite and sent one WorldPacket per
    // chunk. `payload` must already be the final byte image.
    void SendCustomPayload(Player& player, uint16 opcode, std::vector<uint8> const& payload)
    {
        if (payload.empty() || payload.size() > std::numeric_limits<totalSize_t>::max())
            return;

        CustomPacketWrite writer(opcode, MAX_FRAGMENT_SIZE, 0);
        writer.WriteBytes(static_cast<totalSize_t>(payload.size()),
                          reinterpret_cast<char const*>(payload.data()));

        for (CustomPacketChunk& chunk : writer.buildMessages())
        {
            WorldPacket packet(SERVER_TO_CLIENT_OPCODE, chunk.FullSize());
            packet.append(reinterpret_cast<uint8 const*>(chunk.Data()), chunk.FullSize());
            player.SendDirectMessage(&packet);
        }
        writer.Destroy();
    }

    // The tooltip scalar for this player, from the snapshot the packet above carries and the
    // exact inputs the cast hook uses (ComputeFfxivActionWeaponDamagePercent, FfxivPartyFromGroup).
    // The tooltip has no target, so the level correction is the player's own level: 1000.
    uint32 ComputeTooltipMultiplierBasisPoints(Player& player, FfxivDerivedStats const& stats)
    {
        int32 const basisPoints = Combat::ComputeFfxivTooltipMultiplierBasisPoints(
            stats,
            Combat::ComputeFfxivActionWeaponDamagePercent(stats),
            stats.level,
            Combat::FfxivPartyFromGroup(player),
            Combat::FfxivTraitState{});

        return basisPoints > 0 ? uint32(basisPoints) : 0u;
    }

    // The compact list, skipping (not wrapping) an id or potency the u16 wire cannot carry.
    void SendActionPotencyList(Player& player, FfxivDerivedStats const& stats)
    {
        std::vector<Combat::FfxivActionPlainPotency> const entries =
            Combat::CollectFfxivActionPlainPotencies(stats.jobId, stats.level);

        std::vector<uint8> const payload = BuildFfxivActionPotencyPayload(entries);
        if (payload.size() != FfxivActionPotencyPayloadBytes(entries.size()))
        {
            // Skipped entries: the log line below reports the shipped/actual counts so a
            // truncated table is visible rather than silent.
            TC_LOG_DEBUG("ffxiv.stats",
                         "SendFfxivActionPotencyList {} job {} level {} skipped {} entries above the u16 wire",
                         player.GetName(), uint32(stats.jobId), uint32(stats.level),
                         uint32(entries.size()));
        }

        if (entries.empty())
        {
            TC_LOG_DEBUG("ffxiv.stats",
                         "SendFfxivActionPotencyList {} job {} level {}: no potency rows loaded, nothing sent",
                         player.GetName(), uint32(stats.jobId), uint32(stats.level));
            return;
        }

        SendCustomPayload(player, FFXIV_ACTION_POTENCY_OPCODE, payload);

        TC_LOG_DEBUG("ffxiv.stats",
                     "SendFfxivActionPotencyList {} ({}) opcode 0x{:04X} schema {} job {} level {} "
                     "entries {} bytes {}",
                     player.GetName(), player.GetGUID().ToString(), uint32(FFXIV_ACTION_POTENCY_OPCODE),
                     uint32(FFXIV_ACTION_POTENCY_SCHEMA), uint32(stats.jobId), uint32(stats.level),
                     uint32(entries.size()), uint32(payload.size()));
    }
} // namespace

std::vector<uint8> BuildFfxivStatsSnapshotPayload(FfxivDerivedStats const& stats,
                                                   uint32 damageMultiplierBp)
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
    AppendU32(payload, damageMultiplierBp);                         // 42 multiplier (bp), schema 2
    payload.push_back(FFXIV_STATS_SNAPSHOT_SCHEMA);                 // 46 schema

    return payload;
}

std::vector<uint8> BuildFfxivActionPotencyPayload(
    std::vector<Combat::FfxivActionPlainPotency> const& entries)
{
    std::vector<uint8> payload;
    payload.reserve(FfxivActionPotencyPayloadBytes(entries.size()));

    // Count first, then the entries, then the schema byte last: the reader checks the width
    // (3 + 8*count) and the trailing schema before it looks at a single entry.
    std::size_t written = 0;
    for (Combat::FfxivActionPlainPotency const& entry : entries)
        if (entry.actionId <= std::numeric_limits<uint16>::max()
            && (entry.damage > 0 || entry.cure > 0 || entry.dot > 0))
            ++written;

    AppendU16(payload, uint16(written));
    // The reader treats the two-byte count as the whole header: entries start at offset 2 and the
    // trailing schema byte is last. Writing a reserved byte here made the body one byte too long.
    for (Combat::FfxivActionPlainPotency const& entry : entries)
    {
        if (entry.actionId > std::numeric_limits<uint16>::max()
            || (entry.damage == 0 && entry.cure == 0 && entry.dot == 0))
            continue;
        AppendU16(payload, uint16(entry.actionId));
        AppendU16(payload, entry.damage);
        AppendU16(payload, entry.cure);
        AppendU16(payload, entry.dot);
    }
    payload.push_back(FFXIV_ACTION_POTENCY_SCHEMA);

    return payload;
}

void SendFfxivStatsSnapshot(Player& player)
{
    if constexpr (kFfxivStatsSnapshotSendEnabled)
    {
        // GetFFXIVStats() rebuilds the cached snapshot if it is dirty and otherwise
        // returns it unchanged; it is the same value the rebuild hook just logged.
        FfxivDerivedStats const& stats = player.GetFFXIVStats();
        uint32 const multiplierBp = ComputeTooltipMultiplierBasisPoints(player, stats);

        std::vector<uint8> const payload = BuildFfxivStatsSnapshotPayload(stats, multiplierBp);
        if (payload.size() == FFXIV_STATS_SNAPSHOT_BYTES
            && payload.size() <= std::numeric_limits<totalSize_t>::max())
        {
            SendCustomPayload(player, FFXIV_STATS_SNAPSHOT_OPCODE, payload);

            TC_LOG_DEBUG("ffxiv.stats",
                         "SendFfxivStatsSnapshot {} ({}) opcode 0x{:04X} schema {} job {} level {} "
                         "hp {} mp {} gcd {} ms provenance 0x{:X} multiplier {} bp bytes {}",
                         player.GetName(), player.GetGUID().ToString(), uint32(FFXIV_STATS_SNAPSHOT_OPCODE),
                         uint32(FFXIV_STATS_SNAPSHOT_SCHEMA), uint32(stats.jobId), uint32(stats.level),
                         stats.hpMax, stats.mpMax, stats.gcdMs, stats.provenance, multiplierBp,
                         payload.size());
        }

        // The potency list is keyed by (job, level), so it rides the same send points as the
        // snapshot: the enter-world send and the rebuild hook. It is a second sub-opcode rather
        // than a field of the snapshot, so a snapshot rebuild is not charged for the whole table.
        SendActionPotencyList(player, stats);
    }
    else
    {
        (void)player;
    }
}
} // namespace FFXIV::Stats
