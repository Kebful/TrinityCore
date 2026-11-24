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

#include "FFXIV/Combat/FFXIVActionDamage.h"
#include "FFXIV/Combat/FFXIVCombatMath.h"
#include "FFXIVStatEngine.h"
#include "FFXIVStatTables.h"
#include "FFXIVStatsSnapshot.h"

#include <cstdint>
#include <cstdlib>
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

// --- GATE 7 fixtures ------------------------------------------------------------------------

// The Whm100 fixture of tests/game/FFXIVCombatMath.cpp, rebuilt through the same engine so the
// multiplier test and the damage goldens describe one character: WHM L100, MND 4000,
// all secondaries 442 -- f(ATK) 2017, f(DET) 1000, f(TNC) 1000, f(WD) 182 at weapon damage 132,
// trait 130, and a 100-potency non-crit mean-variance hit of 4772.
constexpr uint8 WHM = 24;

int32 JobPercentFor(uint8 jobId, uint8 attributeParam)
{
    FfxivJobRow const& job = GetFfxivJobRow(jobId);
    switch (attributeParam)
    {
        case FFXIV_PARAM_STRENGTH: return job.strPct;
        case FFXIV_PARAM_DEXTERITY: return job.dexPct;
        case FFXIV_PARAM_INTELLIGENCE: return job.intPct;
        case FFXIV_PARAM_MIND: return job.mndPct;
        default: return 100;
    }
}

FfxivDerivedStats Whm100()
{
    uint8 const attribute = FFXIV::Combat::GetFfxivAttackPowerAttribute(WHM);
    int32 const base = GetFfxivLevelRow(100).mainStat * JobPercentFor(WHM, attribute) / 100;

    FfxivStatInputs inputs = Inputs(WHM, 100);
    inputs.crit = 442;
    inputs.directHit = 442;
    inputs.determination = 442;
    inputs.tenacity = 442;
    inputs.skillSpeed = 442;
    inputs.spellSpeed = 442;
    inputs.equipmentParams[attribute] = 4000 - base;
    return ComputeFfxivStats(inputs);
}

// The client's decode contract, restated here so the same rules are proven server-side:
// the body must be exactly this schema's width and the last byte must be the schema number.
// Anything else is rejected whole. Mirrors DecodeSnapshot() in
// repos/clientdecomp/WoW/Source/Duskhaven/FFXIV/Stats/FFXIVStatsSnapshot.cpp.
bool DecodeSchema2ForTest(std::vector<uint8> const& bytes, uint8& job, uint8& level,
                          uint32& provenance, uint32& multiplierBp, uint8& schema)
{
    if (bytes.size() != FFXIV_STATS_SNAPSHOT_BYTES)
        return false;
    if (bytes[FFXIV_SNAPSHOT_OFF_SCHEMA] != FFXIV_STATS_SNAPSHOT_SCHEMA)
        return false;
    job = bytes[FFXIV_SNAPSHOT_OFF_JOB];
    level = bytes[FFXIV_SNAPSHOT_OFF_LEVEL];
    provenance = ReadU32(bytes, FFXIV_SNAPSHOT_OFF_PROVENANCE);
    multiplierBp = ReadU32(bytes, FFXIV_SNAPSHOT_OFF_MULTIPLIER);
    schema = bytes[FFXIV_SNAPSHOT_OFF_SCHEMA];
    return true;
}

// The potency table is process-global; every case here restores the empty shipping state.
struct ScopedPotencyTables
{
    ScopedPotencyTables(std::vector<FFXIV::Combat::FfxivActionPotencyRow> potency)
    {
        FFXIV::Combat::SetFfxivActionDamageTables(std::move(potency), {});
    }
    ~ScopedPotencyTables() { FFXIV::Combat::ClearFfxivActionDamageTables(); }
};
} // namespace

