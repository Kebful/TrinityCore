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

// The charge pool's arithmetic, the cast gate and the 0x7A64 payload. No database, no world: the
// rows below are the ones tools/generate-ffxiv-action-charges.py writes for these actions (Infuriate
// 52: 2 charges, 60 s; Shukuchi 2262: 1 charge at 40, 2 from 74), and every clock reading is passed
// in, so "one charge per rechargeMs" is proven, not observed.

#include "tc_catch2.h"

#include "FFXIV/Charges/FFXIVActionCharges.h"

#include <cstdint>
#include <vector>

using namespace FFXIV::Charges;

namespace
{
constexpr uint32 INFURIATE = 52;
constexpr uint32 SHUKUCHI = 2262;
constexpr uint8 INFURIATE_LEVEL = 50;
constexpr uint32 INFURIATE_RECHARGE = 60000;
constexpr uint32 T0 = 1000000; // an arbitrary "the player just cast" millisecond

ObjectGuid const Player1 = ObjectGuid::Create<HighGuid::Player>(1);
ObjectGuid const Player2 = ObjectGuid::Create<HighGuid::Player>(2);

std::vector<FfxivActionChargeRow> Rows()
{
    return {
        { INFURIATE, 50, 2, INFURIATE_RECHARGE },
        { SHUKUCHI, 40, 1, INFURIATE_RECHARGE },
        { SHUKUCHI, 74, 2, INFURIATE_RECHARGE },
    };
}

struct ChargeFixture
{
    ChargeFixture()
    {
        SetFfxivActionChargeRows(Rows());
        ClearFfxivActionChargeState();
    }

    ~ChargeFixture()
    {
        ClearFfxivActionChargeRows();
        ClearFfxivActionChargeState();
    }
};
} // namespace

TEST_CASE("FFXIV charges: an empty table changes nothing", "[ffxiv]")
{
    ClearFfxivActionChargeRows();
    ClearFfxivActionChargeState();

    CHECK_FALSE(HasFfxivActionChargeRows());
    CHECK(FindFfxivActionChargeRow(INFURIATE, INFURIATE_LEVEL) == nullptr);
    CHECK(GetFfxivActionChargeLevel(INFURIATE) == 0);
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0) == 0);
    // The gate's empty-table rule: an action with no row is always allowed.
    CHECK(HasFfxivChargeAvailable(Player1, INFURIATE, INFURIATE_LEVEL, T0));
    CHECK_FALSE(ConsumeFfxivCharge(Player1, INFURIATE, INFURIATE_LEVEL, T0));
    // The clear snapshot is a count byte and the schema byte.
    std::vector<uint8> const payload = BuildFfxivActionChargesPayload({});
    CHECK(payload.size() == 2);
    CHECK(payload[0] == 0);
    CHECK(payload[1] == FFXIV_ACTION_CHARGES_SCHEMA);
}

TEST_CASE_METHOD(ChargeFixture, "FFXIV charges: two charges, one back at +60 s, both at +120 s", "[ffxiv]")
{
    CHECK(HasFfxivActionChargeRows());
    CHECK(GetFfxivActionChargeLevel(INFURIATE) == 50);
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0) == 2);
    CHECK(GetFfxivChargeRemainingMs(Player1, INFURIATE, INFURIATE_LEVEL, T0) == 0); // full: nothing pending

    CHECK(ConsumeFfxivCharge(Player1, INFURIATE, INFURIATE_LEVEL, T0));
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0) == 1);
    CHECK(GetFfxivChargeRemainingMs(Player1, INFURIATE, INFURIATE_LEVEL, T0) == INFURIATE_RECHARGE);
    // Nothing comes back early, and the remaining time decays exactly.
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 59999) == 1);
    CHECK(GetFfxivChargeRemainingMs(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 59999) == 1);
    // One charge at +60 s: recharging again, still one short of the maximum.
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 60000) == 2);
    CHECK(GetFfxivChargeRemainingMs(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 60000) == 0);

    // Empty the pool, then both charges come back on the same schedule: one at +60 s, two at +120 s.
    CHECK(ConsumeFfxivCharge(Player1, INFURIATE, INFURIATE_LEVEL, T0));
    CHECK(ConsumeFfxivCharge(Player1, INFURIATE, INFURIATE_LEVEL, T0));
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0) == 0);
    CHECK(GetFfxivChargeRemainingMs(Player1, INFURIATE, INFURIATE_LEVEL, T0) == INFURIATE_RECHARGE);
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 59999) == 0);
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 60000) == 1);
    CHECK(GetFfxivChargeRemainingMs(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 60000) == INFURIATE_RECHARGE);
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 119999) == 1);
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 120000) == 2);
    CHECK(GetFfxivChargeRemainingMs(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 120000) == 0);
}

