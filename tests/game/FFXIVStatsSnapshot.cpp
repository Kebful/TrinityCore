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
 * Phase 1c: the FFXIV stats snapshot packet, proven at the builder.
 *
 * The brief forbids running a server, so "the server sends the snapshot at login" is
 * reduced here to what can be proven offline: BuildFfxivStatsSnapshotPayload() is a pure
 * function of FfxivDerivedStats, and these cases pin the exact byte image it returns
 * (size, field order, little-endian encoding) plus the client-side contract
 * (schema byte, no padding, secondaries in the documented order).
 *
 * The login and rebuild call sites are one line each and are cited in
 * work/ffxiv-stats-sheet-20261123/DESIGN.md §1.5; they are not exercised here because
 * that needs a Player, a map and a session.
 */

#include "tc_catch2.h"

#include "FFXIVStatEngine.h"
#include "FFXIVStatTables.h"
#include "FFXIVStatsSnapshot.h"

#include <cstdint>
#include <vector>

namespace
{
using namespace FFXIV::Stats;

FfxivStatInputs Inputs(uint8 jobId, uint8 level)
{
    FfxivStatInputs inputs;
    inputs.jobId = jobId;
    inputs.level = level;
    inputs.effectiveLevel = level;
    return inputs;
}

uint16 ReadU16(std::vector<uint8> const& bytes, std::size_t offset)
{
    return uint16(bytes[offset]) | uint16(uint16(bytes[offset + 1]) << 8);
}

uint32 ReadU32(std::vector<uint8> const& bytes, std::size_t offset)
{
    return uint32(bytes[offset]) | (uint32(bytes[offset + 1]) << 8) |
           (uint32(bytes[offset + 2]) << 16) | (uint32(bytes[offset + 3]) << 24);
}
} // namespace

TEST_CASE("FFXIV stats snapshot payload is a fixed 43-byte little-endian image", "[ffxiv]")
{
    // Distinctive values in every position, so a swapped field or a wrong width fails
    // loudly. Nothing here is derived from a formula: this case pins the wire format.
    FfxivDerivedStats stats;
    stats.jobId = 21;
    stats.level = 80;
    stats.base[0] = 0x0102;     // STR
    stats.base[1] = 0x0304;     // DEX
    stats.base[2] = 0x0506;     // VIT
    stats.base[3] = 0x0708;     // INT
    stats.base[4] = 0x090A;     // MND
    stats.base[5] = 0x0B0C;     // PIE
    stats.param[FFXIV_PARAM_CRITICAL_HIT] = 0x1112;
    stats.param[FFXIV_PARAM_DIRECT_HIT_RATE] = 0x1314;
    stats.param[FFXIV_PARAM_DETERMINATION] = 0x1516;
    stats.param[FFXIV_PARAM_TENACITY] = 0x1718;
    stats.param[FFXIV_PARAM_SKILL_SPEED] = 0x191A;
    stats.param[FFXIV_PARAM_SPELL_SPEED] = 0x1B1C;
    stats.hpMax = 0x21222324;
    stats.mpMax = 0x31323334;
    stats.gcdMs = 0x00000A41;   // 2625 ms
    stats.provenance = 0x00000007;

    std::vector<uint8> const payload = BuildFfxivStatsSnapshotPayload(stats);

    REQUIRE(payload.size() == FFXIV_STATS_SNAPSHOT_BYTES);
    CHECK(FFXIV_STATS_SNAPSHOT_BYTES == 43);
    CHECK(FFXIV_SNAPSHOT_OFF_SCHEMA == 42);

    // job, level
    CHECK(payload[FFXIV_SNAPSHOT_OFF_JOB] == 21);
    CHECK(payload[FFXIV_SNAPSHOT_OFF_LEVEL] == 80);

    // six base values, in FfxivBaseStat order
    CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_BASE + 0) == 0x0102);
    CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_BASE + 2) == 0x0304);
    CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_BASE + 4) == 0x0506);
    CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_BASE + 6) == 0x0708);
    CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_BASE + 8) == 0x090A);
    CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_BASE + 10) == 0x0B0C);

    // six secondaries: CRIT, DIRECT HIT, DETERMINATION, TENACITY, SKILL SPEED, SPELL SPEED
    CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_SECONDARY + 0) == 0x1112);
    CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_SECONDARY + 2) == 0x1314);
    CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_SECONDARY + 4) == 0x1516);
    CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_SECONDARY + 6) == 0x1718);
    CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_SECONDARY + 8) == 0x191A);
    CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_SECONDARY + 10) == 0x1B1C);

    CHECK(ReadU32(payload, FFXIV_SNAPSHOT_OFF_HP) == 0x21222324u);
    CHECK(ReadU32(payload, FFXIV_SNAPSHOT_OFF_MP) == 0x31323334u);
    CHECK(ReadU32(payload, FFXIV_SNAPSHOT_OFF_GCD) == 2625u);
    CHECK(ReadU32(payload, FFXIV_SNAPSHOT_OFF_PROVENANCE) == 7u);
    CHECK(payload[FFXIV_SNAPSHOT_OFF_SCHEMA] == FFXIV_STATS_SNAPSHOT_SCHEMA);
    CHECK(FFXIV_STATS_SNAPSHOT_SCHEMA == 1);
}

