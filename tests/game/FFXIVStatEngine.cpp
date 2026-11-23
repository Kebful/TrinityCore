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
 * FFXIV derived-stat slice 1 (I-REFRESH-20261123 §3.4).
 *
 * Every expectation below is either
 *   - transcribed from the shipped sheets (ClassJob c9-16, ParamGrow c5/c6) with
 *     work/exd-dump.py as the independent reader, or
 *   - the published worked example of B-FORMULA-REFERENCE F8-F12/F20 and the
 *     golden vector table of I §3.4, recomputed by hand in the comment.
 *
 * Units: rate.* is true basis points (10000 == 1.0). I §3.4 publishes det as
 * "1010", tenacity as "1005/991" and crit damage as "10409" -- the first two are
 * per-mille numerators (x10 = 10100 / 10050 / 9910 bp), and 1409 is the crit
 * multiplier per-mille (14090 bp). The comments show both scales.
 */

#include "tc_catch2.h"

#include "DBCStores.h"
#include "DummyData.h"
#include "FFXIVStatEngine.h"
#include "FFXIVStatProjection.h"
#include "FFXIVStatTables.h"
#include "Unit.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace FFXIV::Stats;

namespace
{
FfxivStatInputs Inputs(uint8 jobId, uint8 level)
{
    FfxivStatInputs inputs;
    inputs.jobId = jobId;
    inputs.level = level;
    inputs.effectiveLevel = level;
    return inputs;
}

// Level 80 with all secondaries at X (the golden vectors use X == 442, i.e. a
// geared character; the naked snapshot uses the level base S == 380).
FfxivStatInputs GearedAt80(int32 x)
{
    FfxivStatInputs inputs = Inputs(FFXIV_NEUTRAL_JOB_ID, 80);
    inputs.crit = x;
    inputs.directHit = x;
    inputs.determination = x;
    inputs.tenacity = x;
    inputs.skillSpeed = x;
    inputs.spellSpeed = x;
    return inputs;
}

struct ExpectedJobRow
{
    uint8 jobId;
    uint16 hpPct, mpPct, strPct, vitPct, dexPct, intPct, mndPct, piePct;
};

// work/exd-dump.py ClassJob --cols 0,1,3,9,10,11,12,13,14,15,16 (all 44 rows).
constexpr ExpectedJobRow kSheetJobRows[] =
{
    {  0, 100, 100, 100, 100, 100, 100, 100, 100 },
    {  1, 130, 100,  95, 100,  90,  50,  95, 100 },
    {  2, 105, 100, 100,  95, 100,  45,  85, 100 },
    {  3, 135, 100, 100, 100,  90,  30,  50, 100 },
    {  4, 110, 100, 105, 100,  95,  40,  60, 100 },
    {  5, 100, 100,  85,  95, 105,  80,  75, 100 },
    {  6, 100, 100,  50,  95, 100, 100, 105, 100 },
    {  7, 100, 100,  40,  95,  95, 105,  70, 100 },
    {  8, 100, 100,  90, 105, 100,  90,  90, 100 },
    {  9, 100, 100, 105,  90,  90,  90, 100, 100 },
    { 10, 100, 100, 100, 105,  90,  90,  90, 100 },
    { 11, 100, 100,  90,  90, 105, 100,  90, 100 },
    { 12, 100, 100,  90, 105,  90, 100,  90, 100 },
    { 13, 100, 100,  90,  90, 105,  90, 100, 100 },
    { 14, 100, 100,  90,  90,  90, 105,  90, 100 },
    { 15, 100, 100,  90,  90,  90,  90, 105, 100 },
    { 16, 100, 100, 100,  90,  90,  90, 100, 100 },
    { 17, 100, 100,  90,  90, 100, 100,  90, 100 },
    { 18, 100, 100, 100, 100, 100, 100, 100, 100 },
    { 19, 140, 100, 100, 110,  95,  60, 100, 100 },
    { 20, 110, 100, 110, 100, 105,  50,  90, 100 },
    { 21, 145, 100, 105, 110,  95,  40,  55, 100 },
    { 22, 115, 100, 115, 105, 100,  45,  65, 100 },
    { 23, 105, 100,  90, 100, 115,  85,  80, 100 },
    { 24, 105, 100,  55, 100, 105, 105, 115, 100 },
    { 25, 105, 100,  45, 100, 100, 115,  75, 100 },
    { 26, 100, 100,  85,  95,  95, 105,  75, 100 },
    { 27, 105, 100,  90, 100, 100, 115,  80, 100 },
    { 28, 105, 100,  90, 100, 100, 105, 115, 100 },
    { 29, 103, 100,  80,  95, 100,  60,  70, 100 },
    { 30, 108, 100,  85, 100, 110,  65,  75, 100 },
    { 31, 105, 100,  85, 100, 115,  80,  85, 100 },
    { 32, 140, 100, 105, 110,  95,  60,  40, 100 },
    { 33, 105, 100,  50, 100, 100, 105, 115, 100 },
    { 34, 109, 100, 112, 100, 108,  60,  50, 100 },
    { 35, 105, 100,  55, 100, 105, 115, 110, 100 },
    { 36, 105, 100,  70, 100, 110, 115, 105, 100 },
    { 37, 140, 100, 100, 110,  95,  60, 100, 100 },
    { 38, 105, 100,  90, 100, 115,  85,  80, 100 },
    { 39, 115, 100, 115, 105, 100,  80,  40, 100 },
    { 40, 105, 100,  60, 100, 100, 115, 115, 100 },
    { 41, 111, 100, 100, 100, 110,  45,  55, 100 },
    { 42, 105, 100,  50, 100, 110, 115,  80, 100 },
    { 43, 110, 100, 110, 100, 105,  50,  90, 100 },
};

struct ExpectedLevelSheetRow
{
    uint8 level;
    int32 baseSubStat; // ParamGrow c5
    int32 levelDiv;    // ParamGrow c6
};

// work/exd-dump.py ParamGrow --cols 4,5,6 (anchors + samples between anchors).
constexpr ExpectedLevelSheetRow kSheetLevelRows[] =
{
    {   1,   56,   56 },
    {   2,   57,   57 },
    {  30,  173,  173 },
    {  50,  341,  341 },
    {  51,  342,  366 },
    {  60,  354,  600 },
    {  70,  364,  900 },
    {  79,  378, 1260 },
    {  80,  380, 1300 },
    {  90,  400, 1900 },
    { 100,  420, 2780 },
};
} // namespace

