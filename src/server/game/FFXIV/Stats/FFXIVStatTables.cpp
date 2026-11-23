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

#include "FFXIVStatTables.h"
#include "FFXIVStatTypes.h"
#include "Errors.h"
#include <string>
#include <type_traits>

#include "FFXIVStatTables.inc"

namespace FFXIV::Stats
{
namespace
{
// Build-time checks on the generated module class map. These are the "write every
// pair down" audit: the generator lists one pair per module class, resolves it by
// ClassJob name, and a duplicate module class id or an unmatched name stops the
// build instead of shipping a guessed job id.
constexpr bool ClassJobMapHasUniqueClassIds()
{
    for (size_t i = 0; i < std::size(Tables::kClassJobMap); ++i)
        for (size_t j = i + 1; j < std::size(Tables::kClassJobMap); ++j)
            if (Tables::kClassJobMap[i].classId == Tables::kClassJobMap[j].classId)
                return false;
    return true;
}
static_assert(std::size(Tables::kClassJobMap) == FFXIV_MODULE_CLASS_COUNT,
              "FFXIV class map: the generated table does not list every module class");
static_assert(ClassJobMapHasUniqueClassIds(),
              "FFXIV class map: a module class id appears more than once");
static_assert(Tables::kClassJobMapUnmatched == 0,
              "FFXIV class map: a module class has no ClassJob name match; "
              "fix MODULE_CLASSES in gen_ffxiv_stat_tables.py instead of guessing an id");

uint32 Fnv1a32(std::string_view text)
{
    uint32 hash = 2166136261u;
    for (unsigned char byte : text)
    {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

template <typename... Ts>
void AppendManifestRow(std::string& out, Ts... values)
{
    bool first = true;
    auto append = [&out, &first](auto value)
    {
        if (!first)
            out += '|';
        first = false;
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, char const*> || std::is_same_v<T, std::string_view>)
            out += value;
        else if constexpr (std::is_unsigned_v<T>)
            out += std::to_string(static_cast<uint64>(value));
        else
            out += std::to_string(static_cast<int64>(value));
    };
    (append(values), ...);
    out += '\n';
}

// Must reproduce work/stat-research/gen_ffxiv_stat_tables.py::manifest_payload
// byte for byte. The abbreviation is deliberately not hashed (display only).
std::string BuildManifestPayload()
{
    std::string payload;
    payload += "schema=" + std::to_string(Tables::kSchemaVersion) + "\n";
    payload += "source=ParamGrow.c5,ParamGrow.c6;ClassJob.c9-16,ClassJob.c3,ClassJob.c33;Tribe.c2-9;model.levels\n";
    for (FfxivLevelRow const& row : Tables::kLevelTable)
        AppendManifestRow(payload, row.level, row.mainStat, row.baseSubStat, row.levelDiv, row.baseHp,
                          row.hpScalarTankTenths, row.hpScalarOtherTenths, row.provenance);
    for (FfxivJobRow const& row : Tables::kJobTable)
        AppendManifestRow(payload, row.jobId, row.jobType, row.role, row.hpPct, row.mpPct, row.strPct,
                          row.vitPct, row.dexPct, row.intPct, row.mndPct, row.piePct);
    for (FfxivTribeRow const& row : Tables::kTribeTable)
        AppendManifestRow(payload, row.tribeId, row.hp, row.mp, row.str, row.vit, row.dex, row.int_,
                          row.mnd, row.pie);
    return payload;
}

struct FfxivLevelAnchor
{
    uint8 level;
    int32 mainStat;
    int32 baseSubStat;
    int32 levelDiv;
    int32 baseHp;
    int32 hpScalarTankTenths;
    int32 hpScalarOtherTenths;
};

// Published 1-100 level table (Akhmorning, data/akhmorning-levelmods-20261123.json)
// + ParamGrow S/D [S]. These are the values the generator pins; if a regenerated
// table disagrees, this aborts at load instead of silently drifting (I §3.4.2 / Stage 0).
constexpr FfxivLevelAnchor kLevelAnchors[] =
{
    {   1,  20,  56,   56,   86,   0,   0 },  // published: MAIN 20, HP 86 (was 100, model)
    {  50, 202, 341,  341, 1700, 155, 112 },
    {  60, 218, 354,  600, 1500, 175, 129 },
    {  70, 292, 364,  900, 1700, 188, 140 },
    {  80, 340, 380, 1300, 2000, 266, 188 },
    {  90, 390, 400, 1900, 3000, 346, 243 },
    { 100, 440, 420, 2780, 4000, 430, 301 },
};

void ValidateOrAbort()
{
    static_assert(std::size(Tables::kLevelTable) == FFXIV_LEVEL_COUNT);
    static_assert(std::size(Tables::kJobTable) == FFXIV_JOB_COUNT);
    static_assert(std::size(Tables::kTribeTable) == FFXIV_TRIBE_COUNT);

    for (uint32 i = 0; i < FFXIV_LEVEL_COUNT; ++i)
    {
        FfxivLevelRow const& row = Tables::kLevelTable[i];
        if (row.level != i + 1)
            ABORT_MSG("FFXIV stat table: level row %u has level %u (table is not contiguous 1..%u)",
                      i, uint32(row.level), uint32(FFXIV_LEVEL_TABLE_MAX));
        if (row.baseSubStat <= 0 || row.levelDiv <= 0 || row.mainStat <= 0 || row.baseHp <= 0)
            ABORT_MSG("FFXIV stat table: level %u has a non-positive S/D/MAIN/HP (%d/%d/%d/%d)",
                      uint32(row.level), row.baseSubStat, row.levelDiv, row.mainStat, row.baseHp);
        if (row.provenance & ~uint32(FFXIV_PROV_LEVEL_INTERPOLATED))
            ABORT_MSG("FFXIV stat table: level %u carries unexpected level provenance 0x%X",
                      uint32(row.level), uint32(row.provenance));
    }

    for (FfxivLevelAnchor const& anchor : kLevelAnchors)
    {
        FfxivLevelRow const& row = Tables::kLevelTable[anchor.level - 1];
        if (row.mainStat != anchor.mainStat || row.baseSubStat != anchor.baseSubStat
            || row.levelDiv != anchor.levelDiv || row.baseHp != anchor.baseHp
            || row.hpScalarTankTenths != anchor.hpScalarTankTenths
            || row.hpScalarOtherTenths != anchor.hpScalarOtherTenths)
        {
            ABORT_MSG("FFXIV stat table: level %u is %d/%d/%d/%d/%d/%d but the anchor requires "
                      "%d/%d/%d/%d/%d/%d -- sheets or generator changed, regenerate deliberately",
                      uint32(anchor.level), row.mainStat, row.baseSubStat, row.levelDiv, row.baseHp,
                      row.hpScalarTankTenths, row.hpScalarOtherTenths, anchor.mainStat, anchor.baseSubStat,
                      anchor.levelDiv, anchor.baseHp, anchor.hpScalarTankTenths, anchor.hpScalarOtherTenths);
        }
    }

    for (uint32 i = 0; i < FFXIV_JOB_COUNT; ++i)
    {
        FfxivJobRow const& row = Tables::kJobTable[i];
        if (row.jobId != i)
            ABORT_MSG("FFXIV stat table: job row %u has jobId %u", i, uint32(row.jobId));
        if (row.jobType < FFXIV_JOB_TYPE_DOW || row.jobType > FFXIV_JOB_TYPE_DOH)
            ABORT_MSG("FFXIV stat table: job %u has unknown job type %u", uint32(row.jobId), uint32(row.jobType));
        if (row.role > FFXIV_JOB_ROLE_HEALER)
            ABORT_MSG("FFXIV stat table: job %u has unknown role %u", uint32(row.jobId), uint32(row.role));
        if (!row.abbreviation || !*row.abbreviation)
            ABORT_MSG("FFXIV stat table: job %u has no abbreviation", uint32(row.jobId));
        uint16 const percentages[] = { row.hpPct, row.mpPct, row.strPct, row.vitPct, row.dexPct, row.intPct,
                                       row.mndPct, row.piePct };
        for (uint32 field = 0; field < std::size(percentages); ++field)
            if (percentages[field] == 0 || percentages[field] > 1000)
                ABORT_MSG("FFXIV stat table: job %u percentage %u is out of range (%u)",
                          uint32(row.jobId), field, uint32(percentages[field]));
    }

    for (uint32 i = 0; i < FFXIV_TRIBE_COUNT; ++i)
    {
        FfxivTribeRow const& row = Tables::kTribeTable[i];
        if (row.tribeId != i)
            ABORT_MSG("FFXIV stat table: tribe row %u has tribeId %u", i, uint32(row.tribeId));
        if (row.hp != 0 || row.mp != 0 || row.pie != 0)
            ABORT_MSG("FFXIV stat table: tribe %u has a non-zero HP/MP/PIE modifier (%d/%d/%d); "
                      "B §F21 says the shipped sheet has none -- re-read the sheet before trusting it",
                      uint32(row.tribeId), int32(row.hp), int32(row.mp), int32(row.pie));
    }

    for (FfxivClassJobMapRow const& row : Tables::kClassJobMap)
    {
        if (row.classId == 0)
            ABORT_MSG("FFXIV class map: a row has module class id 0");
        if (row.classJobId >= FFXIV_JOB_COUNT)
            ABORT_MSG("FFXIV class map: module class %u maps to ClassJob %u, outside 0..%u",
                      uint32(row.classId), uint32(row.classJobId), uint32(FFXIV_JOB_COUNT - 1));
        if (!row.name || !*row.name)
            ABORT_MSG("FFXIV class map: module class %u has no display name", uint32(row.classId));
    }

    uint32 const computed = Fnv1a32(BuildManifestPayload());
    if (computed != Tables::kManifest)
        ABORT_MSG("FFXIV stat table manifest mismatch: compiled tables hash to 0x%08X, the generated "
                  "manifest says 0x%08X -- FFXIVStatTables.inc was edited by hand or generated from "
                  "other sheets; re-run work/stat-research/gen_ffxiv_stat_tables.py",
                  computed, Tables::kManifest);
}

void EnsureValidated()
{
    static bool const validated = [] { ValidateOrAbort(); return true; }();
    (void)validated;
}
} // namespace

void ValidateFfxivStatTables() { EnsureValidated(); }

FfxivLevelRow const& GetFfxivLevelRow(uint8 level)
{
    EnsureValidated();
    if (level < 1 || level > FFXIV_LEVEL_TABLE_MAX)
        ABORT_MSG("FFXIV stat table: level %u is outside 1..%u", uint32(level), uint32(FFXIV_LEVEL_TABLE_MAX));
    return Tables::kLevelTable[level - 1];
}

FfxivJobRow const& GetFfxivJobRow(uint8 jobId)
{
    EnsureValidated();
    if (jobId >= FFXIV_JOB_COUNT)
        ABORT_MSG("FFXIV stat table: jobId %u is outside 0..%u", uint32(jobId), uint32(FFXIV_JOB_COUNT - 1));
    return Tables::kJobTable[jobId];
}

FfxivTribeRow const& GetFfxivTribeRow(uint8 tribeId)
{
    EnsureValidated();
    if (tribeId >= FFXIV_TRIBE_COUNT)
        ABORT_MSG("FFXIV stat table: tribeId %u is outside 0..%u", uint32(tribeId), uint32(FFXIV_TRIBE_COUNT - 1));
    return Tables::kTribeTable[tribeId];
}

FfxivClassJobMapRow const& GetFfxivClassJobMapRow(uint8 index)
{
    EnsureValidated();
    if (index >= FFXIV_MODULE_CLASS_COUNT)
        ABORT_MSG("FFXIV class map: row %u is outside 0..%u", uint32(index), uint32(FFXIV_MODULE_CLASS_COUNT - 1));
    return Tables::kClassJobMap[index];
}

uint8 GetFfxivClassJobMapCount()
{
    EnsureValidated();
    return FFXIV_MODULE_CLASS_COUNT;
}

FfxivClassJobMapRow const* FindFfxivClassJobMapRow(uint8 moduleClassId)
{
    EnsureValidated();
    for (FfxivClassJobMapRow const& row : Tables::kClassJobMap)
        if (row.classId == moduleClassId)
            return &row;
    return nullptr;
}

uint8 GetFfxivClassJobId(uint8 moduleClassId)
{
    EnsureValidated();
    if (FfxivClassJobMapRow const* row = FindFfxivClassJobMapRow(moduleClassId))
        return row->classJobId;
    // Not a module class (a stock WoW class, or class 0 before the field is set).
    // ClassJob row 0 is the sheet's neutral 100 % row, so this is neutral, not a
    // guess -- and it must not abort, because any class can log in.
    return FFXIV_NEUTRAL_JOB_ID;
}

uint32 GetFfxivTableManifest()
{
    EnsureValidated();
    return Tables::kManifest;
}

std::string_view GetFfxivTableManifestSha256()
{
    EnsureValidated();
    return Tables::kManifestSha256;
}

uint32 GetFfxivTableSchemaVersion()
{
    EnsureValidated();
    return Tables::kSchemaVersion;
}

std::string_view GetFfxivTableSourceSheets()
{
    EnsureValidated();
    return Tables::kSourceSheets;
}
} // namespace FFXIV::Stats
