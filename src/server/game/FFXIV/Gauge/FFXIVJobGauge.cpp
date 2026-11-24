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
 * FFXIV job gauge: tables, per-player value, the once-per-cast hook and the 0x7A62 packet.
 *
 * Reference for the behaviour, read-only: Sapphire aab9cb23 src/world/Action/Job/Warrior.cpp
 * (Warrior::onAction: a combo action grants its gauge effect only when isCorrectCombo; the reference
 * stacks 2.x Wrath statuses instead of a number, so only that rule is taken, none of its values).
 * The deltas are the sheets' (tools/generate-ffxiv-job-gauges.py). The code here is ours.
 */

#include "FFXIVJobGauge.h"

#include "CustomPacketDefines.h"
#include "CustomPacketWrite.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "FFXIV/Combat/FFXIVActionDamage.h"
#include "FFXIV/Stats/FFXIVStatTables.h"
#include "FFXIV/Status/FFXIVStatus.h"
#include "TSEvent.h"
#include "TSEvents.h"

#include <algorithm>
#include <bitset>
#include <limits>
#include <unordered_map>

namespace FFXIV::Gauge
{
namespace
{
std::vector<FfxivJobGaugeRow> g_gauges;        // sorted by jobId
std::vector<FfxivActionGaugeRow> g_deltas;     // sorted by (actionId, jobId, comboOnly, minLevel)
std::vector<FfxivActionCostWaiverRow> g_waivers; // sorted by (actionId, statusId)

struct FfxivGaugeState
{
    uint8 jobId = 0;
    uint16 value = 0;
};

std::unordered_map<ObjectGuid, FfxivGaugeState> g_state;
std::bitset<256> g_scriptOwned; // jobs whose gauge a livescript owns (ClaimFfxivJobGauge); memory only

bool DeltaLess(FfxivActionGaugeRow const& left, FfxivActionGaugeRow const& right)
{
    if (left.actionId != right.actionId)
        return left.actionId < right.actionId;
    if (left.jobId != right.jobId)
        return left.jobId < right.jobId;
    if (left.comboOnly != right.comboOnly)
        return left.comboOnly < right.comboOnly;
    return left.minLevel < right.minLevel;
}

// The highest-minLevel row <= level for one (action, job, comboOnly), or nullptr.
FfxivActionGaugeRow const* FindDelta(uint32 actionId, uint8 jobId, uint8 level, bool comboOnly)
{
    FfxivActionGaugeRow const* best = nullptr;
    FfxivActionGaugeRow key;
    key.actionId = actionId;
    key.jobId = jobId;
    key.comboOnly = comboOnly;
    for (auto it = std::lower_bound(g_deltas.begin(), g_deltas.end(), key, DeltaLess);
         it != g_deltas.end() && it->actionId == actionId && it->jobId == jobId && it->comboOnly == comboOnly; ++it)
    {
        if (it->minLevel <= level)
            best = &*it;
    }
    return best;
}

void AppendU16(std::vector<uint8>& out, uint16 value)
{
    out.push_back(uint8(value & 0xFF));
    out.push_back(uint8((value >> 8) & 0xFF));
}

// The snapshot's sender is private to FFXIVStatsSnapshot.cpp; this is the same fragment-and-send.
void SendCustomPayload(Player& player, uint16 opcode, std::vector<uint8> const& payload)
{
    if (payload.empty() || payload.size() > std::numeric_limits<totalSize_t>::max())
        return;

    CustomPacketWrite writer(opcode, MAX_FRAGMENT_SIZE, 0);
    writer.WriteBytes(static_cast<totalSize_t>(payload.size()), reinterpret_cast<char const*>(payload.data()));
    for (CustomPacketChunk& chunk : writer.buildMessages())
    {
        WorldPacket packet(SERVER_TO_CLIENT_OPCODE, chunk.FullSize());
        packet.append(reinterpret_cast<uint8 const*>(chunk.Data()), chunk.FullSize());
        player.SendDirectMessage(&packet);
    }
    writer.Destroy();
}
} // namespace

// --------------------------------------------------------------------------
// tables
// --------------------------------------------------------------------------

void SetFfxivJobGaugeTables(std::vector<FfxivJobGaugeRow> gauges, std::vector<FfxivActionGaugeRow> deltas)
{
    std::sort(gauges.begin(), gauges.end(),
        [](FfxivJobGaugeRow const& left, FfxivJobGaugeRow const& right) { return left.jobId < right.jobId; });
    gauges.erase(std::unique(gauges.begin(), gauges.end(),
        [](FfxivJobGaugeRow const& left, FfxivJobGaugeRow const& right) { return left.jobId == right.jobId; }),
        gauges.end());
    g_gauges = std::move(gauges);

    // A delta for a gauge the job does not have would move a value nothing displays: drop it.
    deltas.erase(std::remove_if(deltas.begin(), deltas.end(), [](FfxivActionGaugeRow const& row)
        {
            FfxivJobGaugeRow const* gauge = GetFfxivJobGaugeRow(row.jobId);
            return !gauge || gauge->gaugeId != row.gaugeId || row.delta == 0;
        }), deltas.end());
    std::sort(deltas.begin(), deltas.end(), DeltaLess);
    g_deltas = std::move(deltas);
}

void ClearFfxivJobGaugeTables()
{
    g_gauges.clear();
    g_deltas.clear();
    g_waivers.clear();
}

void LoadFfxivJobGaugeTables()
{
    uint32 const oldMSTime = getMSTime();

    ClearFfxivJobGaugeTables();
    QueryResult result = WorldDatabase.Query("SELECT jobId, gaugeId, name, max FROM ffxiv_job_gauge");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 FFXIV job gauges. DB table `ffxiv_job_gauge` is missing or empty; "
                                      "no job has a gauge.");
        return;
    }

    std::vector<FfxivJobGaugeRow> gauges;
    do
    {
        Field* fields = result->Fetch();
        FfxivJobGaugeRow row;
        row.jobId = fields[0].GetUInt8();
        row.gaugeId = fields[1].GetUInt8();
        row.name = fields[2].GetString();
        row.max = fields[3].GetUInt16();
        if (row.gaugeId == 0 || row.max == 0)
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_job_gauge` has job {} with gaugeId {} / max {}, ignored.",
                         uint32(row.jobId), uint32(row.gaugeId), uint32(row.max));
            continue;
        }
        gauges.push_back(std::move(row));
    } while (result->NextRow());

    std::vector<FfxivActionGaugeRow> deltas;
    if (QueryResult deltaResult = WorldDatabase.Query(
            "SELECT actionId, jobId, gaugeId, minLevel, delta, comboOnly FROM ffxiv_action_gauge"))
    {
        do
        {
            Field* fields = deltaResult->Fetch();
            FfxivActionGaugeRow row;
            row.actionId = fields[0].GetUInt32();
            row.jobId = fields[1].GetUInt8();
            row.gaugeId = fields[2].GetUInt8();
            row.minLevel = fields[3].GetUInt8();
            row.delta = fields[4].GetInt16();
            row.comboOnly = fields[5].GetUInt8() != 0;
            deltas.push_back(row);
        } while (deltaResult->NextRow());
    }

    std::size_t const deltaRows = deltas.size();
    SetFfxivJobGaugeTables(std::move(gauges), std::move(deltas));
    if (g_deltas.size() != deltaRows)
        TC_LOG_ERROR("sql.sql", "Table `ffxiv_action_gauge`: {} rows name a gauge `ffxiv_job_gauge` does not carry "
                                "or a zero delta, ignored.", deltaRows - g_deltas.size());

    // ffxiv_action_cost_waiver: the statuses that make a spender free (Inner Release -> Fell Cleave).
    // Independent of the gauge rows above: a waiver for an action with no cost is harmless.
    std::vector<FfxivActionCostWaiverRow> waivers;
    if (QueryResult waiverResult = WorldDatabase.Query(
            "SELECT statusId, actionId FROM ffxiv_action_cost_waiver"))
    {
        do
        {
            Field* fields = waiverResult->Fetch();
            FfxivActionCostWaiverRow row;
            row.statusId = fields[0].GetUInt32();
            row.actionId = fields[1].GetUInt32();
            if (row.statusId == 0 || row.actionId == 0)
            {
                TC_LOG_ERROR("sql.sql", "Table `ffxiv_action_cost_waiver` has a zero status or action, ignored.");
                continue;
            }
            waivers.push_back(row);
        } while (waiverResult->NextRow());
    }
    std::size_t const waiverRows = waivers.size();
    SetFfxivActionCostWaiverRows(std::move(waivers));

    TC_LOG_INFO("server.loading", ">> Loaded {} FFXIV job gauges, {} action gauge deltas and {} cost waivers in {} ms",
                g_gauges.size(), g_deltas.size(), waiverRows, GetMSTimeDiffToNow(oldMSTime));
}

void ClearFfxivActionCostWaiverRows()
{
    g_waivers.clear();
}

void SetFfxivActionCostWaiverRows(std::vector<FfxivActionCostWaiverRow> waivers)
{
    waivers.erase(std::remove_if(waivers.begin(), waivers.end(), [](FfxivActionCostWaiverRow const& row)
        {
            return row.statusId == 0 || row.actionId == 0;
        }), waivers.end());
    std::sort(waivers.begin(), waivers.end(), [](FfxivActionCostWaiverRow const& left,
                                                 FfxivActionCostWaiverRow const& right)
        {
            if (left.actionId != right.actionId)
                return left.actionId < right.actionId;
            return left.statusId < right.statusId;
        });
    waivers.erase(std::unique(waivers.begin(), waivers.end(),
        [](FfxivActionCostWaiverRow const& left, FfxivActionCostWaiverRow const& right)
        {
            return left.actionId == right.actionId && left.statusId == right.statusId;
        }), waivers.end());
    g_waivers = std::move(waivers);
}

FfxivJobGaugeRow const* GetFfxivJobGaugeRow(uint8 jobId)
{
    auto const it = std::lower_bound(g_gauges.begin(), g_gauges.end(), jobId,
        [](FfxivJobGaugeRow const& row, uint8 id) { return row.jobId < id; });
    return it != g_gauges.end() && it->jobId == jobId ? &*it : nullptr;
}

int32 ResolveFfxivActionGaugeDelta(uint32 actionId, uint8 jobId, uint8 level, bool comboCorrect)
{
    if (actionId == 0 || !GetFfxivJobGaugeRow(jobId))
        return 0;

    int32 delta = 0;
    if (FfxivActionGaugeRow const* plain = FindDelta(actionId, jobId, level, false))
        delta += plain->delta;
    if (comboCorrect)
        if (FfxivActionGaugeRow const* combo = FindDelta(actionId, jobId, level, true))
            delta += combo->delta;
    return delta;
}

uint16 GetFfxivActionGaugeCost(uint32 actionId, uint8 jobId, uint8 level)
{
    if (actionId == 0 || !GetFfxivJobGaugeRow(jobId))
        return 0;
    // Only a plain negative row is a cost: a comboOnly row is the "Combo Bonus" gain, and a cast
    // never pays one of those.
    FfxivActionGaugeRow const* spend = FindDelta(actionId, jobId, level, false);
    if (!spend || spend->delta >= 0)
        return 0;
    return uint16(-int32(spend->delta));
}

std::vector<uint32> GetFfxivActionCostWaiverStatuses(uint32 actionId)
{
    std::vector<uint32> statuses;
    auto const it = std::lower_bound(g_waivers.begin(), g_waivers.end(), actionId,
        [](FfxivActionCostWaiverRow const& row, uint32 id) { return row.actionId < id; });
    for (auto row = it; row != g_waivers.end() && row->actionId == actionId; ++row)
        statuses.push_back(row->statusId);
    return statuses;
}

// --------------------------------------------------------------------------
// state
// --------------------------------------------------------------------------

uint16 GetFfxivGauge(ObjectGuid player, uint8 jobId)
{
    if (!GetFfxivJobGaugeRow(jobId))
        return 0;
    auto const it = g_state.find(player);
    return it != g_state.end() && it->second.jobId == jobId ? it->second.value : 0;
}

uint16 AddFfxivGauge(ObjectGuid player, uint8 jobId, int32 amount)
{
    FfxivJobGaugeRow const* gauge = GetFfxivJobGaugeRow(jobId);
    if (!gauge)
        return 0;

    int32 const next = std::clamp<int32>(int32(GetFfxivGauge(player, jobId)) + amount, 0, gauge->max);
    if (next == 0)
        g_state.erase(player); // 0 is the value of an absent entry; keep the map to non-empty gauges
    else
        g_state[player] = FfxivGaugeState{ jobId, uint16(next) };
    return uint16(next);
}

bool SpendFfxivGauge(ObjectGuid player, uint8 jobId, uint16 amount)
{
    if (!GetFfxivJobGaugeRow(jobId) || GetFfxivGauge(player, jobId) < amount)
        return false;
    AddFfxivGauge(player, jobId, -int32(amount));
    return true;
}

void ClearFfxivGaugeState()
{
    g_state.clear();
}

bool ApplyFfxivActionGauge(ObjectGuid player, uint8 jobId, uint8 level, uint32 actionId,
                           bool comboCorrect, bool costWaived)
{
    int32 delta = ResolveFfxivActionGaugeDelta(actionId, jobId, level, comboCorrect);
    // A status-waived cost cannot spend: the gain part of the row (if any) still applies. The clamp
    // below stays as a safety, but a cast that cannot pay is refused in Spell::CheckCast and never
    // reaches this function (CanAffordFfxivActionGauge).
    if (costWaived && delta < 0)
        delta = 0;
    if (delta == 0)
        return false;
    uint16 const before = GetFfxivGauge(player, jobId);
    return AddFfxivGauge(player, jobId, delta) != before;
}

bool CanAffordFfxivActionGauge(ObjectGuid player, uint32 actionId, uint8 jobId, uint8 level,
                               bool costWaived)
{
    uint16 const cost = GetFfxivActionGaugeCost(actionId, jobId, level);
    if (cost == 0 || costWaived)
        return true;
    return GetFfxivGauge(player, jobId) >= cost;
}

// --------------------------------------------------------------------------
// script ownership
// --------------------------------------------------------------------------

void SetFfxivJobGaugeScriptOwned(uint8 jobId, bool owned)
{
    g_scriptOwned.set(jobId, owned);
}

bool IsFfxivJobGaugeScriptOwned(uint8 jobId)
{
    return g_scriptOwned.test(jobId);
}

void ClearFfxivJobGaugeScriptOwners()
{
    g_scriptOwned.reset();
}

// --------------------------------------------------------------------------
// Player API
// --------------------------------------------------------------------------

uint16 GetFfxivGauge(Player const& player)
{
    return GetFfxivGauge(player.GetGUID(), player.GetFFXIVStats().jobId);
}

uint16 GetFfxivGaugeMax(Player const& player)
{
    FfxivJobGaugeRow const* gauge = GetFfxivJobGaugeRow(player.GetFFXIVStats().jobId);
    return gauge ? gauge->max : 0;
}

bool IsFfxivGaugeScriptOwned(Player const& player)
{
    return IsFfxivJobGaugeScriptOwned(player.GetFFXIVStats().jobId);
}

bool IsFfxivActionRageCostWaived(Player const& player, uint32 actionId)
{
    if (actionId == 0)
        return false;
    return IsFfxivGaugeScriptOwned(player) || IsFfxivActionGaugeCostWaived(player, actionId);
}

bool CanAffordFfxivActionGauge(Player const& player, uint32 actionId)
{
    auto const& stats = player.GetFFXIVStats();
    // A script-owned job's cost gate is the script's (Spell.OnCheckCast); the data cost is not read.
    if (IsFfxivJobGaugeScriptOwned(stats.jobId))
        return true;
    return CanAffordFfxivActionGauge(player.GetGUID(), actionId, stats.jobId, stats.level,
                                     IsFfxivActionGaugeCostWaived(player, actionId));
}

bool IsFfxivActionGaugeCostWaived(Player const& player, uint32 actionId)
{
    if (actionId == 0 || g_waivers.empty())
        return false;
    for (uint32 const statusId : GetFfxivActionCostWaiverStatuses(actionId))
    {
        FFXIV::Status::FfxivStatusRow const* status = FFXIV::Status::GetFfxivStatus(statusId);
        if (status && status->spellId != 0 && player.HasAura(status->spellId))
            return true;
    }
    return false;
}

uint16 AddFfxivGauge(Player& player, int32 amount)
{
    uint8 const jobId = player.GetFFXIVStats().jobId;
    uint16 const before = GetFfxivGauge(player.GetGUID(), jobId);
    uint16 const after = AddFfxivGauge(player.GetGUID(), jobId, amount);
    if (after != before)
        SendFfxivJobGauge(player);
    return after;
}

uint16 SetFfxivGauge(Player& player, int32 value)
{
    // Absolute set = relative add of the difference, so the clamp, the change test and the send are
    // the ones AddFfxivGauge already has.
    return AddFfxivGauge(player, value - int32(GetFfxivGauge(player)));
}

bool SpendFfxivGauge(Player& player, uint16 amount)
{
    if (!SpendFfxivGauge(player.GetGUID(), player.GetFFXIVStats().jobId, amount))
        return false;
    if (amount != 0)
        SendFfxivJobGauge(player);
    return true;
}

// --------------------------------------------------------------------------
// rage: the gauge's display
// --------------------------------------------------------------------------

bool IsFfxivRageOwner(Unit const& unit)
{
    // An empty gauge table means "no FFXIV gauge data loaded": WoW rage keeps its stock behaviour,
    // which is the same rule every other table in this slice follows.
    if (g_gauges.empty())
        return false;
    Player const* player = unit.ToPlayer();
    if (!player)
        return false;
    // Only the module's ChrClasses rows (12..30). A stock WoW class keeps its rage untouched.
    return FFXIV::Stats::FindFfxivClassJobMapRow(player->GetClass()) != nullptr;
}

bool SuppressesWowRage(Unit const& unit)
{
    return IsFfxivRageOwner(unit);
}

uint16 SyncFfxivRage(Player& player)
{
    if (!IsFfxivRageOwner(player))
        return 0;

    uint8 const jobId = player.GetFFXIVStats().jobId;
    FfxivJobGaugeRow const* gauge = GetFfxivJobGaugeRow(jobId);
    uint16 const value = gauge ? std::min<uint16>(GetFfxivGauge(player.GetGUID(), jobId), FFXIV_RAGE_MAX) : 0;

    // The client must not predict rage regen/decay: PLAYER_FIELD_BYTES2 byte 2 is a bitmask of the
    // power types whose prediction is ignored, so the displayed rage stays exactly what was sent.
    if (!player.HasByteFlag(PLAYER_FIELD_BYTES2,
                            PLAYER_FIELD_BYTES_2_OFFSET_IGNORE_POWER_REGEN_PREDICTION_MASK,
                            uint8(1u << AsUnderlyingType(POWER_RAGE))))
        player.SetByteFlag(PLAYER_FIELD_BYTES2,
                           PLAYER_FIELD_BYTES_2_OFFSET_IGNORE_POWER_REGEN_PREDICTION_MASK,
                           uint8(1u << AsUnderlyingType(POWER_RAGE)));
    player.SetMaxPower(POWER_RAGE, FFXIV_RAGE_MAX_FIELD);
    player.SetPower(POWER_RAGE, uint16(value * FFXIV_RAGE_TENTHS));

    if (player.GetPowerType() != POWER_RAGE)
        TC_LOG_ERROR("sql.sql", "FFXIV gauge: {} is a module class with power type {}; the Beast Gauge "
                                "mirror needs POWER_RAGE (ChrClasses.DisplayPower) to show on the bar.",
                     player.GetName(), uint32(player.GetPowerType()));
    return value;
}

// --------------------------------------------------------------------------
// the hook
// --------------------------------------------------------------------------

void OnFfxivActionResolved(Player& caster, SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return;

    uint32 actionId = Combat::ResolveFfxivActionIdFromSpell(spellInfo->Id);
    if (actionId == 0)
        return;

    auto const& stats = caster.GetFFXIVStats();
    // The damage hook ran for every target of this cast in this same tick, so a correct combo step
    // is recorded at this GameTime (FFXIVActionDamage.h WasFfxivCorrectComboStep).
    bool const comboCorrect =
        Combat::WasFfxivCorrectComboStep(caster.GetGUID(), actionId, GameTime::GetGameTimeMS());

    // The script's turn, for every mapped action whatever the data says: the event carries what the
    // core knows and nothing else; a script moves the value through TSPlayer::AddFfxivJobGauge.
    FIRE_ID(spellInfo->events.id, Spell, OnFfxivActionResolved, TSPlayer(&caster), TSSpellInfo(spellInfo),
            TSNumber<uint32>(actionId), comboCorrect);

    // A job a script owns has no data path: its rows, waivers and cost are not read.
    if (IsFfxivJobGaugeScriptOwned(stats.jobId))
        return;

    if (g_deltas.empty())
    {
        TC_LOG_ERROR("sql.sql", "[ffxiv-debug] gauge: no action-gauge rows loaded");
        return;
    }

    bool const waived = IsFfxivActionGaugeCostWaived(caster, actionId);
    uint16 const before = GetFfxivGauge(caster);
    bool const applied = ApplyFfxivActionGauge(caster.GetGUID(), stats.jobId, stats.level, actionId,
                                               comboCorrect, waived);
    TC_LOG_ERROR("sql.sql", "[ffxiv-debug] gauge action {} job {} level {} combo {} waived {} applied {} {}->{}",
                actionId, uint32(stats.jobId), uint32(stats.level), comboCorrect, waived, applied,
                before, GetFfxivGauge(caster));
    if (!applied)
        return;

    TC_LOG_DEBUG("spells", "FFXIV gauge {} action {} job {} level {} combo {} -> {}", caster.GetName(), actionId,
                 uint32(stats.jobId), uint32(stats.level), comboCorrect, GetFfxivGauge(caster));
    SendFfxivJobGauge(caster);
}

// --------------------------------------------------------------------------
// wire
// --------------------------------------------------------------------------

std::vector<uint8> BuildFfxivJobGaugePayload(FfxivJobGaugeRow const* gauge, uint16 value)
{
    std::vector<uint8> out;
    std::size_t const nameLength = gauge ? std::min(gauge->name.size(), FFXIV_JOB_GAUGE_MAX_NAME) : 0;
    out.reserve(FFXIV_JOB_GAUGE_FIXED_BYTES + nameLength);
    out.push_back(gauge ? gauge->jobId : 0);
    out.push_back(gauge ? gauge->gaugeId : 0);
    AppendU16(out, gauge ? std::min(value, gauge->max) : 0);
    AppendU16(out, gauge ? gauge->max : 0);
    out.push_back(uint8(nameLength));
    if (nameLength != 0)
        out.insert(out.end(), gauge->name.begin(), gauge->name.begin() + nameLength);
    out.push_back(FFXIV_JOB_GAUGE_SCHEMA);
    return out;
}

void SendFfxivJobGauge(Player& player)
{
    // The rage mirror is written first and unconditionally for a module class: a job with no gauge
    // sends no packet but must still show 0, not a leftover bar.
    SyncFfxivRage(player);

    if (g_gauges.empty())
        return; // no gauge data: no traffic, the client's readout stays empty

    uint8 const jobId = player.GetFFXIVStats().jobId;
    SendCustomPayload(player, FFXIV_JOB_GAUGE_OPCODE,
                      BuildFfxivJobGaugePayload(GetFfxivJobGaugeRow(jobId), GetFfxivGauge(player.GetGUID(), jobId)));
}
} // namespace FFXIV::Gauge