TEST_CASE("FFXIV tables match the shipped sheets", "[ffxiv]")
{
    ValidateFfxivStatTables();

    SECTION("manifest is the one the generator wrote")
    {
        // Fixture: regenerate the tables and update this hash together.
        CHECK(GetFfxivTableManifest() == 0x67BC815Cu);
        CHECK(GetFfxivTableSchemaVersion() == 1u);
        CHECK(GetFfxivTableManifestSha256() ==
              "699cbbcfd8a56a61e954bbd755df2f48f092d421dd2123bb8634853b3584fa45");
        CHECK(GetFfxivTableSourceSheets() == "work/exd-all/exd");
    }

    SECTION("ParamGrow c5/c6 per level")
    {
        for (ExpectedLevelSheetRow const& expected : kSheetLevelRows)
        {
            FfxivLevelRow const& row = GetFfxivLevelRow(expected.level);
            INFO("level " << uint32(expected.level));
            CHECK(row.level == expected.level);
            CHECK(row.baseSubStat == expected.baseSubStat);
            CHECK(row.levelDiv == expected.levelDiv);
        }
    }

    SECTION("model anchors are the published level table")
    {
        // B §F1: MAIN / base HP / VIT->HP scalars (x10), plus ParamGrow S/D.
        struct { uint8 level; int32 main, sub, div, hp, tank, other; bool interpolated; } const anchors[] =
        {
            {   1,  20,  56,   56,   86,   0,   0, false },
            {  50, 202, 341,  341, 1700, 155, 112, false },
            {  60, 218, 354,  600, 1500, 175, 129, false },
            {  70, 292, 364,  900, 1700, 188, 140, false },
            {  80, 340, 380, 1300, 2000, 266, 188, false },
            {  90, 390, 400, 1900, 3000, 346, 243, false },
            { 100, 440, 420, 2780, 4000, 430, 301, false },
        };
        for (auto const& anchor : anchors)
        {
            FfxivLevelRow const& row = GetFfxivLevelRow(anchor.level);
            INFO("level " << uint32(anchor.level));
            CHECK(row.mainStat == anchor.main);
            CHECK(row.baseSubStat == anchor.sub);
            CHECK(row.levelDiv == anchor.div);
            CHECK(row.baseHp == anchor.hp);
            CHECK(row.hpScalarTankTenths == anchor.tank);
            CHECK(row.hpScalarOtherTenths == anchor.other);
            CHECK(((row.provenance & FFXIV_PROV_LEVEL_INTERPOLATED) != 0) == anchor.interpolated);
        }
    }

    SECTION("ClassJob c9-16 for every shipped row")
    {
        REQUIRE(std::size(kSheetJobRows) == FFXIV_JOB_COUNT);
        for (ExpectedJobRow const& expected : kSheetJobRows)
        {
            FfxivJobRow const& row = GetFfxivJobRow(expected.jobId);
            INFO("job " << uint32(expected.jobId) << " (" << row.abbreviation << ")");
            CHECK(row.jobId == expected.jobId);
            CHECK(row.hpPct == expected.hpPct);
            CHECK(row.mpPct == expected.mpPct);
            CHECK(row.strPct == expected.strPct);
            CHECK(row.vitPct == expected.vitPct);
            CHECK(row.dexPct == expected.dexPct);
            CHECK(row.intPct == expected.intPct);
            CHECK(row.mndPct == expected.mndPct);
            CHECK(row.piePct == expected.piePct);
        }
    }

    SECTION("job type and role columns")
    {
        CHECK(GetFfxivJobRow(19).role == FFXIV_JOB_ROLE_TANK);   // PLD
        CHECK(GetFfxivJobRow(21).role == FFXIV_JOB_ROLE_TANK);   // WAR
        CHECK(GetFfxivJobRow(32).role == FFXIV_JOB_ROLE_TANK);   // DRK
        CHECK(GetFfxivJobRow(37).role == FFXIV_JOB_ROLE_TANK);   // GNB
        CHECK(GetFfxivJobRow(24).jobType == FFXIV_JOB_TYPE_DOM); // WHM
        CHECK(GetFfxivJobRow(25).jobType == FFXIV_JOB_TYPE_DOM); // BLM
        CHECK(GetFfxivJobRow(23).jobType == FFXIV_JOB_TYPE_DOW); // BRD
        CHECK(GetFfxivJobRow(8).jobType == FFXIV_JOB_TYPE_DOH);  // CRP
        CHECK(GetFfxivJobRow(16).jobType == FFXIV_JOB_TYPE_DOL); // MIN
    }

    SECTION("Tribe modifiers are the shipped rows")
    {
        FfxivTribeRow const& midlander = GetFfxivTribeRow(1);
        CHECK(midlander.str == 2);
        CHECK(midlander.vit == 0);
        CHECK(midlander.dex == -1);
        CHECK(midlander.int_ == 3);
        CHECK(midlander.mnd == -1);
        CHECK(midlander.pie == 0);
        for (uint8 tribeId = 0; tribeId < FFXIV_TRIBE_COUNT; ++tribeId)
        {
            FfxivTribeRow const& row = GetFfxivTribeRow(tribeId);
            CHECK(row.hp == 0);
            CHECK(row.mp == 0);
            CHECK(row.pie == 0);
        }
    }
}

TEST_CASE("FFXIV module class map (characters.class -> ClassJob)", "[ffxiv]")
{
    // Module class display names from
    // release/modules/ffxivhaven/datascripts/classes/{arr-classes,arr-jobs}.ts, class
    // ids from datascripts/shared/content.ts::FfxivClassIDs, ClassJob ids/abbreviations
    // from work/exd-dump.py ClassJob --cols 0,1 (row name == lowercased display name).
    struct ExpectedClassMapRow
    {
        uint8 classId;
        char const* name;
        uint8 classJobId;
        char const* abbreviation;
    };
    constexpr ExpectedClassMapRow kExpected[] =
    {
        { 12, "Gladiator",   1, "GLA" },
        { 13, "Marauder",    3, "MRD" },
        { 14, "Lancer",      4, "LNC" },
        { 15, "Pugilist",    2, "PGL" },
        { 16, "Rogue",      29, "ROG" }, // TS key ffxivrogue; ClassJob 29 rogue, not 30 ninja
        { 17, "Archer",      5, "ARC" },
        { 18, "Conjurer",    6, "CNJ" },
        { 19, "Thaumaturge", 7, "THM" },
        { 20, "Arcanist",   26, "ACN" },
        { 21, "Paladin",    19, "PLD" },
        { 22, "Warrior",    21, "WAR" },
        { 23, "Dragoon",    22, "DRG" },
        { 24, "Monk",       20, "MNK" },
        { 25, "Ninja",      30, "NIN" },
        { 26, "Bard",       23, "BRD" },
        { 27, "White Mage", 24, "WHM" },
        { 28, "Black Mage", 25, "BLM" },
        { 29, "Summoner",   27, "SMN" },
        { 30, "Scholar",    28, "SCH" },
    };

    SECTION("every module class maps to its ClassJob row by name")
    {
        REQUIRE(std::size(kExpected) == FFXIV_MODULE_CLASS_COUNT);
        CHECK(GetFfxivClassJobMapCount() == FFXIV_MODULE_CLASS_COUNT);
        for (ExpectedClassMapRow const& expected : kExpected)
        {
            INFO("module class " << uint32(expected.classId) << " (" << expected.name << ")");
            FfxivClassJobMapRow const* row = FindFfxivClassJobMapRow(expected.classId);
            REQUIRE(row != nullptr);
            CHECK(row->classId == expected.classId);
            CHECK(row->classJobId == expected.classJobId);
            CHECK(std::string(row->name) == expected.name);
            CHECK(GetFfxivClassJobId(expected.classId) == expected.classJobId);
            CHECK(std::string(GetFfxivJobRow(expected.classJobId).abbreviation) == expected.abbreviation);
        }
    }

    SECTION("the generated table is one row per module class, in class order, no duplicates")
    {
        for (uint8 i = 0; i < FFXIV_MODULE_CLASS_COUNT; ++i)
        {
            FfxivClassJobMapRow const& row = GetFfxivClassJobMapRow(i);
            INFO("row " << uint32(i));
            CHECK(row.classId == kExpected[i].classId);
            CHECK(row.classJobId == kExpected[i].classJobId);
            for (uint8 j = uint8(i + 1); j < FFXIV_MODULE_CLASS_COUNT; ++j)
                CHECK(row.classId != GetFfxivClassJobMapRow(j).classId);
        }
    }

    SECTION("a class outside the module resolves to the neutral row and never aborts")
    {
        // Stock WoW classes 1..11 and anything above the module allocation. The
        // class map must tolerate them: any class can log in.
        for (uint8 classId : { uint8(0), uint8(1), uint8(2), uint8(11), uint8(31), uint8(255) })
        {
            INFO("class " << uint32(classId));
            CHECK(FindFfxivClassJobMapRow(classId) == nullptr);
            CHECK(GetFfxivClassJobId(classId) == FFXIV_NEUTRAL_JOB_ID);
        }
    }

    SECTION("a mapped class builds the snapshot with that job's percentages")
    {
        // class 13 Marauder -> ClassJob 3: hp 135 %, mp 100 %, STR 100 %, VIT 100 %,
        // DEX 90 %, INT 30 %, MND 50 %, PIE 100 % (ClassJob c9-16). L80 M = 340.
        FfxivDerivedStats marauder = ComputeFfxivStats(Inputs(GetFfxivClassJobId(13), 80));
        CHECK(marauder.jobId == 3);
        CHECK(marauder.base[0] == 340);   // STR 100 %
        CHECK(marauder.base[1] == 306);   // DEX  90 %
        CHECK(marauder.base[2] == 340);   // VIT 100 %
        CHECK(marauder.base[3] == 102);   // INT  30 %
        CHECK(marauder.base[4] == 170);   // MND  50 %
        CHECK(marauder.base[5] == 340);   // PIE 100 %
        // F3: fl(2000*135/100) + fl((340-340)*18.8) = 2700
        CHECK(marauder.hpMax == 2700);
        CHECK(marauder.mpMax == FFXIV_FIXED_MAX_MP);
        CHECK(marauder.gcdMs == 2500);
        std::string const block = FormatFfxivStatsBlock(marauder);
        INFO(block);
        CHECK(block.find("job  3 MRD  level 80") != std::string::npos);

        // class 22 Warrior -> ClassJob 21: hp 145 %, VIT 110 %.
        // fl(2000*145/100) + fl((374-340)*26.6) = 2900 + 904 = 3804
        FfxivDerivedStats warrior = ComputeFfxivStats(Inputs(GetFfxivClassJobId(22), 80));
        CHECK(warrior.jobId == 21);
        CHECK(warrior.base[0] == 357);    // STR 105 %
        CHECK(warrior.base[2] == 374);    // VIT 110 %
        CHECK(warrior.hpMax == 3804);
        std::string const warriorBlock = FormatFfxivStatsBlock(warrior);
        INFO(warriorBlock);
        CHECK(warriorBlock.find("job 21 WAR  level 80") != std::string::npos);
    }
}