TEST_CASE_METHOD(ChargeFixture, "FFXIV charges: a second cast mid-recharge keeps the first schedule", "[ffxiv]")
{
    CHECK(ConsumeFfxivCharge(Player1, INFURIATE, INFURIATE_LEVEL, T0));
    // The second charge is spent 30 s later; the recharge that was already running is not restarted.
    CHECK(ConsumeFfxivCharge(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 30000));
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 30000) == 0);
    CHECK(GetFfxivChargeRemainingMs(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 30000) == 30000);
    // The recharge that started at T0 is not restarted: the first charge is back at +60 s and the
    // second 60 s after that.
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 60000) == 1);
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 90000) == 1);
    CHECK(AvailableFfxivCharges(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 120000) == 2);
}

TEST_CASE_METHOD(ChargeFixture, "FFXIV charges: an empty pool refuses and a single charge allows", "[ffxiv]")
{
    CHECK(HasFfxivChargeAvailable(Player1, INFURIATE, INFURIATE_LEVEL, T0));
    CHECK(ConsumeFfxivCharge(Player1, INFURIATE, INFURIATE_LEVEL, T0));
    CHECK(HasFfxivChargeAvailable(Player1, INFURIATE, INFURIATE_LEVEL, T0)); // one still there
    CHECK(ConsumeFfxivCharge(Player1, INFURIATE, INFURIATE_LEVEL, T0));
    CHECK_FALSE(HasFfxivChargeAvailable(Player1, INFURIATE, INFURIATE_LEVEL, T0)); // the gate says no
    CHECK_FALSE(ConsumeFfxivCharge(Player1, INFURIATE, INFURIATE_LEVEL, T0));      // and nothing is taken
    CHECK(HasFfxivChargeAvailable(Player1, INFURIATE, INFURIATE_LEVEL, T0 + 60000)); // one is back

    // State is per player: another player's pool is untouched.
    CHECK(AvailableFfxivCharges(Player2, INFURIATE, INFURIATE_LEVEL, T0) == 2);
}

TEST_CASE_METHOD(ChargeFixture, "FFXIV charges: the level band picks the pool", "[ffxiv]")
{
    CHECK(FindFfxivActionChargeRow(SHUKUCHI, 40) != nullptr);
    CHECK(FindFfxivActionChargeRow(SHUKUCHI, 40)->maxCharges == 1);
    CHECK(FindFfxivActionChargeRow(SHUKUCHI, 73)->maxCharges == 1);
    CHECK(FindFfxivActionChargeRow(SHUKUCHI, 74)->maxCharges == 2);
    CHECK(FindFfxivActionChargeRow(SHUKUCHI, 100)->maxCharges == 2);
    // Below the action's own level there is no pool at all (the spell is not usable either).
    CHECK(FindFfxivActionChargeRow(SHUKUCHI, 39) == nullptr);
    CHECK(FindFfxivActionChargeRow(INFURIATE, 49) == nullptr);

    // A one-charge band is a plain cooldown: one cast, then nothing until the recharge.
    CHECK(AvailableFfxivCharges(Player1, SHUKUCHI, 40, T0) == 1);
    CHECK(ConsumeFfxivCharge(Player1, SHUKUCHI, 40, T0));
    CHECK_FALSE(HasFfxivChargeAvailable(Player1, SHUKUCHI, 40, T0));
    CHECK(AvailableFfxivCharges(Player1, SHUKUCHI, 40, T0 + 60000) == 1);

    // Levelling into the two-charge band: the stored pool clamps up to the new maximum.
    CHECK(AvailableFfxivCharges(Player1, SHUKUCHI, 74, T0 + 60000) == 2);
}

