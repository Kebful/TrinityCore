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

#include "FFXIVStatEngine.h"
#include "FFXIVStatTables.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace FFXIV::Stats
{
namespace
{
// C++ integer division already truncates toward zero; this name makes the
// intent explicit at every call site.
int32 TruncDiv(int32 value, int32 divisor)
{
    return value / divisor;
}

// floor(value / divisor) for divisor > 0 (the model's fl()).
int32 FloorDiv(int32 value, int32 divisor)
{
    int32 quotient = value / divisor;
    if ((value % divisor != 0) && (value < 0))
        --quotient;
    return quotient;
}

std::string SignedPercent(int32 bp)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%s%d.%02d%%", bp < 0 ? "-" : "",
                  std::abs(bp) / 100, std::abs(bp) % 100);
    return buffer;
}

std::string SignedMultiplier(int32 bp)
{
    // bp is 10000 == 1.0, so 14090 -> "1.409x".
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%s%d.%03dx", bp < 0 ? "-" : "",
                  std::abs(bp) / 10000, (std::abs(bp) % 10000) / 10);
    return buffer;
}

std::string ProvenanceNames(uint32 provenance)
{
    std::string names;
    auto add = [&names](char const* name)
    {
        if (!names.empty())
            names += ' ';
        names += name;
    };
    if (provenance & FFXIV_PROV_EQUIPMENT_PARAMS_ZERO)
        add("equipmentParams=0");
    if (provenance & FFXIV_PROV_BASE_MAIN_MODEL)
        add("baseMainModel");
    if (provenance & FFXIV_PROV_LEVEL_INTERPOLATED)
        add("levelInterpolated");
    if (names.empty())
        names = "none";
    return names;
}
} // namespace