TEST_CASE("FFXIV level-80 golden vectors (I §3.4)", "[ffxiv]")
{
    SECTION("crit 442 -> 5.90 % / 1.409x")
    {
        // 200*(442-380)/1300 = 9.538 -> fl 9; (9+50)/1000 = 5.90 %; (1400+9)/1000 = 1.409
        FfxivDerivedStats stats = ComputeFfxivStats(GearedAt80(442));
        CHECK(stats.rate.critBp == 590);
        CHECK(stats.rate.critDamageBp == 14090);
    }

    SECTION("direct hit 442 -> 2.60 %")
    {
        // fl(550*62/1300) = fl(26.23) = 26 -> 2.60 %
        FfxivDerivedStats stats = ComputeFfxivStats(GearedAt80(442));
        CHECK(stats.rate.dhBp == 260);
    }

    SECTION("determination 442 -> 1.010x")
    {
        // fl(140*(442-340)/1300) = fl(10.98) = 10 -> (1000+10)/1000
        FfxivDerivedStats stats = ComputeFfxivStats(GearedAt80(442));
        CHECK(stats.rate.detBp == 10100); // I §3.4 publishes the per-mille 1010
    }

    SECTION("tenacity 442 -> 1005 / 991")
    {
        // out (1000 + fl(112*62/1300))/1000 = (1000+5)/1000
        // in  (1000 - fl(200*62/1300))/1000 = (1000-9)/1000
        FfxivDerivedStats stats = ComputeFfxivStats(GearedAt80(442));
        CHECK(stats.rate.tncOutBp == 10050); // published per-mille 1005
        CHECK(stats.rate.tncInBp == 9910);   // published per-mille 991
    }

    SECTION("speed 442 with base 2500 ms -> 2485 ms")
    {
        // q = trunc(130*(380-442)/1300) = trunc(-6.2) = -6; F = 994; 2500*994/1000 = 2485
        FfxivDerivedStats stats = ComputeFfxivStats(GearedAt80(442));
        CHECK(stats.rate.sksBp == 9940);
        CHECK(stats.rate.spsBp == 9940);
        CHECK(stats.gcdSksMs == 2485);
        CHECK(stats.gcdSpsMs == 2485);
        CHECK(stats.gcdMs == 2485);
    }

    SECTION("speed 300 (below base) -> signed truncation, 2520 ms")
    {
        // q = trunc(130*(380-300)/1300) = 8 (positive); F = 1008; 2500*1008/1000 = 2520
        FfxivStatInputs inputs = Inputs(FFXIV_NEUTRAL_JOB_ID, 80);
        inputs.skillSpeed = 300;
        FfxivDerivedStats stats = ComputeFfxivStats(inputs);
        CHECK(stats.rate.sksBp == 10080);
        CHECK(stats.gcdSksMs == 2520);
    }

    SECTION("piety 442 -> MP tick 211")
    {
        // PIE 442 = job-0 base 340 + 102 of equipment (F2 total()); the formula
        // is 200 + fl(150*(442-340)/1300) = 200 + fl(11.77) = 211
        FfxivStatInputs inputs = Inputs(FFXIV_NEUTRAL_JOB_ID, 80);
        inputs.equipmentParams[FFXIV_PARAM_PIETY] = 102;
        FfxivDerivedStats stats = ComputeFfxivStats(inputs);
        CHECK(stats.param[FFXIV_PARAM_PIETY] == 442);
        CHECK(stats.mpTick == 211);
    }
}

TEST_CASE("FFXIV rate boundaries around the level base", "[ffxiv]")
{
    // S = 380 at level 80; the boundary set is X in {S-1, S, S+1}. The model's
    // fl() floors a negative delta (crit 379 -> 4.90 %), while F12's native stage
    // truncates toward zero (speed 381 -> q = trunc(-0.1) = 0, so no change).
    SECTION("crit chance / crit damage")
    {
        CHECK(ComputeFfxivStats(GearedAt80(379)).rate.critBp == 490);      // fl(-0.1538) = -1
        CHECK(ComputeFfxivStats(GearedAt80(380)).rate.critBp == 500);      // fl(0) = 0
        CHECK(ComputeFfxivStats(GearedAt80(381)).rate.critBp == 500);      // fl(0.1538) = 0
        CHECK(ComputeFfxivStats(GearedAt80(379)).rate.critDamageBp == 13990);
        CHECK(ComputeFfxivStats(GearedAt80(380)).rate.critDamageBp == 14000);
        CHECK(ComputeFfxivStats(GearedAt80(381)).rate.critDamageBp == 14000);
    }

    SECTION("direct hit")
    {
        CHECK(ComputeFfxivStats(GearedAt80(379)).rate.dhBp == -10);        // fl(-0.423) = -1
        CHECK(ComputeFfxivStats(GearedAt80(380)).rate.dhBp == 0);
        CHECK(ComputeFfxivStats(GearedAt80(381)).rate.dhBp == 0);
    }

    SECTION("determination (baseline is the level main stat M = 340)")
    {
        CHECK(ComputeFfxivStats(GearedAt80(339)).rate.detBp == 9990);
        CHECK(ComputeFfxivStats(GearedAt80(340)).rate.detBp == 10000);
        CHECK(ComputeFfxivStats(GearedAt80(341)).rate.detBp == 10000);
    }

    SECTION("tenacity")
    {
        CHECK(ComputeFfxivStats(GearedAt80(379)).rate.tncOutBp == 9990);
        CHECK(ComputeFfxivStats(GearedAt80(379)).rate.tncInBp == 10010);
        CHECK(ComputeFfxivStats(GearedAt80(380)).rate.tncOutBp == 10000);
        CHECK(ComputeFfxivStats(GearedAt80(380)).rate.tncInBp == 10000);
        CHECK(ComputeFfxivStats(GearedAt80(381)).rate.tncOutBp == 10000);
        CHECK(ComputeFfxivStats(GearedAt80(381)).rate.tncInBp == 10000);
    }

    SECTION("speed keeps signed truncation, so +/-1 point is a no-op")
    {
        CHECK(ComputeFfxivStats(GearedAt80(379)).gcdSksMs == 2500);
        CHECK(ComputeFfxivStats(GearedAt80(380)).gcdSksMs == 2500);
        CHECK(ComputeFfxivStats(GearedAt80(381)).gcdSksMs == 2500);
        CHECK(ComputeFfxivStats(GearedAt80(379)).rate.sksBp == 10000);
        CHECK(ComputeFfxivStats(GearedAt80(381)).rate.sksBp == 10000);
    }

    SECTION("MP tick (baseline is the level main stat M = 340)")
    {
        // PIE 340 = job-0 base, so a {-1,0,+1} PIE step is a {-1,0,+1} equipment delta.
        auto withPiety = [](int32 delta)
        {
            FfxivStatInputs inputs = Inputs(FFXIV_NEUTRAL_JOB_ID, 80);
            inputs.equipmentParams[FFXIV_PARAM_PIETY] = delta;
            return ComputeFfxivStats(inputs);
        };
        CHECK(withPiety(-1).param[FFXIV_PARAM_PIETY] == 339);
        CHECK(withPiety(-1).mpTick == 199);
        CHECK(withPiety(0).mpTick == 200);
        CHECK(withPiety(+1).mpTick == 200);
    }
}