TEST_CASE("FFXIV stats snapshot payload is a fixed 47-byte little-endian image", "[ffxiv]")
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

    std::vector<uint8> const payload = BuildFfxivStatsSnapshotPayload(stats, 0x41424344u);

    REQUIRE(payload.size() == FFXIV_STATS_SNAPSHOT_BYTES);
    CHECK(FFXIV_STATS_SNAPSHOT_BYTES == 47);
    CHECK(FFXIV_SNAPSHOT_OFF_SCHEMA == 46);
    CHECK(FFXIV_SNAPSHOT_OFF_MULTIPLIER == 42);

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
    // Schema 2's one new field: the tooltip multiplier, in basis points.
    CHECK(ReadU32(payload, FFXIV_SNAPSHOT_OFF_MULTIPLIER) == 0x41424344u);
    CHECK(payload[FFXIV_SNAPSHOT_OFF_SCHEMA] == FFXIV_STATS_SNAPSHOT_SCHEMA);
    CHECK(FFXIV_STATS_SNAPSHOT_SCHEMA == 2);
}

TEST_CASE("FFXIV stats snapshot payload carries the engine snapshot verbatim", "[ffxiv]")
{
    // The same naked level-80 neutral snapshot the engine test pins: base 340, secondaries
    // at S = 380, hp 2000, mp 10000, gcd 2500, provenance 0x3.
    FfxivDerivedStats const stats = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 80));
    std::vector<uint8> const payload = BuildFfxivStatsSnapshotPayload(stats, 0);

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
        std::vector<uint8> const body = BuildFfxivStatsSnapshotPayload(warrior, 0);

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

    std::vector<uint8> const payload = BuildFfxivStatsSnapshotPayload(stats, 0);
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

    // GATE 7's second sub-opcode is its own id, one above the snapshot.
    CHECK(FFXIV_ACTION_POTENCY_OPCODE == 0x7A61);
    CHECK(FFXIV_ACTION_POTENCY_OPCODE != FFXIV_STATS_SNAPSHOT_OPCODE);
    CHECK(FFXIV_ACTION_POTENCY_SCHEMA == 1);
}

TEST_CASE("FFXIV tooltip multiplier equals the damage test's non-crit mean value", "[ffxiv]")
{
    // The vector the direct-damage goldens pin (tests/game/FFXIVCombatMath.cpp,
    // "FFXIV L100 direct damage: published stage values"): 100 potency, no crit, no direct hit,
    // variance 100 -> 4772. These are the five factors the multiplier multiplies:
    //   f(ATK) = trunc(237*(4000-440)/440)+100 = 2017
    //   f(DET) = 1000, f(TNC) = 1000, f(WD) = floor(440*115/1000)+132 = 182, trait = 130
    FFXIV::Combat::FfxivDerivedStats const stats = Whm100();

    int32 const bp = FFXIV::Combat::ComputeFfxivTooltipMultiplierBasisPoints(
        stats, 182, 100, FFXIV::Combat::FfxivPartyComposition(), FFXIV::Combat::FfxivTraitState{});
    // 2017 * 1000 * 1000 * 182 * 130 * 1000 / 10^11 = 477222.2 -> 477222.
    CHECK(bp == 477222);

    FFXIV::Combat::FfxivFixedRollSource rolls(99, 100); // no crit, no DH, variance 100
    FFXIV::Combat::FfxivDirectDamageResult const damage = FFXIV::Combat::ComputeFfxivDirectDamage(
        stats, 100, 182, 100, FFXIV::Combat::FfxivPartyComposition(), FFXIV::Combat::FfxivTraitState{},
        rolls);
    CHECK(damage.value == 4772);

    // The client's arithmetic -- potency * bp / 10000, integer division (floor) -- reproduces it.
    CHECK(int64(100) * bp / 10000 == damage.value);

    // The multiplier is exactly the product of the terms, not a second formula: five party
    // roles boost MND 4000 -> floor(4000*105/100) = 4200, f(ATK) becomes
    // trunc(237*3760/440)+100 = 2125, and the scalar moves by exactly that factor.
    CHECK(FFXIV::Combat::ComputeFfxivTooltipMultiplierBasisPoints(
              stats, 182, 100, FFXIV::Combat::FfxivPartyComposition(0x1F),
              FFXIV::Combat::FfxivTraitState{}) == 502775);
    // 2125 * 1000 * 1000 * 182 * 130 * 1000 / 10^11 = 502775.0

    // The trait is a switch, as in the damage chain: 100 instead of 130.
    CHECK(FFXIV::Combat::ComputeFfxivTooltipMultiplierBasisPoints(
              stats, 182, 100, FFXIV::Combat::FfxivPartyComposition(),
              FFXIV::Combat::FfxivTraitState{ false }) == 367094);
    // ... and the damages the goldens pin for that case land on the same integer.
    {
        FFXIV::Combat::FfxivFixedRollSource meanRolls(99, 100);
        CHECK(int64(100) * 367094 / 10000 ==
              FFXIV::Combat::ComputeFfxivDirectDamage(
                  stats, 100, 182, 100, FFXIV::Combat::FfxivPartyComposition(),
                  FFXIV::Combat::FfxivTraitState{ false }, meanRolls).value); // 3670
    }

    // The level correction is the same per-mille term: -2.5 % per level the attacker is below
    // the target. At the player's own level it is 1000 and inert.
    CHECK(FFXIV::Combat::ComputeFfxivTooltipMultiplierBasisPoints(
              stats, 182, 102, FFXIV::Combat::FfxivPartyComposition(),
              FFXIV::Combat::FfxivTraitState{}) == 453361);
    // 2017 * 1000 * 1000 * 182 * 130 * 950 / 10^11 = 453361.09

    // The one honest limit of a single scalar: the damage chain floors twice (D1 and D2), this
    // multiplier floors once, so only potency 100 is exact. Measured spread over the shipped
    // potency range is at most 3 points, which is why the client labels the line an estimate.
    int64 worst = 0;
    for (int32 potency = 20; potency <= 600; potency += 10)
    {
        FFXIV::Combat::FfxivFixedRollSource meanRolls(99, 100);
        int64 const exact = FFXIV::Combat::ComputeFfxivDirectDamage(
            stats, potency, 182, 100, FFXIV::Combat::FfxivPartyComposition(),
            FFXIV::Combat::FfxivTraitState{}, meanRolls).value;
        int64 const displayed = int64(potency) * bp / 10000;
        int64 const delta = displayed > exact ? displayed - exact : exact - displayed;
        if (delta > worst)
            worst = delta;
    }
    CHECK(worst <= 3);
}