FfxivDerivedStats ComputeFfxivStats(FfxivStatInputs const& inputs)
{
    FfxivLevelRow const& levelRow = GetFfxivLevelRow(inputs.level);
    FfxivJobRow const& job = GetFfxivJobRow(inputs.jobId);
    FfxivTribeRow const& tribe = GetFfxivTribeRow(inputs.tribeId);
    uint8 const effectiveLevel = inputs.effectiveLevel ? inputs.effectiveLevel : inputs.level;

    int32 const S = levelRow.baseSubStat;
    int32 const D = levelRow.levelDiv;
    int32 const M = levelRow.mainStat;
    int32 const haste = std::max(0, inputs.haste + inputs.equipmentParams[FFXIV_PARAM_HASTE]);

    FfxivDerivedStats stats;
    stats.rulesetVersion = GetFfxivTableManifest();
    stats.jobId = inputs.jobId;
    stats.level = inputs.level;
    stats.effectiveLevel = effectiveLevel;

    bool equipmentParamsUsed = false;
    for (int32 value : inputs.equipmentParams)
        if (value != 0)
        {
            equipmentParamsUsed = true;
            break;
        }

    // 1. F2: base(stat) = trunc(levelMain * jobPct / 100) + tribeBonus
    auto composeBase = [M](uint16 jobPct, int8 tribeBonus)
    {
        return TruncDiv(M * int32(jobPct), 100) + int32(tribeBonus);
    };
    stats.base[0] = composeBase(job.strPct, tribe.str);
    stats.base[1] = composeBase(job.dexPct, tribe.dex);
    stats.base[2] = composeBase(job.vitPct, tribe.vit);
    stats.base[3] = composeBase(job.intPct, tribe.int_);
    stats.base[4] = composeBase(job.mndPct, tribe.mnd);
    stats.base[5] = composeBase(job.piePct, tribe.pie);

    // 2. Parameters: the six base attributes, then the secondaries. A naked
    // character has no secondary contribution, so it sits at the level base S
    // (U4, INFERRED: retail floors secondaries at S).
    stats.param[FFXIV_PARAM_STRENGTH] = stats.base[0] + inputs.equipmentParams[FFXIV_PARAM_STRENGTH];
    stats.param[FFXIV_PARAM_DEXTERITY] = stats.base[1] + inputs.equipmentParams[FFXIV_PARAM_DEXTERITY];
    stats.param[FFXIV_PARAM_VITALITY] = stats.base[2] + inputs.equipmentParams[FFXIV_PARAM_VITALITY];
    stats.param[FFXIV_PARAM_INTELLIGENCE] = stats.base[3] + inputs.equipmentParams[FFXIV_PARAM_INTELLIGENCE];
    stats.param[FFXIV_PARAM_MIND] = stats.base[4] + inputs.equipmentParams[FFXIV_PARAM_MIND];
    stats.param[FFXIV_PARAM_PIETY] = stats.base[5] + inputs.equipmentParams[FFXIV_PARAM_PIETY];

    auto composeSecondary = [S](int32 requested, int32 equipment)
    {
        return (requested >= 0 ? requested : S) + equipment;
    };
    stats.param[FFXIV_PARAM_CRITICAL_HIT] =
        composeSecondary(inputs.crit, inputs.equipmentParams[FFXIV_PARAM_CRITICAL_HIT]);
    stats.param[FFXIV_PARAM_DIRECT_HIT_RATE] =
        composeSecondary(inputs.directHit, inputs.equipmentParams[FFXIV_PARAM_DIRECT_HIT_RATE]);
    stats.param[FFXIV_PARAM_DETERMINATION] =
        composeSecondary(inputs.determination, inputs.equipmentParams[FFXIV_PARAM_DETERMINATION]);
    stats.param[FFXIV_PARAM_TENACITY] =
        composeSecondary(inputs.tenacity, inputs.equipmentParams[FFXIV_PARAM_TENACITY]);
    stats.param[FFXIV_PARAM_SKILL_SPEED] =
        composeSecondary(inputs.skillSpeed, inputs.equipmentParams[FFXIV_PARAM_SKILL_SPEED]);
    stats.param[FFXIV_PARAM_SPELL_SPEED] =
        composeSecondary(inputs.spellSpeed, inputs.equipmentParams[FFXIV_PARAM_SPELL_SPEED]);
    stats.param[FFXIV_PARAM_HASTE] = haste;

    // 3. Rates. Every *_Bp below is one true basis point (10000 == 1.0); each
    // formula's per-mille result is multiplied by ten at the assignment.
    int32 const critDelta = stats.param[FFXIV_PARAM_CRITICAL_HIT] - S;
    int32 const critTerm = FloorDiv(200 * critDelta, D);           // fl(200*(CRIT-S)/D)
    stats.rate.critBp = (critTerm + 50) * 10;                      // F9 chance
    stats.rate.critDamageBp = 14000 + critTerm * 10;               // F9 damage

    int32 const dhTerm = FloorDiv(550 * (stats.param[FFXIV_PARAM_DIRECT_HIT_RATE] - S), D);
    stats.rate.dhBp = dhTerm * 10;                                 // F10 chance

    // F8 and F4 use M, the level main-stat base (B §F20 pins M for DET: 442/440
    // is exactly 1.000x, which S=420 would not give). Below level 50 M is
    // model-interpolated and sits far below S (at L1, M=20 and S=56), so
    // low-level determination and MP tick inherit that gap; such rows carry
    // FFXIV_PROV_LEVEL_INTERPOLATED (U2/U3, unmeasured).
    int32 const detTerm = FloorDiv(140 * (stats.param[FFXIV_PARAM_DETERMINATION] - M), D);
    stats.rate.detBp = 10000 + detTerm * 10;                       // F8

    int32 const tncDelta = stats.param[FFXIV_PARAM_TENACITY] - S;
    stats.rate.tncOutBp = 10000 + FloorDiv(112 * tncDelta, D) * 10; // F11 outgoing
    stats.rate.tncInBp = 10000 - FloorDiv(200 * tncDelta, D) * 10;  // F11 incoming

    // 4. F12: q = trunc(130*(S-X)/D); F = max(0, 1000+q);
    //    T1 = trunc(B*F/1000); T2 = trunc(T1*Haste/100).
    auto speedFactor = [S, D](int32 value)
    {
        return std::max(0, 1000 + TruncDiv(130 * (S - value), D));
    };
    auto gcdFor = [haste](int32 factor)
    {
        uint32 const t1 = uint32(std::max(0, TruncDiv(int32(FFXIV_STANDARD_GCD_MS) * factor, 1000)));
        uint32 const t2 = uint32(std::max(0, TruncDiv(int32(t1) * haste, 100)));
        return std::max(FFXIV_MIN_GCD_MS, t2);
    };

    int32 const skillSpeedFactor = speedFactor(stats.param[FFXIV_PARAM_SKILL_SPEED]);
    int32 const spellSpeedFactor = speedFactor(stats.param[FFXIV_PARAM_SPELL_SPEED]);
    stats.rate.sksBp = skillSpeedFactor * 10;
    stats.rate.spsBp = spellSpeedFactor * 10;
    stats.gcdSksMs = gcdFor(skillSpeedFactor);
    stats.gcdSpsMs = gcdFor(spellSpeedFactor);

    // A Disciple of Magic job's GCD is governed by Spell Speed, everything else
    // by Skill Speed (job type is sheet data, ClassJob column 3; the rule itself
    // is INFERRED from the formula, and the snapshot exposes both values).
    bool const spellSpeedJob = job.jobType == FFXIV_JOB_TYPE_DOM;
    int32 const jobSpeedFactor = spellSpeedJob ? spellSpeedFactor : skillSpeedFactor;
    stats.gcdMs = uint32(spellSpeedJob ? stats.gcdSpsMs : stats.gcdSksMs);
    stats.castSpeedBp = jobSpeedFactor * 10;

    // 5. F3 HP: fl(levelHP * jobHpPct/100) + fl((VIT - M) * hpScalar(role))
    int32 const hpScalarTenths = job.role == FFXIV_JOB_ROLE_TANK
        ? levelRow.hpScalarTankTenths : levelRow.hpScalarOtherTenths;
    int32 const hpBase = TruncDiv(levelRow.baseHp * int32(job.hpPct), 100);
    int32 const hpFromVit = FloorDiv((stats.param[FFXIV_PARAM_VITALITY] - M) * hpScalarTenths, 10);
    stats.hpMax = uint32(std::max(0, hpBase + hpFromVit));

    // 6. F4: maximum MP is always 10000; the tick is the model's only PIE use.
    stats.mpMax = FFXIV_FIXED_MAX_MP;
    stats.mpTick = 200 + FloorDiv(150 * (stats.param[FFXIV_PARAM_PIETY] - M), D);

    stats.provenance = FFXIV_PROV_BASE_MAIN_MODEL;
    if (!equipmentParamsUsed)
        stats.provenance |= FFXIV_PROV_EQUIPMENT_PARAMS_ZERO;
    if (levelRow.provenance & FFXIV_PROV_LEVEL_INTERPOLATED)
        stats.provenance |= FFXIV_PROV_LEVEL_INTERPOLATED;

    return stats;
}