TEST_CASE("FFXIV level-100 anchors (B §F20)", "[ffxiv]")
{
    // S = 420, D = 2780, M = 440. The server caps at level 80; these rows exist
    // precisely so one code path reproduces the published L100 examples.
    FfxivStatInputs inputs = Inputs(FFXIV_NEUTRAL_JOB_ID, 100);
    inputs.crit = 442;
    inputs.directHit = 442;
    inputs.determination = 442;
    inputs.tenacity = 442;
    inputs.skillSpeed = 442;
    inputs.spellSpeed = 442;
    FfxivDerivedStats stats = ComputeFfxivStats(inputs);

    CHECK(stats.rate.critBp == 510);        // fl(200*22/2780 + 50) = 51 -> 5.1 %
    CHECK(stats.rate.critDamageBp == 14010); // (1400 + 1)/1000 = 1.401
    CHECK(stats.rate.dhBp == 40);            // fl(550*22/2780) = 4 -> 0.4 %
    CHECK(stats.rate.detBp == 10000);        // fl(140*2/2780) = 0 -> 1.000x
    CHECK(stats.rate.tncOutBp == 10000);     // fl(112*22/2780) = 0
    CHECK(stats.rate.tncInBp == 9990);       // (1000 - fl(200*22/2780))/1000 = 0.999
    CHECK(stats.gcdSksMs == 2497);           // q = trunc(130*(-22)/2780) = -1; 2500*999/1000
    CHECK(stats.mpTick == 200);              // 200 + fl(150*2/2780)
}

TEST_CASE("FFXIV naked snapshot at the level cap", "[ffxiv]")
{
    // These are the values `.ffxivstats` prints for a level-80 character today:
    // neutral job 0, no clan, no secondaries (naked -> S), no FFXIV gear.
    FfxivDerivedStats neutral = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 80));

    CHECK(neutral.rulesetVersion == GetFfxivTableManifest());
    CHECK(neutral.jobId == 0);
    CHECK(neutral.level == 80);
    CHECK(neutral.effectiveLevel == 80);
    CHECK(neutral.provenance == (FFXIV_PROV_EQUIPMENT_PARAMS_ZERO | FFXIV_PROV_BASE_MAIN_MODEL));

    for (int32 base : neutral.base)
        CHECK(base == 340);                          // trunc(340 * 100 / 100)
    CHECK(neutral.param[FFXIV_PARAM_STRENGTH] == 340);
    CHECK(neutral.param[FFXIV_PARAM_DEXTERITY] == 340);
    CHECK(neutral.param[FFXIV_PARAM_VITALITY] == 340);
    CHECK(neutral.param[FFXIV_PARAM_INTELLIGENCE] == 340);
    CHECK(neutral.param[FFXIV_PARAM_MIND] == 340);
    CHECK(neutral.param[FFXIV_PARAM_PIETY] == 340);

    // naked secondaries sit at the level base S
    CHECK(neutral.param[FFXIV_PARAM_CRITICAL_HIT] == 380);
    CHECK(neutral.param[FFXIV_PARAM_DIRECT_HIT_RATE] == 380);
    CHECK(neutral.param[FFXIV_PARAM_DETERMINATION] == 380);
    CHECK(neutral.param[FFXIV_PARAM_TENACITY] == 380);
    CHECK(neutral.param[FFXIV_PARAM_SKILL_SPEED] == 380);
    CHECK(neutral.param[FFXIV_PARAM_SPELL_SPEED] == 380);
    CHECK(neutral.param[FFXIV_PARAM_HASTE] == 100);

    CHECK(neutral.rate.critBp == 500);
    CHECK(neutral.rate.critDamageBp == 14000);
    CHECK(neutral.rate.dhBp == 0);
    // F8's baseline is the level main stat M (340 at L80), not S (380), so a
    // naked DET at the U4 floor of S is 1.004x: fl(140*40/1300) = 4.
    // B §F20 pins M as the baseline (DET 442 / M 440 -> exactly 1.000x).
    CHECK(neutral.rate.detBp == 10040);
    CHECK(neutral.rate.tncOutBp == 10000);
    CHECK(neutral.rate.tncInBp == 10000);
    CHECK(neutral.rate.sksBp == 10000);
    CHECK(neutral.rate.spsBp == 10000);
    CHECK(neutral.gcdMs == 2500);
    CHECK(neutral.gcdSksMs == 2500);
    CHECK(neutral.gcdSpsMs == 2500);

    // F3: fl(2000*100/100) + fl((340-340)*18.8)
    CHECK(neutral.hpMax == 2000);
    CHECK(neutral.mpMax == 10000);
    CHECK(neutral.mpTick == 200);
}

TEST_CASE("FFXIV job and level samples", "[ffxiv]")
{
    SECTION("PLD level 80: VIT 110 %, tank HP scalar")
    {
        FfxivDerivedStats stats = ComputeFfxivStats(Inputs(19, 80));
        CHECK(stats.base[0] == 340);   // STR 100 %
        CHECK(stats.base[1] == 323);   // DEX  95 %
        CHECK(stats.base[2] == 374);   // VIT 110 %
        CHECK(stats.base[3] == 204);   // INT  60 %
        // fl(2000*140/100) + fl((374-340)*26.6) = 2800 + 904
        CHECK(stats.hpMax == 3704);
        CHECK(stats.gcdMs == 2500);    // DoW -> skill speed
    }

    SECTION("WHM level 80: healer, spell-speed GCD, no tank scalar")
    {
        FfxivDerivedStats stats = ComputeFfxivStats(Inputs(24, 80));
        CHECK(stats.base[2] == 340);   // VIT 100 %
        // fl(2000*105/100) + fl((340-340)*18.8) = 2100
        CHECK(stats.hpMax == 2100);
        CHECK(stats.gcdMs == stats.gcdSpsMs);
    }

    SECTION("PLD level 1: interpolated row, flagged")
    {
        FfxivDerivedStats stats = ComputeFfxivStats(Inputs(19, 1));
        CHECK(stats.level == 1);
        CHECK(stats.provenance == (FFXIV_PROV_EQUIPMENT_PARAMS_ZERO | FFXIV_PROV_BASE_MAIN_MODEL));
        CHECK(stats.base[0] == 20);    // trunc(20*100/100)
        CHECK(stats.base[1] == 19);    // trunc(20*95/100)
        CHECK(stats.base[2] == 22);    // trunc(20*110/100)
        CHECK(stats.base[3] == 12);    // trunc(20*60/100)
        CHECK(stats.param[FFXIV_PARAM_CRITICAL_HIT] == 56);  // level base S
        // fl(86*140/100) + fl((22-20)*0.0) = 120
        CHECK(stats.hpMax == 120);
        CHECK(stats.gcdMs == 2500);
        CHECK(stats.rate.critBp == 500);  // at X == S
    }

    SECTION("WHM level 1")
    {
        FfxivDerivedStats stats = ComputeFfxivStats(Inputs(24, 1));
        // fl(86*105/100) + fl((20-20)*0.0) = 90
        CHECK(stats.hpMax == 90);
        CHECK(stats.mpMax == 10000);
    }

    SECTION("an interpolated mid-level is flagged, an anchor is not")
    {
        CHECK(HasProvenance(ComputeFfxivStats(Inputs(0, 79)), FFXIV_PROV_LEVEL_INTERPOLATED));
        CHECK_FALSE(HasProvenance(ComputeFfxivStats(Inputs(0, 80)), FFXIV_PROV_LEVEL_INTERPOLATED));
        CHECK_FALSE(HasProvenance(ComputeFfxivStats(Inputs(0, 50)), FFXIV_PROV_LEVEL_INTERPOLATED));
    }

    SECTION("equipment params clear the provenance bit")
    {
        FfxivStatInputs inputs = Inputs(19, 80);
        inputs.equipmentParams[FFXIV_PARAM_CRITICAL_HIT] = 62;
        FfxivDerivedStats stats = ComputeFfxivStats(inputs);
        CHECK(stats.param[FFXIV_PARAM_CRITICAL_HIT] == 442);
        CHECK_FALSE(HasProvenance(stats, FFXIV_PROV_EQUIPMENT_PARAMS_ZERO));
        CHECK(stats.rate.critBp == 590);   // same 442 vector, reached through F2 total()
    }

    SECTION("printable samples (this section is the offline .ffxivstats sample)")
    {
        // Two jobs x two levels. Run `./tests "[ffxiv]" -s` to see the blocks;
        // they are what `.ffxivstats` prints for a character with that job and
        // level (job 0/19/24 differ only by the sheet percentages).
        struct { uint8 jobId; uint8 level; } const samples[] =
        {
            { 19, 80 }, // PLD
            { 24, 80 }, // WHM
            { 19, 1 },  // PLD
            { 24, 1 },  // WHM
        };
        for (auto const& sample : samples)
        {
            std::string block = FormatFfxivStatsBlock(ComputeFfxivStats(Inputs(sample.jobId, sample.level)));
            INFO(block);
            CHECK(!block.empty());
        }
    }
}