TEST_CASE("FFXIV stats snapshot schema 2 round-trips and schema 1 is rejected", "[ffxiv]")
{
    FfxivDerivedStats const stats = Whm100();
    uint32 const bp = 477222;
    std::vector<uint8> const payload = BuildFfxivStatsSnapshotPayload(stats, bp);

    SECTION("schema 2 decode")
    {
        uint8 job = 0, level = 0, schema = 0;
        uint32 provenance = 0, multiplierBp = 0;
        REQUIRE(DecodeSchema2ForTest(payload, job, level, provenance, multiplierBp, schema));
        CHECK(job == WHM);
        CHECK(level == 100);
        CHECK(provenance == stats.provenance);
        CHECK(multiplierBp == bp);
        CHECK(schema == 2);
    }

    SECTION("a schema 1 body (43 bytes, schema byte at 42) is rejected whole")
    {
        std::vector<uint8> schema1(43, 0);
        schema1[0] = WHM;
        schema1[1] = 100;
        schema1[42] = 1; // the old trailing schema number

        uint8 job = 0, level = 0, schema = 0;
        uint32 provenance = 0, multiplierBp = 0;
        CHECK_FALSE(DecodeSchema2ForTest(schema1, job, level, provenance, multiplierBp, schema));
        // Nothing was written: the out-parameters still hold the caller's values.
        CHECK(job == 0);
        CHECK(multiplierBp == 0);
    }

    SECTION("an unknown schema number on a schema 2 width is rejected too")
    {
        std::vector<uint8> schema3 = payload;
        schema3[FFXIV_SNAPSHOT_OFF_SCHEMA] = 3;

        uint8 job = 0, level = 0, schema = 0;
        uint32 provenance = 0, multiplierBp = 0;
        CHECK_FALSE(DecodeSchema2ForTest(schema3, job, level, provenance, multiplierBp, schema));
    }
}