TEST_CASE_METHOD(ChargeFixture, "FFXIV charges: the snapshot is count, 14-byte entries, schema", "[ffxiv]")
{
    std::vector<FfxivActionChargeEntry> entries;
    // Deliberately out of order: the builder sorts by spell id so a snapshot is comparable byte for
    // byte with the one the tick last sent.
    entries.push_back(FfxivActionChargeEntry{ 0x0000BBBBu, 1, 3, 15000, 30000 });
    entries.push_back(FfxivActionChargeEntry{ 0x0000AAAAu, 2, 2, 0, 60000 });
    std::vector<uint8> const payload = BuildFfxivActionChargesPayload(entries);

    CHECK(payload.size() == 1 + 2 * FFXIV_ACTION_CHARGES_ENTRY_BYTES + 1);
    CHECK(payload[0] == 2);
    // First entry, ascending: spell 0x0000AAAA, current 2, max 2, remaining 0, recharge 60000.
    CHECK(payload[1] == 0xAA);
    CHECK(payload[2] == 0xAA);
    CHECK(payload[3] == 0x00);
    CHECK(payload[4] == 0x00);
    CHECK(payload[5] == 2);
    CHECK(payload[6] == 2);
    CHECK((payload[7] == 0 && payload[8] == 0 && payload[9] == 0 && payload[10] == 0));
    // 60000 = 0x0000EA60.
    CHECK(payload[11] == 0x60);
    CHECK(payload[12] == 0xEA);
    CHECK(payload[13] == 0x00);
    CHECK(payload[14] == 0x00);
    // Second entry starts at 1 + 14.
    CHECK(payload[15] == 0xBB);
    CHECK(payload[16] == 0xBB);
    CHECK(payload[17] == 0);
    CHECK(payload[18] == 0);
    CHECK(payload[19] == 1);
    CHECK(payload[20] == 3);
    // 15000 = 0x00003A98.
    CHECK(payload[21] == 0x98);
    CHECK(payload[22] == 0x3A);
    CHECK(payload[23] == 0);
    CHECK(payload[24] == 0);
    // 30000 = 0x00007530.
    CHECK(payload[25] == 0x30);
    CHECK(payload[26] == 0x75);
    CHECK(payload[27] == 0);
    CHECK(payload[28] == 0);
    CHECK(payload[payload.size() - 1] == FFXIV_ACTION_CHARGES_SCHEMA);

    // A current above max is a server bug: the entry is dropped, not sent.
    std::vector<uint8> const bad = BuildFfxivActionChargesPayload({ FfxivActionChargeEntry{ 7u, 4, 2, 0, 1000 } });
    CHECK(bad.size() == 2);
    CHECK(bad[0] == 0);
}

TEST_CASE_METHOD(ChargeFixture, "FFXIV charges: a malformed row is dropped at load", "[ffxiv]")
{
    SetFfxivActionChargeRows({
        { 0, 50, 2, 60000 },            // no action
        { INFURIATE, 50, 0, 60000 },    // no charges
        { INFURIATE, 50, 2, 0 },        // no recharge
        { INFURIATE, 50, 2, 60000 },    // the one that survives
        { INFURIATE, 50, 5, 1000 },     // duplicate key: ignored
    });
    CHECK(FindFfxivActionChargeRow(INFURIATE, 50) != nullptr);
    CHECK(FindFfxivActionChargeRow(INFURIATE, 50)->maxCharges == 2);
    CHECK(FindFfxivActionChargeRow(INFURIATE, 50)->rechargeMs == 60000);
}