TEST_CASE("FFXIV .ffxivstats block is a stable fixture", "[ffxiv]")
{
    // Level-80 neutral character: exactly what `.ffxivstats` prints today.
    std::string const level80 = FormatFfxivStatsBlock(ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 80)));
    INFO(level80);
    CHECK(level80 ==
          "FFXIV derived stats  ruleset 67BC815C  schema 1\n"
          "job  0 ---  level 80  effective 80\n"
          "  STR  340  DEX  340  VIT  340  INT  340  MND  340  PIE  340\n"
          "  CRIT  380  DH  380  DET  380  TNC  380  SKS  380  SPS  380\n"
          "  crit   5.00%  damage  1.400x  direct hit   0.00%\n"
          "  det   1.004x  tenacity out  1.000x  in  1.000x\n"
          "  speed sks 10000 bp  sps 10000 bp  gcd 2500 ms (skill)\n"
          "  hp   2000  mp  10000  mp/tick  200\n"
          "  provenance 0x3 [equipmentParams=0 baseMainModel]\n");

    // Level-1 WHM: the interpolated row is visible in the provenance line.
    std::string const level1 = FormatFfxivStatsBlock(ComputeFfxivStats(Inputs(24, 1)));
    INFO(level1);
    CHECK(level1 ==
          "FFXIV derived stats  ruleset 67BC815C  schema 1\n"
          "job 24 WHM  level  1  effective  1\n"
          "  STR   11  DEX   21  VIT   20  INT   21  MND   23  PIE   20\n"
          "  CRIT   56  DH   56  DET   56  TNC   56  SKS   56  SPS   56\n"
          "  crit   5.00%  damage  1.400x  direct hit   0.00%\n"
          "  det   1.090x  tenacity out  1.000x  in  1.000x\n"
          "  speed sks 10000 bp  sps 10000 bp  gcd 2500 ms (spell)\n"
          "  hp     90  mp  10000  mp/tick  200\n"
          "  provenance 0x3 [equipmentParams=0 baseMainModel]\n");
}

/*
 * Phase 1 projection (FFXIV/Stats/FFXIVStatProjection.h).
 *
 * The applier needs a live Player, so these tests pin the two things that decide
 * what it can write: the pure mapping and the gate. The no-write property of a
 * gate-off build is a source/build fact, checked by grepping for Unit setters and
 * by reading the `if constexpr` in PlayerFFXIVStats.cpp, not by a unit test.
 */

TEST_CASE("FFXIV projection gate is off by default", "[ffxiv]")
{
    // Shipping default. This line is the review gate: flipping the constant is a
    // modelling change (FFXIV owns the sheet) and whoever flips it must update this
    // test in the same commit. It is deliberately not a cvar and not a DB switch.
    CHECK_FALSE(kFfxivStatProjectionEnabled);
}

TEST_CASE("FFXIV -> core field mapping is positional and constexpr", "[ffxiv]")
{
    SECTION("the mapper runs in a constant expression (no Player, no DB, no table)")
    {
        constexpr FfxivDerivedStats empty{};
        constexpr FfxivWowStats mappedEmpty = MapFfxivStatsToWow(empty);
        static_assert(mappedEmpty.strength == 0 && mappedEmpty.agility == 0 &&
                      mappedEmpty.intellect == 0 && mappedEmpty.spirit == 0 &&
                      mappedEmpty.stamina == 0 && mappedEmpty.health == 0 && mappedEmpty.mana == 0);
    }

    SECTION("each base index lands on its own core field")
    {
        // Distinct sentinels: a swap (VIT->SPI, MND->STA) would be visible.
        FfxivDerivedStats stats;
        stats.base[0] = 1;   // STR
        stats.base[1] = 2;   // DEX
        stats.base[2] = 3;   // VIT
        stats.base[3] = 4;   // INT
        stats.base[4] = 5;   // MND
        stats.base[5] = 6;   // PIE
        stats.hpMax = 7;
        stats.mpMax = 8;

        FfxivWowStats const mapped = MapFfxivStatsToWow(stats);
        CHECK(mapped.strength == 1);    // <- STR
        CHECK(mapped.agility == 2);     // <- DEX
        CHECK(mapped.stamina == 3);     // <- VIT, not PIE
        CHECK(mapped.intellect == 4);   // <- INT
        CHECK(mapped.spirit == 5);      // <- MND, not VIT
        CHECK(mapped.health == 7);      // <- hpMax
        CHECK(mapped.mana == 8);        // <- mpMax
    }

    SECTION("PIE is dropped: it reaches no core field")
    {
        // Every field that could plausibly absorb Piety is pinned to zero, and PIE
        // alone is non-zero (base[5] = 987654). Nothing used as a WoW field may
        // change, so Piety cannot be leaking into Spirit/Intellect/anything.
        FfxivDerivedStats stats;
        stats.base[5] = 987654;         // PIE
        stats.hpMax = 7;
        stats.mpMax = FFXIV_FIXED_MAX_MP;

        FfxivWowStats const mapped = MapFfxivStatsToWow(stats);
        CHECK(mapped.strength == 0);
        CHECK(mapped.agility == 0);
        CHECK(mapped.intellect == 0);
        CHECK(mapped.spirit == 0);
        CHECK(mapped.stamina == 0);
        CHECK(mapped.health == 7);      // HP/MP come from F3/F4, not from PIE
        CHECK(mapped.mana == FFXIV_FIXED_MAX_MP);
    }
}

TEST_CASE("FFXIV -> core field mapping for six module classes", "[ffxiv]")
{
    // Level 80, naked, classJob percentages from ClassJob c9-16 (the table in
    // FFXIVStatEngine.cpp's "ClassJob c9-16 for every shipped row" case):
    // base[i] = trunc(340 * jobPct(i) / 100) at L80 (F2, M = 340).
    struct ExpectedMappingRow
    {
        uint8 classId;
        char const* name;
        uint8 jobId;
        char const* abbreviation;
        int32 str, dex, vit, int_, mnd;   // FFXIV base[], L80
    };
    constexpr ExpectedMappingRow kExpected[] =
    {
        // STR main, tank
        { 13, "Marauder",    3, "MRD", 340, 306, 340, 102, 170 },
        // STR main, tank, VIT 110 %
        { 22, "Warrior",    21, "WAR", 357, 323, 374, 136, 187 },
        { 21, "Paladin",    19, "PLD", 340, 323, 374, 204, 340 },
        // DEX main
        { 17, "Archer",      5, "ARC", 289, 357, 323, 272, 255 },
        // INT main
        { 19, "Thaumaturge", 7, "THM", 136, 323, 323, 357, 238 },
        // MND main
        { 18, "Conjurer",    6, "CNJ", 170, 340, 323, 340, 357 },
    };

    for (ExpectedMappingRow const& expected : kExpected)
    {
        INFO("module class " << uint32(expected.classId) << " (" << expected.name << ")");
        uint8 const jobId = GetFfxivClassJobId(expected.classId);
        REQUIRE(jobId == expected.jobId);
        REQUIRE(std::string(GetFfxivJobRow(jobId).abbreviation) == expected.abbreviation);

        FfxivDerivedStats const stats = ComputeFfxivStats(Inputs(jobId, 80));
        FfxivWowStats const mapped = MapFfxivStatsToWow(stats);

        // The FFXIV base attributes are the source of truth for the mapping.
        CHECK(stats.base[0] == expected.str);
        CHECK(stats.base[1] == expected.dex);
        CHECK(stats.base[2] == expected.vit);
        CHECK(stats.base[3] == expected.int_);
        CHECK(stats.base[4] == expected.mnd);

        // What the projection would write.
        CHECK(mapped.strength == expected.str);
        CHECK(mapped.agility == expected.dex);
        CHECK(mapped.stamina == expected.vit);          // VIT -> STA
        CHECK(mapped.intellect == expected.int_);
        CHECK(mapped.spirit == expected.mnd);           // MND -> SPI
        CHECK(mapped.health == stats.hpMax);            // hp <- F3
        CHECK(mapped.mana == stats.mpMax);              // mp <- F4
        CHECK(mapped.mana == FFXIV_FIXED_MAX_MP);       // F4 is the fixed 10 000
    }

    SECTION("the HP side of the mapping on the three sheet-verified cases")
    {
        // F3 values already pinned by "FFXIV module class map" above:
        // MRD fl(2000*135/100) + fl((340-340)*18.8) = 2700
        CHECK(MapFfxivStatsToWow(ComputeFfxivStats(Inputs(GetFfxivClassJobId(13), 80))).health == 2700);
        // WAR fl(2000*145/100) + fl((374-340)*26.6) = 2900 + 904 = 3804
        CHECK(MapFfxivStatsToWow(ComputeFfxivStats(Inputs(GetFfxivClassJobId(22), 80))).health == 3804);
        // PLD fl(2000*140/100) + fl((374-340)*26.6) = 2800 + 904 = 3704
        CHECK(MapFfxivStatsToWow(ComputeFfxivStats(Inputs(GetFfxivClassJobId(21), 80))).health == 3704);
    }

    SECTION("every module class projects consistently at the gameplay levels")
    {
        // No hardcoded numbers here: the mapping must agree with the snapshot it was
        // handed, for all 19 module classes and both ends of the shipped level range.
        for (uint8 i = 0; i < FFXIV_MODULE_CLASS_COUNT; ++i)
        {
            uint8 const classId = GetFfxivClassJobMapRow(i).classId;
            uint8 const jobId = GetFfxivClassJobId(classId);
            for (uint8 level : { uint8(1), uint8(50), uint8(80) })
            {
                INFO("class " << uint32(classId) << " job " << uint32(jobId) << " level " << uint32(level));
                FfxivDerivedStats const stats = ComputeFfxivStats(Inputs(jobId, level));
                FfxivWowStats const mapped = MapFfxivStatsToWow(stats);
                CHECK(mapped.strength == stats.base[0]);
                CHECK(mapped.agility == stats.base[1]);
                CHECK(mapped.stamina == stats.base[2]);
                CHECK(mapped.intellect == stats.base[3]);
                CHECK(mapped.spirit == stats.base[4]);
                CHECK(mapped.health == stats.hpMax);
                CHECK(mapped.mana == stats.mpMax);
            }
        }
    }
}