std::string FormatFfxivStatsBlock(FfxivDerivedStats const& stats)
{
    FfxivJobRow const& job = GetFfxivJobRow(stats.jobId);
    char buffer[256];
    std::string out;

    std::snprintf(buffer, sizeof(buffer), "FFXIV derived stats  ruleset %08X  schema %u\n",
                  stats.rulesetVersion, GetFfxivTableSchemaVersion());
    out += buffer;

    std::snprintf(buffer, sizeof(buffer), "job %2u %-3s  level %2u  effective %2u%s\n",
                  uint32(stats.jobId), job.abbreviation, uint32(stats.level), uint32(stats.effectiveLevel),
                  stats.effectiveLevel == stats.level ? "" : "  (synced)");
    out += buffer;

    std::snprintf(buffer, sizeof(buffer),
                  "  STR %4d  DEX %4d  VIT %4d  INT %4d  MND %4d  PIE %4d\n",
                  stats.base[0], stats.base[1], stats.base[2], stats.base[3], stats.base[4], stats.base[5]);
    out += buffer;

    std::snprintf(buffer, sizeof(buffer),
                  "  CRIT %4d  DH %4d  DET %4d  TNC %4d  SKS %4d  SPS %4d\n",
                  stats.param[FFXIV_PARAM_CRITICAL_HIT], stats.param[FFXIV_PARAM_DIRECT_HIT_RATE],
                  stats.param[FFXIV_PARAM_DETERMINATION], stats.param[FFXIV_PARAM_TENACITY],
                  stats.param[FFXIV_PARAM_SKILL_SPEED], stats.param[FFXIV_PARAM_SPELL_SPEED]);
    out += buffer;

    std::snprintf(buffer, sizeof(buffer), "  crit %7s  damage %7s  direct hit %7s\n",
                  SignedPercent(stats.rate.critBp).c_str(),
                  SignedMultiplier(stats.rate.critDamageBp).c_str(),
                  SignedPercent(stats.rate.dhBp).c_str());
    out += buffer;

    std::snprintf(buffer, sizeof(buffer), "  det  %7s  tenacity out %7s  in %7s\n",
                  SignedMultiplier(stats.rate.detBp).c_str(),
                  SignedMultiplier(stats.rate.tncOutBp).c_str(),
                  SignedMultiplier(stats.rate.tncInBp).c_str());
    out += buffer;

    std::snprintf(buffer, sizeof(buffer), "  speed sks %5d bp  sps %5d bp  gcd %4u ms (%s)\n",
                  stats.rate.sksBp, stats.rate.spsBp, stats.gcdMs, job.jobType == FFXIV_JOB_TYPE_DOM ? "spell" : "skill");
    out += buffer;

    std::snprintf(buffer, sizeof(buffer), "  hp %6u  mp %6u  mp/tick %4d\n",
                  stats.hpMax, stats.mpMax, stats.mpTick);
    out += buffer;

    std::snprintf(buffer, sizeof(buffer), "  provenance 0x%X [%s]\n",
                  stats.provenance, ProvenanceNames(stats.provenance).c_str());
    out += buffer;

    return out;
}
} // namespace FFXIV::Stats