TEST_CASE("FFXIV stats snapshot payload carries the engine snapshot verbatim", "[ffxiv]")
{
    // The same naked level-80 neutral snapshot the engine test pins: base 340, secondaries
    // at S = 380, hp 2000, mp 10000, gcd 2500, provenance 0x3.
    FfxivDerivedStats const stats = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 80));
    std::vector<uint8> const payload = BuildFfxivStatsSnapshotPayload(stats);

    REQUIRE(payload.size() == FFXIV_STATS_SNAPSHOT_BYTES);
    CHECK(payload[FFXIV_SNAPSHOT_OFF_JOB] == 0);
    CHECK(payload[FFXIV_SNAPSHOT_OFF_LEVEL] == 80);

    for (int i = 0; i < 6; ++i)
        CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_BASE + std::size_t(i) * 2) == 340);

    for (int i = 0; i < 6; ++i)
        CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_SECONDARY + std::size_t(i) * 2) == 380);

    CHECK(ReadU32(payload, FFXIV_SNAPSHOT_OFF_HP) == stats.hpMax);
    CHECK(ReadU32(payload, FFXIV_SNAPSHOT_OFF_HP) == 2000u);
    CHECK(ReadU32(payload, FFXIV_SNAPSHOT_OFF_MP) == stats.mpMax);
    CHECK(ReadU32(payload, FFXIV_SNAPSHOT_OFF_MP) == 10000u);
    CHECK(ReadU32(payload, FFXIV_SNAPSHOT_OFF_GCD) == stats.gcdMs);
    CHECK(ReadU32(payload, FFXIV_SNAPSHOT_OFF_GCD) == 2500u);
    CHECK(ReadU32(payload, FFXIV_SNAPSHOT_OFF_PROVENANCE) == stats.provenance);
    CHECK(payload[FFXIV_SNAPSHOT_OFF_SCHEMA] == FFXIV_STATS_SNAPSHOT_SCHEMA);

    SECTION("the job byte is the resolved ClassJob row, not the module class")
    {
        // Player::RebuildFFXIVStats passes GetFfxivClassJobId(GetClass()); module class 22
        // (Warrior) resolves to ClassJob row 21. The client must see 21 -- it has no
        // ClassJob map of its own and must never re-resolve the job.
        FfxivStatInputs inputs = Inputs(GetFfxivClassJobId(22), 80);
        FfxivDerivedStats const warrior = ComputeFfxivStats(inputs);
        std::vector<uint8> const body = BuildFfxivStatsSnapshotPayload(warrior);

        REQUIRE(body.size() == FFXIV_STATS_SNAPSHOT_BYTES);
        CHECK(body[FFXIV_SNAPSHOT_OFF_JOB] == 21);
        CHECK(body[FFXIV_SNAPSHOT_OFF_LEVEL] == 80);
        CHECK(ReadU16(body, FFXIV_SNAPSHOT_OFF_BASE) == uint16(warrior.base[0]));
        CHECK(ReadU32(body, FFXIV_SNAPSHOT_OFF_HP) == warrior.hpMax);
        CHECK(ReadU32(body, FFXIV_SNAPSHOT_OFF_MP) == 10000u);
        CHECK(ReadU32(body, FFXIV_SNAPSHOT_OFF_GCD) == warrior.gcdMs);
    }
}

TEST_CASE("FFXIV stats snapshot payload widens every u16 field the same way", "[ffxiv]")
{
    // Wire-width guard only: negative -> 0, above u16 -> 0xFFFF. No shipped value can hit
    // either bound; the case exists so the client never sees a wrapped number.
    FfxivDerivedStats stats;
    for (int i = 0; i < 6; ++i)
    {
        stats.base[i] = -1;
        stats.param[FFXIV_STATS_SNAPSHOT_SECONDARIES[i]] = 70000;
    }

    std::vector<uint8> const payload = BuildFfxivStatsSnapshotPayload(stats);
    REQUIRE(payload.size() == FFXIV_STATS_SNAPSHOT_BYTES);

    for (int i = 0; i < 6; ++i)
        CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_BASE + std::size_t(i) * 2) == 0);

    for (int i = 0; i < 6; ++i)
        CHECK(ReadU16(payload, FFXIV_SNAPSHOT_OFF_SECONDARY + std::size_t(i) * 2) == 0xFFFF);
}

TEST_CASE("FFXIV stats snapshot sub-opcode is a free custom id", "[ffxiv]")
{
    // 0x51F is the custom channel itself (server CMSG_CUSTOM / client CLIENT_TO_SERVER_OPCODE)
    // and 0x7A00..0x7A51 are allocated feature blocks; 0x7A60 starts the stats block.
    // See work/ffxiv-stats-sheet-20261123/DESIGN.md §1.3.
    CHECK(FFXIV_STATS_SNAPSHOT_OPCODE == 0x7A60);
    CHECK(FFXIV_STATS_SNAPSHOT_OPCODE > 0x7A51);
    CHECK(FFXIV_STATS_SNAPSHOT_OPCODE != 0x51F);
    CHECK(FFXIV_STATS_SNAPSHOT_OPCODE != 0x102);
}