TEST_CASE("FFXIV projection boundary snapshots", "[ffxiv]")
{
    SECTION("level 1: the smallest row the gameplay path can build")
    {
        // PLD level 1 (values pinned by "FFXIV job and level samples"): base
        // 20/19/22/12 and F3 hp 120.
        FfxivDerivedStats const stats = ComputeFfxivStats(Inputs(19, 1));
        FfxivWowStats const mapped = MapFfxivStatsToWow(stats);

        CHECK(stats.base[0] == 20);
        CHECK(stats.base[1] == 19);
        CHECK(stats.base[2] == 22);
        CHECK(stats.base[3] == 12);
        CHECK(stats.hpMax == 120);

        CHECK(mapped.strength == 20);
        CHECK(mapped.agility == 19);
        CHECK(mapped.stamina == 22);
        CHECK(mapped.intellect == 12);
        CHECK(mapped.health == 120);
        CHECK(mapped.mana == FFXIV_FIXED_MAX_MP);
        // PLD MND is 100 % of M = 20 at level 1, so VIT 110 % -> 22 and MND -> 20
        // are two different numbers: the VIT->STA / MND->SPI split is visible.
        CHECK(mapped.spirit == 20);
    }

    SECTION("level 80 cap: neutral job, every field at the level base")
    {
        FfxivDerivedStats const stats = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 80));
        FfxivWowStats const mapped = MapFfxivStatsToWow(stats);

        CHECK(mapped.strength == 340);
        CHECK(mapped.agility == 340);
        CHECK(mapped.stamina == 340);
        CHECK(mapped.intellect == 340);
        CHECK(mapped.spirit == 340);
        CHECK(mapped.health == 2000);
        CHECK(mapped.mana == 10000);
    }

    SECTION("level 100 table anchor: the mapping is level-agnostic")
    {
        // The engine ships L90/L100 rows for B §F20; the projection must carry them
        // unchanged instead of clamping to the gameplay cap of 80.
        FfxivDerivedStats const stats = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 100));
        FfxivWowStats const mapped = MapFfxivStatsToWow(stats);

        CHECK(stats.level == 100);
        CHECK(mapped.strength == stats.base[0]);
        CHECK(mapped.agility == stats.base[1]);
        CHECK(mapped.stamina == stats.base[2]);
        CHECK(mapped.intellect == stats.base[3]);
        CHECK(mapped.spirit == stats.base[4]);
        CHECK(mapped.health == stats.hpMax);
        CHECK(mapped.mana == stats.mpMax);
    }
}

TEST_CASE("FFXIV rebuild log path is unchanged by the gate", "[ffxiv]")
{
    // The phase-1a log line compares the model against the live fields. That
    // arithmetic is now a pure helper so it can be pinned here: the log must keep
    // printing snapshot-minus-live deltas while the projection is gated off.

    SECTION("the delta is snapshot minus live")
    {
        FfxivDerivedStats const stats = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 80));
        REQUIRE(stats.hpMax == 2000);
        REQUIRE(stats.mpMax == 10000);

        FfxivProjectionDelta const equal = ComputeFfxivProjectionDelta(stats, 2000, 10000);
        CHECK(equal.hp == 0);
        CHECK(equal.mp == 0);

        // Model above the live field -> positive delta (the logged meaning: the
        // sign is always snapshot - live, never live - snapshot).
        FfxivProjectionDelta const above = ComputeFfxivProjectionDelta(stats, 1500, 12000);
        CHECK(above.hp == 500);
        CHECK(above.mp == -2000);
    }

    SECTION("the delta is computed in 64 bits")
    {
        // A live max above INT32_MAX must not wrap: the old local was int64 too, and
        // the log's {:+} formatting depends on the signed wide type.
        FfxivDerivedStats const stats = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 80));
        FfxivProjectionDelta const delta = ComputeFfxivProjectionDelta(stats, 3000000000u, 10000);
        CHECK(delta.hp == int64(2000) - int64(3000000000u));
        CHECK(delta.hp < int64(INT32_MIN) / 2);   // would have wrapped in 32 bits
        CHECK(delta.mp == 0);
    }
}

/*
 * Phase 1b: crit chance -> rating inversion.
 *
 * The core's forward path (Player::UpdateCritPercentage, StatSystem.cpp) is
 *
 *   PLAYER_CRIT_PERCENTAGE = baseFlatMod(aura) + basePctMod(agility)
 *                          + rating[CR_CRIT_MELEE] * GetRatingMultiplier()
 *                          + (GetWeaponSkillValue() - GetMaxSkillValueForLevel()) * 0.04
 *
 * and the applier zeroes basePctMod, subtracts the skill term, and leaves the aura
 * term alone. The multipliers below are not constants: the shipped DBC rows are
 * loaded at their real positions, then read back through
 * GetCombatRatingMultiplier() -- the function Player::GetRatingMultiplier() delegates
 * to -- so the test exercises the same lookup the player path uses.
 *
 * Rows transcribed 2026-11-23 from
 *   release/modules/default/datasets/dataset/dbc/gtCombatRatings.dbc
 *     format "f": no id column, so id = cr * GT_MAX_LEVEL + level - 1 = position
 *   release/modules/default/datasets/dataset/dbc/gtOCTClassCombatRatingScalar.dbc
 *     format "df": 'd' is the sorted column (not part of the entry), so
 *     id = (class-1) * GT_MAX_RATING + cr + 1 = position
 * Neither file is read at test time; the suite stays hermetic.
 */

