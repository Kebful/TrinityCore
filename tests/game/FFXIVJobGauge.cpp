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

#include "tc_catch2.h"

#include "FFXIV/Gauge/FFXIVJobGauge.h"

using namespace FFXIV::Gauge;

namespace
{
constexpr uint8 WAR = 21;
constexpr uint8 MRD = 3;
constexpr uint8 BEAST = 22;

ObjectGuid const Player1 = ObjectGuid::Create<HighGuid::Player>(1);
ObjectGuid const Player2 = ObjectGuid::Create<HighGuid::Player>(2);

// The rows tools/generate-ffxiv-job-gauges.py writes from the shipped sheets.
std::vector<FfxivJobGaugeRow> Gauges()
{
    return { { WAR, BEAST, "Beast Gauge", 100 } };
}

std::vector<FfxivActionGaugeRow> Deltas()
{
    return {
        { 37, WAR, BEAST, 35, 10, true },   // Maim
        { 42, WAR, BEAST, 35, 20, true },   // Storm's Path
        { 45, WAR, BEAST, 50, 10, true },   // Storm's Eye
        { 49, WAR, BEAST, 35, -50, false }, // Inner Beast
        { 51, WAR, BEAST, 45, -50, false }, // Steel Cyclone
        { 52, WAR, BEAST, 50, 50, false },  // Infuriate
    };
}

struct GaugeFixture
{
    GaugeFixture()
    {
        SetFfxivJobGaugeTables(Gauges(), Deltas());
        ClearFfxivGaugeState();
    }

    ~GaugeFixture()
    {
        ClearFfxivJobGaugeTables();
        ClearFfxivGaugeState();
    }
};
} // namespace

TEST_CASE("FFXIV gauge: an empty table changes nothing", "[ffxiv]")
{
    ClearFfxivJobGaugeTables();
    ClearFfxivGaugeState();

    CHECK(GetFfxivJobGaugeRow(WAR) == nullptr);
    CHECK(ResolveFfxivActionGaugeDelta(52, WAR, 50, false) == 0);
    CHECK(AddFfxivGauge(Player1, WAR, 50) == 0);
    CHECK(GetFfxivGauge(Player1, WAR) == 0);
    CHECK_FALSE(SpendFfxivGauge(Player1, WAR, 0));
    CHECK_FALSE(ApplyFfxivActionGauge(Player1, WAR, 50, 52, false));
}

TEST_CASE_METHOD(GaugeFixture, "FFXIV gauge: gain, spend and the clamp", "[ffxiv]")
{
    FfxivJobGaugeRow const* gauge = GetFfxivJobGaugeRow(WAR);
    REQUIRE(gauge != nullptr);
    CHECK(gauge->gaugeId == BEAST);
    CHECK(gauge->max == 100);

    CHECK(AddFfxivGauge(Player1, WAR, 30) == 30);
    CHECK(AddFfxivGauge(Player1, WAR, 90) == 100); // clamped at max
    CHECK(GetFfxivGauge(Player1, WAR) == 100);
    CHECK(GetFfxivGauge(Player2, WAR) == 0);       // per player

    CHECK(SpendFfxivGauge(Player1, WAR, 50));
    CHECK(GetFfxivGauge(Player1, WAR) == 50);
    CHECK_FALSE(SpendFfxivGauge(Player1, WAR, 60)); // all or nothing
    CHECK(GetFfxivGauge(Player1, WAR) == 50);
    CHECK(SpendFfxivGauge(Player1, WAR, 50));
    CHECK(GetFfxivGauge(Player1, WAR) == 0);

    CHECK(AddFfxivGauge(Player1, WAR, -20) == 0);   // clamped at 0
}

TEST_CASE_METHOD(GaugeFixture, "FFXIV gauge: the value belongs to one job", "[ffxiv]")
{
    CHECK(GetFfxivJobGaugeRow(MRD) == nullptr);
    CHECK(AddFfxivGauge(Player1, MRD, 50) == 0);    // Marauder has no gauge

    AddFfxivGauge(Player1, WAR, 40);
    CHECK(GetFfxivGauge(Player1, MRD) == 0);
    CHECK(GetFfxivGauge(Player1, WAR) == 40);
}

