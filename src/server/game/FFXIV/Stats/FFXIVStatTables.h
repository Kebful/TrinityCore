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

#ifndef TRINITYCORE_FFXIV_STAT_TABLES_H
#define TRINITYCORE_FFXIV_STAT_TABLES_H

#include "Define.h"
#include <string>
#include <string_view>

/*
 * Versioned FFXIV stat tables for slice 1.
 *
 * The data itself lives in FFXIVStatTables.inc, which is GENERATED from the
 * shipped sheets by work/stat-research/gen_ffxiv_stat_tables.py -- never edit it
 * by hand. The generator bakes in a manifest hash; ValidateFfxivStatTables()
 * recomputes that hash from the compiled arrays on first use and aborts loudly
 * if the two disagree (the Stage-0 rule of the research plan: a sheet/manifest
 * mismatch must stop the server, not produce quiet wrong numbers).
 *
 * Sheet-sourced (VERIFIED against work/exd-all/exd):
 *   ParamGrow c5 -> baseSubStat, c6 -> levelDiv, for every level 1..100
 *   ClassJob  c9..c16 -> job percentages, c3 -> job type, c33 -> role
 *   ClassJob  c0 -> job name, matched to the module class list for the
 *             classId -> classJobId map (module class 13 Marauder -> ClassJob 3)
 *   Tribe     c2..c9  -> additive tribe modifiers
 *
 * Model-sourced (community model [X], NOT in any sheet):
 *   mainStat, baseHp and the two VIT->HP scalars, anchored at levels
 *   1/50/60/70/80/90/100. The server caps at 100; the L90/L100 rows are also the
 *   top of the playable range now, so no level in 1..100 is interpolation-only.
 *   Levels between anchors are linearly interpolated and mark
 *   FFXIV_PROV_LEVEL_INTERPOLATED. ParamGrow column 8 exists in the sheet and is
 *   deliberately unused (B §F1: it is not the model's base HP).
 */

namespace FFXIV::Stats
{
constexpr uint8 FFXIV_MAX_LEVEL = 100;       // server cap (worldserver.conf MaxPlayerLevel)
constexpr uint8 FFXIV_LEVEL_TABLE_MAX = 100; // model level table (anchors up to L100)
constexpr uint8 FFXIV_LEVEL_COUNT = 100;
constexpr uint8 FFXIV_JOB_COUNT = 44;   // ClassJob rows 0..43
constexpr uint8 FFXIV_TRIBE_COUNT = 17; // Tribe rows 0..16

// Job type, ClassJob column 3.
enum FfxivJobType : uint8
{
    FFXIV_JOB_TYPE_NONE = 0,
    FFXIV_JOB_TYPE_DOW  = 30,
    FFXIV_JOB_TYPE_DOM  = 31,
    FFXIV_JOB_TYPE_DOL  = 32,
    FFXIV_JOB_TYPE_DOH  = 33,
};

// Role, ClassJob column 33.
enum FfxivJobRole : uint8
{
    FFXIV_JOB_ROLE_NONE    = 0,
    FFXIV_JOB_ROLE_TANK    = 1,
    FFXIV_JOB_ROLE_MELEE   = 2,
    FFXIV_JOB_ROLE_RANGED  = 3,
    FFXIV_JOB_ROLE_HEALER  = 4,
};

struct FfxivLevelRow
{
    uint8 level;
    int32 mainStat;             // model table, or interpolated (provenance)
    int32 baseSubStat;          // ParamGrow c5 (S)
    int32 levelDiv;             // ParamGrow c6 (D)
    int32 baseHp;               // model table, or interpolated
    int32 hpScalarTankTenths;   // model table x10 (F3 tank scalar)
    int32 hpScalarOtherTenths;  // model table x10 (F3 non-tank scalar)
    uint8 provenance;           // FFXIV_PROV_LEVEL_INTERPOLATED or 0
};

struct FfxivJobRow
{
    uint8 jobId;
    char const* abbreviation;
    uint8 jobType;              // FfxivJobType
    uint8 role;                 // FfxivJobRole
    uint16 hpPct;
    uint16 mpPct;
    uint16 strPct;
    uint16 vitPct;
    uint16 dexPct;
    uint16 intPct;
    uint16 mndPct;
    uint16 piePct;
};

struct FfxivTribeRow
{
    uint8 tribeId;
    int8 hp;                    // every currently shipped row is 0 (B §F21)
    int8 mp;                    // every currently shipped row is 0
    int8 str;
    int8 vit;
    int8 dex;
    int8 int_;
    int8 mnd;
    int8 pie;
};

/*
 * Module class -> FFXIV job.
 *
 * The server stores the module's own ChrClasses id in `characters.class`
 * (Player::GetClass()): FFXIV classes/jobs are rows 12..42, allocated by the data
 * build (release/modules/ffxivhaven/datascripts/shared/content.ts FfxivClassIDs).
 * That is NOT the ClassJob row id -- Marauder is class 13 but ClassJob 3 -- so the
 * generator resolves every module class by matching its display name to ClassJob
 * column 0 and emits the pair here. Nothing is guessed: a module class with no
 * ClassJob name match is emitted as classJobId 0 and the build fails on
 * kClassJobMapUnmatched.
 */
constexpr uint8 FFXIV_MODULE_CLASS_COUNT = 30; // module ChrClasses rows 12..42

struct FfxivClassJobMapRow
{
    uint8 classId;              // characters.class / ChrClasses id (12..42)
    uint8 classJobId;           // ClassJob row; 0 = no name match (must never ship)
    char const* name;           // module class display name the match used
};

// Accessors validate the compiled table once, then return the row. Out-of-range
// ids abort with a message that names the table. The level accessor accepts the
// whole model table (1..FFXIV_LEVEL_TABLE_MAX); gameplay callers must cap at
// FFXIV_MAX_LEVEL themselves.
FfxivLevelRow const& GetFfxivLevelRow(uint8 level);
FfxivJobRow const& GetFfxivJobRow(uint8 jobId);
FfxivTribeRow const& GetFfxivTribeRow(uint8 tribeId);

// The generated module class map, in class-id order. Out-of-range iteration aborts.
FfxivClassJobMapRow const& GetFfxivClassJobMapRow(uint8 index);
uint8 GetFfxivClassJobMapCount();

// The map row for a class id, or nullptr when the class is not a module class.
FfxivClassJobMapRow const* FindFfxivClassJobMapRow(uint8 moduleClassId);

// ClassJob row for a class id from Player::GetClass(). Returns
// FFXIV_NEUTRAL_JOB_ID (0, the sheet's 100 % row) when the class is not one of the
// module classes -- stock WoW classes have no ClassJob row, so this must not abort.
uint8 GetFfxivClassJobId(uint8 moduleClassId);

uint32 GetFfxivTableManifest();             // FNV-1a/32 of the manifest payload
std::string_view GetFfxivTableManifestSha256();
uint32 GetFfxivTableSchemaVersion();
std::string_view GetFfxivTableSourceSheets();

// Recomputes the manifest from the compiled arrays and checks the structural
// invariants. Aborts (ABORT_MSG) on any mismatch. Idempotent; the accessors call
// it on first use, so a tampered or mis-generated .inc can never be read.
void ValidateFfxivStatTables();
} // namespace FFXIV::Stats

#endif // TRINITYCORE_FFXIV_STAT_TABLES_H