namespace
{
// Module class 13 is Marauder (FFXIVStatTables kClassJobMap -> ClassJob 3). The
// shipped scalar table carries 1.0 for CR_CRIT_* on every module class 12..30, so
// the multiplier there is 1 / the gtCombatRatings row.
constexpr uint8 kCritTestClass = 13;

// The shipped scalar table's zero block: class 10 does not exist in the class list,
// so all 32 of its rows are 0.0 -- a real "multiplier is 0" case, not a synthetic one.
constexpr uint8 kCritZeroScalarClass = 10;

struct CritRatingRow
{
    uint8 cr;
    uint8 level;
    float data;
};

// gtCombatRatings rows for CR_CRIT_MELEE (8), CR_CRIT_RANGED (9), CR_CRIT_SPELL (10).
// Listed per rating deliberately: the three are equal in this file, and a table that
// ever diverges must show up here rather than be papered over.
constexpr CritRatingRow kGtCombatRatingRows[] =
{
    {  8,   1, 0.5384619832038879f },  // id 800
    {  8,  50, 11.307692527770996f },  // id 849
    {  8,  80, 45.90598678588867f },   // id 879
    {  8, 100, 198.48641967773438f },  // id 899
    {  9,   1, 0.5384619832038879f },  // id 900
    {  9,  50, 11.307692527770996f },  // id 949
    {  9,  80, 45.90598678588867f },   // id 979
    {  9, 100, 198.48641967773438f },  // id 999
    { 10,   1, 0.5384619832038879f },  // id 1000
    { 10,  50, 11.307692527770996f },  // id 1049
    { 10,  80, 45.90598678588867f },   // id 1079
    { 10, 100, 198.48641967773438f },  // id 1099
};

UnitTestDataLoader::FlatDBC<GtCombatRatingsEntry> critRatingStore(sGtCombatRatingsStore);
UnitTestDataLoader::FlatDBC<GtOCTClassCombatRatingScalarEntry> critScalarStore(sGtOCTClassCombatRatingScalarStore);

void LoadCritRatingStores()
{
    if (!critRatingStore.Empty() || !critScalarStore.Empty())
        return;

    {
        auto guard = critRatingStore.Loader();
        for (CritRatingRow const& row : kGtCombatRatingRows)
            guard.At(uint32(row.cr) * GT_MAX_LEVEL + row.level - 1).Data = row.data;
    }

    {
        auto guard = critScalarStore.Loader();
        for (uint32 cr = 0; cr < MAX_COMBAT_RATING; ++cr)
        {
            guard.At((uint32(kCritTestClass) - 1) * GT_MAX_RATING + cr + 1).Data = 1.0f;
            guard.At((uint32(kCritZeroScalarClass) - 1) * GT_MAX_RATING + cr + 1).Data = 0.0f;
        }
    }
}

CombatRating CritRatingForSlot(uint8 slot)
{
    switch (slot)
    {
        case FFXIV_CRIT_SLOT_MELEE:
            return CR_CRIT_MELEE;
        case FFXIV_CRIT_SLOT_RANGED:
            return CR_CRIT_RANGED;
        default:
            return CR_CRIT_SPELL;
    }
}

FfxivCritCoreTerms CoreTermsForClass(uint8 classId, uint8 level, float meleeTerm = 0.0f,
                                     float rangedTerm = 0.0f, float spellTerm = 0.0f)
{
    FfxivCritCoreTerms core;
    for (uint8 slot = 0; slot < FFXIV_CRIT_SLOT_COUNT; ++slot)
        core.ratingMultiplier[slot] = GetCombatRatingMultiplier(classId, level, CritRatingForSlot(slot));
    core.baseTermPercent[FFXIV_CRIT_SLOT_MELEE] = meleeTerm;
    core.baseTermPercent[FFXIV_CRIT_SLOT_RANGED] = rangedTerm;
    core.baseTermPercent[FFXIV_CRIT_SLOT_SPELL] = spellTerm;
    return core;
}

bool NearlyEqual(float a, float b, float tolerance)
{
    return std::fabs(a - b) <= tolerance;
}
} // namespace

TEST_CASE("FFXIV crit rating inversion round-trips the snapshot crit chance", "[ffxiv]")
{
    LoadCritRatingStores();

    SECTION("naked snapshot (5.00 %) at L1 / L50 / L80 / L100")
    {
        struct Vector
        {
            uint8 level;
            uint32 rating;          // round(5.00 / multiplier), see below
            float multiplier;       // classRating->Data / Rating->Data
            float residualPercent;  // rating * multiplier - 5.00
        };

        // The expected multiplier is the transcribed DBC ratio (1.0 / row); the
        // expected rating is the rounding of 5.00 % through it.
        Vector const vectors[] =
        {
            {   1,    3, 1.85714138f,  0.57142401f },  // 5.00 / 1.857141 = 2.6923 -> 3
            {  50,   57, 0.08843537f,  0.04081631f },  // 56.5385 -> 57
            {  80,  230, 0.02178365f,  0.01024008f },  // 229.5299 -> 230
            { 100,  992, 0.00503813f, -0.00217676f },  // 992.4321 -> 992
        };

        for (Vector const& v : vectors)
        {
            FfxivDerivedStats const stats = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, v.level));
            REQUIRE(stats.rate.critBp == 500);      // naked: critDelta == 0 (F9)
            float const chance = FfxivCritChancePercent(stats);
            REQUIRE(chance == 5.0f);

            // The store lookup, through the same composite ids the player path uses.
            float const multiplier = GetCombatRatingMultiplier(kCritTestClass, v.level, CR_CRIT_MELEE);
            CHECK(NearlyEqual(multiplier, v.multiplier, 1e-7f));
            CHECK(GetCombatRatingMultiplier(kCritTestClass, v.level, CR_CRIT_RANGED) == multiplier);
            CHECK(GetCombatRatingMultiplier(kCritTestClass, v.level, CR_CRIT_SPELL) == multiplier);

            uint32 const rating = FfxivRatingForPercent(chance, multiplier, 0.0f);
            CHECK(rating == v.rating);

            // The round trip the combat path performs: the field value times the
            // multiplier, plus the base term the applier did not remove (none here).
            float const applied = float(rating) * multiplier;
            CHECK(NearlyEqual(applied - chance, v.residualPercent, 1e-5f));

            // The integer rating field cannot do better than half a point.
            CHECK(std::fabs(applied - chance) <= 0.5f * multiplier + 1e-5f);

            // And the plan the applier executes agrees with the bare inversion,
            // with the agility term removed on every slot.
            FfxivCritPlan const plan = PlanFfxivCritProjection(stats, CoreTermsForClass(kCritTestClass, v.level));
            for (uint8 slot = 0; slot < FFXIV_CRIT_SLOT_COUNT; ++slot)
            {
                CHECK(plan.rating[slot] == v.rating);
                CHECK(plan.basePctMod[slot] == 0.0f);
            }
        }
    }

    SECTION("geared vectors use the engine's own chance, not 5.00 %")
    {
        struct Vector
        {
            uint8 level;
            int32 crit;
            int32 critBp;           // F9 for this vector
            uint32 rating;
            float residualPercent;
        };

        Vector const vectors[] =
        {
            {  80, 442, 590,  271,  0.00336933f },  // 5.90 / 0.02178365 = 270.86 -> 271
            { 100, 442, 510, 1012, -0.00141430f },  // 5.10 / 0.00503813 = 1012.28 -> 1012
        };

        for (Vector const& v : vectors)
        {
            FfxivStatInputs inputs = Inputs(FFXIV_NEUTRAL_JOB_ID, v.level);
            inputs.crit = v.crit;
            FfxivDerivedStats const stats = ComputeFfxivStats(inputs);
            REQUIRE(stats.rate.critBp == v.critBp);

            float const chance = FfxivCritChancePercent(stats);
            CHECK(NearlyEqual(chance, float(v.critBp) / 100.0f, 1e-6f));

            float const multiplier = GetCombatRatingMultiplier(kCritTestClass, v.level, CR_CRIT_MELEE);
            uint32 const rating = FfxivRatingForPercent(chance, multiplier, 0.0f);
            CHECK(rating == v.rating);
            CHECK(NearlyEqual(float(rating) * multiplier - chance, v.residualPercent, 1e-5f));
        }
    }

    SECTION("the 0.01 % goal is a level-dependent bound: measured, not assumed")
    {
        // Half a rating point is the best the integer field can do, and at L80 that
        // is 0.0109 % -- already above the 0.01 % the phase-1b brief asks for. The
        // residuals below are the ones the shipped rows actually give; the L1 and
        // L50 cases pin that 0.01 % is unreachable there, so nobody "fixes" it by
        // adding a fudge to the rating.
        FfxivDerivedStats const level1 = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 1));
        float const level1Multiplier = GetCombatRatingMultiplier(kCritTestClass, 1, CR_CRIT_MELEE);
        float const level1Applied = float(FfxivRatingForPercent(FfxivCritChancePercent(level1), level1Multiplier, 0.0f)) * level1Multiplier;
        CHECK(std::fabs(level1Applied - FfxivCritChancePercent(level1)) > 0.01f);   // 0.5714 % at L1
        CHECK(std::fabs(level1Applied - FfxivCritChancePercent(level1)) <= 0.5f * level1Multiplier + 1e-5f);

        FfxivDerivedStats const level50 = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 50));
        float const level50Multiplier = GetCombatRatingMultiplier(kCritTestClass, 50, CR_CRIT_MELEE);
        float const level50Applied = float(FfxivRatingForPercent(FfxivCritChancePercent(level50), level50Multiplier, 0.0f)) * level50Multiplier;
        CHECK(std::fabs(level50Applied - FfxivCritChancePercent(level50)) > 0.01f);  // 0.0408 % at L50

        FfxivDerivedStats const level100 = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 100));
        float const level100Multiplier = GetCombatRatingMultiplier(kCritTestClass, 100, CR_CRIT_MELEE);
        float const level100Applied = float(FfxivRatingForPercent(FfxivCritChancePercent(level100), level100Multiplier, 0.0f)) * level100Multiplier;
        CHECK(std::fabs(level100Applied - FfxivCritChancePercent(level100)) <= 0.01f);  // 0.0022 % at L100
    }

    SECTION("a measured base term is subtracted, not ignored")
    {
        // The applier feeds the weapon-skill term it measured on the player. A -0.2 %
        // term (an unarmed level-1 main hand) has to move the rating up, otherwise
        // the resulting PLAYER_CRIT_PERCENTAGE would land 0.2 % low.
        FfxivDerivedStats const stats = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 80));
        float const chance = FfxivCritChancePercent(stats);
        float const multiplier = GetCombatRatingMultiplier(kCritTestClass, 80, CR_CRIT_MELEE);
        REQUIRE(chance == 5.0f);

        uint32 const withoutTerm = FfxivRatingForPercent(chance, multiplier, 0.0f);
        uint32 const withTerm = FfxivRatingForPercent(chance, multiplier, -0.2f);
        CHECK(withoutTerm == 230);                          // 5.00 / 0.02178365
        CHECK(withTerm == 239);                             // 5.20 / 0.02178365 = 238.71
        CHECK(withTerm == withoutTerm + 9);

        // Forward check for both: rating * multiplier + term reproduces the chance
        // within half a rating point.
        CHECK(NearlyEqual(float(withoutTerm) * multiplier - chance, 0.0f, 0.5f * multiplier + 1e-5f));
        CHECK(NearlyEqual(float(withTerm) * multiplier - 0.2f - chance, 0.0f, 0.5f * multiplier + 1e-5f));
    }

    SECTION("the plan zeroes the agility term on all three slots")
    {
        // This is the exact value the applier writes into CRIT_PERCENTAGE,
        // OFFHAND_CRIT_PERCENTAGE and RANGED_CRIT_PERCENTAGE through
        // SetBaseModPctValue(): the core's GetMeleeCritFromAgility() term must not
        // survive the projection. (That the setter actually lands on a live Player is
        // not testable here -- Player needs a world -- and is covered by the gated
        // apply path instead.)
        FfxivDerivedStats const stats = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 80));
        FfxivCritCoreTerms const core = CoreTermsForClass(kCritTestClass, 80, -0.2f, -0.1f, 2.5f);
        FfxivCritPlan const plan = PlanFfxivCritProjection(stats, core);

        for (uint8 slot = 0; slot < FFXIV_CRIT_SLOT_COUNT; ++slot)
        {
            CHECK(plan.basePctMod[slot] == 0.0f);
            CHECK(plan.compensatedPercent[slot] == core.baseTermPercent[slot]);
        }

        // A base term above the target cannot be cancelled by a rating: the slot
        // floors at 0 instead of inventing a negative one.
        FfxivCritCoreTerms overTerm = CoreTermsForClass(kCritTestClass, 80);
        overTerm.baseTermPercent[FFXIV_CRIT_SLOT_SPELL] = 7.0f;   // > the 5.00 % chance
        CHECK(PlanFfxivCritProjection(stats, overTerm).rating[FFXIV_CRIT_SLOT_SPELL] == 0);
    }
}