TEST_CASE("FFXIV action potency list is the plain column for one job and level", "[ffxiv]")
{
    // Rows for one action id across job/level, plus a heal-only row, plus an id the u16 wire
    // cannot carry: the hook's precedence must decide which single number each action sends.
    using FFXIV::Combat::FfxivActionPotencyRow;
    using FFXIV::Combat::FfxivActionPlainPotency;

    std::vector<FfxivActionPotencyRow> rows;
    auto add = [&rows](uint32 actionId, uint8 classJob, uint8 minLevel, uint16 damage, uint16 cure,
                       uint16 combo, uint16 flank)
    {
        FfxivActionPotencyRow row;
        row.actionId = actionId;
        row.classJob = classJob;
        row.minLevel = minLevel;
        row.damage = damage;
        row.cure = cure;
        row.combo = combo;
        row.flank = flank;
        rows.push_back(row);
    };

    add(31, 0, 1, 100, 0, 0, 0);    // Heavy Swing, generic
    add(31, 21, 50, 120, 0, 0, 0);  // Heavy Swing, WAR from 50
    add(31, 21, 80, 150, 0, 160, 170); // Heavy Swing, WAR from 80 (combo/flank not sent)
    add(133, 0, 1, 0, 500, 0, 0);   // a heal-only row: no plain damage
    add(7477, 0, 1, 200, 0, 0, 0);  // a starter action
    add(70000, 0, 1, 90, 0, 0, 0);  // beyond u16: skipped on the wire, never wrapped

    ScopedPotencyTables const scoped(std::move(rows));

    SECTION("precedence: exact job first, then the generic row")
    {
        std::vector<FfxivActionPlainPotency> const warrior =
            FFXIV::Combat::CollectFfxivActionPlainPotencies(21, 80);
        REQUIRE(warrior.size() == 3);
        CHECK(warrior[0].actionId == 31);
        CHECK(warrior[0].damage == 150);
        CHECK(warrior[1].actionId == 7477);
        CHECK(warrior[1].damage == 200);
        CHECK(warrior[2].actionId == 70000);

        // WHM never reads WAR's rows: action 31 falls back to the generic 100.
        std::vector<FfxivActionPlainPotency> const whm =
            FFXIV::Combat::CollectFfxivActionPlainPotencies(WHM, 80);
        REQUIRE(whm.size() == 3);
        CHECK(whm[0].damage == 100);

        // Below the job row's minLevel the generic row wins again.
        std::vector<FfxivActionPlainPotency> const lowWarrior =
            FFXIV::Combat::CollectFfxivActionPlainPotencies(21, 49);
        REQUIRE(lowWarrior.size() == 3);
        CHECK(lowWarrior[0].damage == 100);
    }

    SECTION("wire image: count u16, actionId u16 + damage u16 per entry, schema u8 last")
    {
        std::vector<FfxivActionPlainPotency> const warrior =
            FFXIV::Combat::CollectFfxivActionPlainPotencies(21, 80);
        std::vector<uint8> const payload = BuildFfxivActionPotencyPayload(warrior);

        // 70000 is skipped, so the wire carries two entries, not three.
        REQUIRE(payload.size() == FfxivActionPotencyPayloadBytes(2));
        CHECK(payload.size() == 3 + 2 * 4);
        CHECK(ReadU16(payload, 0) == 2);
        CHECK(ReadU16(payload, 2) == 31);
        CHECK(ReadU16(payload, 4) == 150);
        CHECK(ReadU16(payload, 6) == 7477);
        CHECK(ReadU16(payload, 8) == 200);
        CHECK(payload[10] == FFXIV_ACTION_POTENCY_SCHEMA);
    }

    SECTION("an empty table sends nothing the client could misread")
    {
        ScopedPotencyTables const empty(std::vector<FfxivActionPotencyRow>{});
        CHECK(FFXIV::Combat::CollectFfxivActionPlainPotencies(21, 80).empty());
        std::vector<uint8> const payload = BuildFfxivActionPotencyPayload({});
        REQUIRE(payload.size() == 3);
        CHECK(ReadU16(payload, 0) == 0);
        CHECK(payload[2] == FFXIV_ACTION_POTENCY_SCHEMA);
    }
}