TEST_CASE_METHOD(GaugeFixture, "FFXIV gauge: action deltas follow combo and level", "[ffxiv]")
{
    // Combo Bonus gains need a correct combo.
    CHECK(ResolveFfxivActionGaugeDelta(37, WAR, 50, false) == 0);
    CHECK(ResolveFfxivActionGaugeDelta(37, WAR, 50, true) == 10);
    CHECK(ResolveFfxivActionGaugeDelta(42, WAR, 50, true) == 20);
    // Infuriate gains on every cast; a spender spends whether or not it was a combo.
    CHECK(ResolveFfxivActionGaugeDelta(52, WAR, 50, false) == 50);
    CHECK(ResolveFfxivActionGaugeDelta(49, WAR, 50, true) == -50);
    // minLevel: Maim's gain starts at 35, Storm's Eye's at 50.
    CHECK(ResolveFfxivActionGaugeDelta(37, WAR, 34, true) == 0);
    CHECK(ResolveFfxivActionGaugeDelta(45, WAR, 49, true) == 0);
    // Not a gauge action, or not the gauge's job.
    CHECK(ResolveFfxivActionGaugeDelta(31, WAR, 50, true) == 0);
    CHECK(ResolveFfxivActionGaugeDelta(37, MRD, 50, true) == 0);

    // Heavy Swing -> Maim -> Storm's Path, Infuriate, then Inner Beast twice.
    CHECK(ApplyFfxivActionGauge(Player1, WAR, 50, 37, true));
    CHECK(ApplyFfxivActionGauge(Player1, WAR, 50, 42, true));
    CHECK(GetFfxivGauge(Player1, WAR) == 30);
    CHECK(ApplyFfxivActionGauge(Player1, WAR, 50, 52, false));
    CHECK(GetFfxivGauge(Player1, WAR) == 80);
    CHECK(ApplyFfxivActionGauge(Player1, WAR, 50, 49, false));
    CHECK(GetFfxivGauge(Player1, WAR) == 30);
    CHECK(ApplyFfxivActionGauge(Player1, WAR, 50, 49, false)); // below cost: clamped, no cast gate yet
    CHECK(GetFfxivGauge(Player1, WAR) == 0);
    CHECK_FALSE(ApplyFfxivActionGauge(Player1, WAR, 50, 49, false)); // already 0: no change
}

TEST_CASE("FFXIV gauge: a delta for a gauge the job lacks is dropped", "[ffxiv]")
{
    SetFfxivJobGaugeTables(Gauges(), { { 37, WAR, 99, 1, 10, false }, { 52, MRD, BEAST, 1, 50, false } });
    CHECK(ResolveFfxivActionGaugeDelta(37, WAR, 50, false) == 0);
    CHECK(ResolveFfxivActionGaugeDelta(52, MRD, 50, false) == 0);
    ClearFfxivJobGaugeTables();
}

TEST_CASE("FFXIV gauge: the 0x7A62 payload", "[ffxiv]")
{
    CHECK(FFXIV_JOB_GAUGE_OPCODE == 0x7A62);

    FfxivJobGaugeRow const beast{ WAR, BEAST, "Beast Gauge", 100 };
    std::vector<uint8> const payload = BuildFfxivJobGaugePayload(&beast, 80);
    std::vector<uint8> const expected = {
        21, 22, 80, 0, 100, 0, 11,
        'B', 'e', 'a', 's', 't', ' ', 'G', 'a', 'u', 'g', 'e',
        FFXIV_JOB_GAUGE_SCHEMA,
    };
    CHECK(payload == expected);
    CHECK(BuildFfxivJobGaugePayload(&beast, 400)[2] == 100); // clamped to max

    std::vector<uint8> const none = BuildFfxivJobGaugePayload(nullptr, 0);
    CHECK(none == std::vector<uint8>{ 0, 0, 0, 0, 0, 0, 0, FFXIV_JOB_GAUGE_SCHEMA });
    CHECK(none.size() == FFXIV_JOB_GAUGE_FIXED_BYTES);
}