TEST_CASE("FFXIV crit rating inversion guards", "[ffxiv]")
{
    LoadCritRatingStores();

    SECTION("an unusable multiplier yields no rating")
    {
        CHECK(FfxivRatingForPercent(5.0f, 0.0f, 0.0f) == 0);
        CHECK(FfxivRatingForPercent(5.0f, -1.0f, 0.0f) == 0);
        CHECK(FfxivRatingForPercent(5.0f, 0.0f, -100.0f) == 0);
        CHECK(FfxivRatingForPercent(5.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f) == 0);
        CHECK(FfxivRatingForPercent(5.0f, std::numeric_limits<float>::infinity(), 0.0f) == 0);
    }

    SECTION("the shipped class-10 zero scalar block really is 0")
    {
        // Not a synthetic case: gtOCTClassCombatRatingScalar row (10-1)*32+9..11 is
        // 0.0 in the shipped file, so GetRatingBonusValue() is 0 for every rating and
        // the inversion must report "cannot express it" rather than divide by it.
        CHECK(GetCombatRatingMultiplier(kCritZeroScalarClass, 80, CR_CRIT_MELEE) == 0.0f);
        CHECK(GetCombatRatingMultiplier(kCritZeroScalarClass, 80, CR_CRIT_RANGED) == 0.0f);
        CHECK(GetCombatRatingMultiplier(kCritZeroScalarClass, 80, CR_CRIT_SPELL) == 0.0f);

        FfxivDerivedStats const stats = ComputeFfxivStats(Inputs(FFXIV_NEUTRAL_JOB_ID, 80));
        FfxivCritPlan const plan = PlanFfxivCritProjection(stats, CoreTermsForClass(kCritZeroScalarClass, 80));
        for (uint8 slot = 0; slot < FFXIV_CRIT_SLOT_COUNT; ++slot)
            CHECK(plan.rating[slot] == 0);
    }

    SECTION("a missing DBC row falls back to 1.0 %/point, like the player path")
    {
        // GetCombatRatingMultiplier returns 1.0f when either lookup misses, exactly
        // like Player::GetRatingMultiplier. The rows loaded here cover CR_CRIT_* for
        // class 13, so CR_HIT_TAKEN_MELEE (no row) and class 14 (no scalar row) both
        // take the fallback.
        CHECK(GetCombatRatingMultiplier(kCritTestClass, 1, CR_HIT_TAKEN_MELEE) == 1.0f);
        CHECK(GetCombatRatingMultiplier(14, 80, CR_CRIT_MELEE) == 1.0f);

        // A level past the table is clamped to GT_MAX_LEVEL, not rejected: the same
        // row as level 100 (id 899), which is why the clamp is inside the function.
        CHECK(GetCombatRatingMultiplier(kCritTestClass, 150, CR_CRIT_MELEE) ==
              GetCombatRatingMultiplier(kCritTestClass, 100, CR_CRIT_MELEE));
    }

    SECTION("an absurd rating clamps instead of wrapping")
    {
        CHECK(FfxivRatingForPercent(100.0f, 1e-9f, 0.0f) == kFfxivMaxProjectedRating);
        CHECK(FfxivRatingForPercent(1e30f, 1.0f, 0.0f) == kFfxivMaxProjectedRating);
        // Just under the ceiling stays exact: (kMax - 1) * 1.0
        CHECK(FfxivRatingForPercent(float(kFfxivMaxProjectedRating - 1), 1.0f, 0.0f) == kFfxivMaxProjectedRating - 1);
    }
}

TEST_CASE("FFXIV crit plan is a pure constexpr decision", "[ffxiv]")
{
    // The planner cannot touch a Unit field: it is evaluated in a constant
    // expression. The only code that writes a rating is ApplyFfxivStatsToPlayer's
    // `if constexpr (kFfxivStatProjectionEnabled)` body in PlayerFFXIVStats.cpp,
    // which is discarded in the shipping build (no relocation names any of those
    // setters in the gate-off object).
    constexpr FfxivCritCoreTerms core{};
    constexpr FfxivCritPlan plan = PlanFfxivCritProjection(FfxivDerivedStats{}, core);
    static_assert(plan.rating[FFXIV_CRIT_SLOT_MELEE] == 0);
    static_assert(plan.basePctMod[FFXIV_CRIT_SLOT_SPELL] == 0.0f);

    CHECK_FALSE(kFfxivStatProjectionEnabled);
}
