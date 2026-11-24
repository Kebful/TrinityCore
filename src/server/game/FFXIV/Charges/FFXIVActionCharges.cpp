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
 * FFXIV action charges: the table, the per-player pools, the once-per-cast consume and the 0x7A64
 * snapshot. Values and level bands are the sheets' (tools/generate-ffxiv-action-charges.py); the
 * arithmetic here is ours and is proven by tests/game/FFXIVActionCharges.cpp.
 */

#include "FFXIVActionCharges.h"

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
#include "FFXIV/Combat/FFXIVActionProc.h"

#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>

namespace FFXIV::Charges
{
namespace
{
// Sorted by (actionId, minLevel): FindFfxivActionChargeRow is a bounded scan.
std::vector<FfxivActionChargeRow> g_rows;

struct FfxivChargePool
{
    uint8 charges = 0;
    uint32 anchorMs = 0; // when the recharge of the next charge started
};

// Only pools that are below full: an absent action IS "full" (see Refill).
std::unordered_map<ObjectGuid, std::map<uint32, FfxivChargePool>> g_state;

struct FfxivChargeSnapshot
{
    std::vector<uint8> payload;
    uint32 stampMs = 0;
};

// What each player was last sent, so the tick only talks when the drawn value changes.
std::unordered_map<ObjectGuid, FfxivChargeSnapshot> g_sent;

constexpr std::size_t FFXIV_CHARGE_SENT_LIMIT = 1024;
constexpr uint32 FFXIV_CHARGE_SENT_STALE_MS = 10 * 60 * 1000;

bool RowLess(FfxivActionChargeRow const& left, FfxivActionChargeRow const& right)
{
    if (left.actionId != right.actionId)
        return left.actionId < right.actionId;
    return left.minLevel < right.minLevel;
}

void AppendU32(std::vector<uint8>& out, uint32 value)
{
    out.push_back(uint8(value & 0xFF));
    out.push_back(uint8((value >> 8) & 0xFF));
    out.push_back(uint8((value >> 16) & 0xFF));
    out.push_back(uint8((value >> 24) & 0xFF));
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

// One charge per rechargeMs from the anchor, capped at the row's max. Erases the pool when it is
// full again: absence is "full", so a resting pool costs nothing.
uint8 Refill(ObjectGuid player, FfxivActionChargeRow const& row, uint32 nowMs)
{
    auto playerIt = g_state.find(player);
    if (playerIt == g_state.end())
        return row.maxCharges;
    auto poolIt = playerIt->second.find(row.actionId);
    if (poolIt == playerIt->second.end())
        return row.maxCharges;

    FfxivChargePool& pool = poolIt->second;
    // A level band that lowers the maximum clamps the pool down (a level-down or a band boundary).
    if (pool.charges > row.maxCharges)
        pool.charges = row.maxCharges;

    while (pool.charges < row.maxCharges && row.rechargeMs != 0 && nowMs - pool.anchorMs >= row.rechargeMs)
    {
        ++pool.charges;
        pool.anchorMs += row.rechargeMs;
    }

    uint8 const charges = pool.charges;
    if (charges >= row.maxCharges)
        playerIt->second.erase(poolIt);
    if (playerIt->second.empty())
        g_state.erase(playerIt);
    return charges;
}
} // namespace

// --------------------------------------------------------------------------
// table
// --------------------------------------------------------------------------

void SetFfxivActionChargeRows(std::vector<FfxivActionChargeRow> rows)
{
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](FfxivActionChargeRow const& row)
        {
            return row.actionId == 0 || row.maxCharges == 0 || row.rechargeMs == 0;
        }), rows.end());
    std::sort(rows.begin(), rows.end(), RowLess);
    rows.erase(std::unique(rows.begin(), rows.end(),
        [](FfxivActionChargeRow const& left, FfxivActionChargeRow const& right)
        {
            return left.actionId == right.actionId && left.minLevel == right.minLevel;
        }), rows.end());
    g_rows = std::move(rows);
}

void ClearFfxivActionChargeRows()
{
    g_rows.clear();
}

bool HasFfxivActionChargeRows()
{
    return !g_rows.empty();
}

void LoadFfxivActionChargesTable()
{
    uint32 const oldMSTime = getMSTime();

    ClearFfxivActionChargeRows();
    QueryResult result = WorldDatabase.Query(
        "SELECT actionId, minLevel, maxCharges, rechargeMs FROM ffxiv_action_charges");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 FFXIV action charges. DB table `ffxiv_action_charges` is "
                                      "missing or empty; every action keeps its stock cooldown.");
        return;
    }

    std::vector<FfxivActionChargeRow> rows;
    do
    {
        Field* fields = result->Fetch();
        FfxivActionChargeRow row;
        row.actionId = fields[0].GetUInt32();
        row.minLevel = fields[1].GetUInt8();
        row.maxCharges = fields[2].GetUInt8();
        row.rechargeMs = fields[3].GetUInt32();
        if (row.actionId == 0 || row.maxCharges == 0 || row.rechargeMs == 0)
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_action_charges` has action {} with maxCharges {} / "
                                    "rechargeMs {}, ignored.", row.actionId, uint32(row.maxCharges),
                         row.rechargeMs);
            continue;
        }
        rows.push_back(row);
    } while (result->NextRow());

    std::size_t loaded = rows.size();
    SetFfxivActionChargeRows(std::move(rows));
    if (g_rows.size() != loaded)
        TC_LOG_ERROR("sql.sql", "Table `ffxiv_action_charges`: {} duplicate or empty rows ignored.",
                     loaded - g_rows.size());

    std::size_t actions = 0;
    for (std::size_t i = 0; i < g_rows.size();)
    {
        uint32 const actionId = g_rows[i].actionId;
        ++actions;
        while (i < g_rows.size() && g_rows[i].actionId == actionId)
            ++i;
    }
    TC_LOG_INFO("server.loading", ">> Loaded {} FFXIV action charge rows ({} actions) in {} ms",
                g_rows.size(), actions, GetMSTimeDiffToNow(oldMSTime));
}

FfxivActionChargeRow const* FindFfxivActionChargeRow(uint32 actionId, uint8 level)
{
    if (actionId == 0 || g_rows.empty())
        return nullptr;
    FfxivActionChargeRow const* best = nullptr;
    auto it = std::lower_bound(g_rows.begin(), g_rows.end(), FfxivActionChargeRow{ actionId, 0, 0, 0 },
        [](FfxivActionChargeRow const& row, FfxivActionChargeRow const& key)
        {
            return RowLess(row, key);
        });
    for (; it != g_rows.end() && it->actionId == actionId; ++it)
    {
        if (it->minLevel > level)
            break;
        best = &*it;
    }
    return best;
}

uint8 GetFfxivActionChargeLevel(uint32 actionId)
{
    auto const it = std::lower_bound(g_rows.begin(), g_rows.end(), actionId,
        [](FfxivActionChargeRow const& row, uint32 id) { return row.actionId < id; });
    return it != g_rows.end() && it->actionId == actionId ? it->minLevel : 0;
}

// --------------------------------------------------------------------------
// state
// --------------------------------------------------------------------------

uint8 AvailableFfxivCharges(ObjectGuid player, uint32 actionId, uint8 level, uint32 nowMs)
{
    FfxivActionChargeRow const* row = FindFfxivActionChargeRow(actionId, level);
    if (!row)
        return 0; // no pool for this action at this level
    return Refill(player, *row, nowMs);
}

uint32 GetFfxivChargeRemainingMs(ObjectGuid player, uint32 actionId, uint8 level, uint32 nowMs)
{
    FfxivActionChargeRow const* row = FindFfxivActionChargeRow(actionId, level);
    if (!row)
        return 0;
    auto const playerIt = g_state.find(player);
    if (playerIt != g_state.end())
    {
        auto const poolIt = playerIt->second.find(actionId);
        if (poolIt != playerIt->second.end() && poolIt->second.charges < row->maxCharges)
        {
            uint32 const elapsed = nowMs - poolIt->second.anchorMs;
            return elapsed >= row->rechargeMs ? 0 : row->rechargeMs - elapsed;
        }
    }
    return 0; // full
}

uint8 AvailableFfxivCharges(Player const& player, uint32 actionId, uint32 nowMs)
{
    auto const& stats = player.GetFFXIVStats();
    return AvailableFfxivCharges(player.GetGUID(), actionId, stats.level, nowMs);
}

bool HasFfxivChargeAvailable(ObjectGuid player, uint32 actionId, uint8 level, uint32 nowMs)
{
    if (!FindFfxivActionChargeRow(actionId, level))
        return true; // not a charged action (or no data): the stock path decides
    return AvailableFfxivCharges(player, actionId, level, nowMs) > 0;
}

bool HasFfxivChargeAvailable(Player const& player, uint32 actionId)
{
    auto const& stats = player.GetFFXIVStats();
    return HasFfxivChargeAvailable(player.GetGUID(), actionId, stats.level, GameTime::GetGameTimeMS());
}

bool ConsumeFfxivCharge(ObjectGuid player, uint32 actionId, uint8 level, uint32 nowMs)
{
    FfxivActionChargeRow const* row = FindFfxivActionChargeRow(actionId, level);
    if (!row)
        return false;
    if (AvailableFfxivCharges(player, actionId, level, nowMs) == 0)
        return false;

    // A full pool had no entry (absence is full), so the first consumption starts the recharge now.
    // A pool already recharging keeps its anchor: the next charge arrives on its original schedule.
    auto& playerState = g_state[player];
    auto it = playerState.find(actionId);
    if (it == playerState.end())
        it = playerState.emplace(actionId, FfxivChargePool{ row->maxCharges, nowMs }).first;
    --it->second.charges;
    return true;
}

bool ConsumeFfxivCharge(Player& player, uint32 actionId)
{
    uint8 const level = player.GetFFXIVStats().level;
    if (!ConsumeFfxivCharge(player.GetGUID(), actionId, level, GameTime::GetGameTimeMS()))
        return false;
    SendFfxivActionCharges(player);
    return true;
}

void ClearFfxivActionChargeState()
{
    g_state.clear();
    g_sent.clear();
}

// --------------------------------------------------------------------------
// hooks
// --------------------------------------------------------------------------

void OnFfxivActionResolved(Player& caster, SpellInfo const* spellInfo)
{
    if (!spellInfo || g_rows.empty())
        return;

    uint32 actionId = Combat::ResolveFfxivActionIdFromSpell(spellInfo->Id);
    if (actionId == 0)
        return;

    // A status-gated replacement resolves first, exactly as the gauge and damage hooks do: the pool
    // that pays is the one of the action the cast actually became.
    auto const& stats = caster.GetFFXIVStats();
    actionId = Combat::ResolveFfxivActionProcId(caster, actionId, stats.jobId, stats.level);

    if (!ConsumeFfxivCharge(caster, actionId))
        return;

    TC_LOG_DEBUG("spells", "FFXIV action {}: charge consumed, {} left (spell {})", actionId,
                 uint32(AvailableFfxivCharges(caster, actionId, GameTime::GetGameTimeMS())),
                 spellInfo->Id);
}

void UpdateFfxivActionCharges(Player& player)
{
    if (g_rows.empty())
        return;

    ObjectGuid const guid = player.GetGUID();
    auto const stateIt = g_state.find(guid);
    auto const sentIt = g_sent.find(guid);
    if (stateIt == g_state.end() && sentIt == g_sent.end())
        return; // nothing recharging and nothing ever sent: nothing to say

    // The snapshot rebuild is also the refill: CollectFfxivActionCharges asks
    // AvailableFfxivCharges for every pool, which advances the ones whose recharge has elapsed.
    uint32 const nowMs = GameTime::GetGameTimeMS();
    std::vector<uint8> const payload = BuildFfxivActionChargesPayload(CollectFfxivActionCharges(player, nowMs));
    if (sentIt != g_sent.end() && sentIt->second.payload == payload)
        return;

    SendFfxivActionCharges(player);
}

void SendFfxivActionCharges(Player& player)
{
    if (g_rows.empty())
        return; // no charge data: no traffic

    uint32 const nowMs = GameTime::GetGameTimeMS();
    std::vector<uint8> const payload = BuildFfxivActionChargesPayload(CollectFfxivActionCharges(player, nowMs));
    SendCustomPayload(player, FFXIV_ACTION_CHARGES_OPCODE, payload);
    g_sent[player.GetGUID()] = FfxivChargeSnapshot{ payload, nowMs };

    // A snapshot per player is small, but the map must not grow with every character ever seen: once
    // it is large, drop entries nobody refreshed for ten minutes (a re-entering player is re-sent at
    // enter-world anyway).
    if (g_sent.size() > FFXIV_CHARGE_SENT_LIMIT)
    {
        for (auto it = g_sent.begin(); it != g_sent.end();)
        {
            if (it->first != player.GetGUID() && nowMs - it->second.stampMs > FFXIV_CHARGE_SENT_STALE_MS)
                it = g_sent.erase(it);
            else
                ++it;
        }
    }
}

// --------------------------------------------------------------------------
// wire
// --------------------------------------------------------------------------

std::vector<uint8> BuildFfxivActionChargesPayload(std::vector<FfxivActionChargeEntry> entries)
{
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](FfxivActionChargeEntry const& entry)
        {
            return entry.spellId == 0 || entry.max == 0 || entry.current > entry.max;
        }), entries.end());
    std::sort(entries.begin(), entries.end(),
        [](FfxivActionChargeEntry const& left, FfxivActionChargeEntry const& right)
        {
            return left.spellId < right.spellId;
        });
    entries.erase(std::unique(entries.begin(), entries.end(),
        [](FfxivActionChargeEntry const& left, FfxivActionChargeEntry const& right)
        {
            return left.spellId == right.spellId;
        }), entries.end());
    if (entries.size() > FFXIV_ACTION_CHARGES_MAX_ENTRIES)
        entries.resize(FFXIV_ACTION_CHARGES_MAX_ENTRIES);

    std::vector<uint8> out;
    out.reserve(1 + entries.size() * FFXIV_ACTION_CHARGES_ENTRY_BYTES);
    out.push_back(uint8(entries.size()));
    for (FfxivActionChargeEntry const& entry : entries)
    {
        AppendU32(out, entry.spellId);
        out.push_back(entry.current);
        out.push_back(entry.max);
        AppendU32(out, entry.remainingMs);
        AppendU32(out, entry.rechargeMs);
    }
    out.push_back(FFXIV_ACTION_CHARGES_SCHEMA);
    return out;
}

std::vector<FfxivActionChargeEntry> CollectFfxivActionCharges(Player& player, uint32 nowMs)
{
    std::vector<FfxivActionChargeEntry> entries;
    if (g_rows.empty())
        return entries;

    auto const& stats = player.GetFFXIVStats();
    for (std::size_t i = 0; i < g_rows.size();)
    {
        uint32 const actionId = g_rows[i].actionId;
        std::size_t at = i;
        while (at < g_rows.size() && g_rows[at].actionId == actionId)
            ++at;

        FfxivActionChargeRow const* row = FindFfxivActionChargeRow(actionId, stats.level);
        if (row && stats.level >= GetFfxivActionChargeLevel(actionId))
        {
            uint32 const spellId = Combat::ResolveFfxivSpellFromActionId(actionId);
            // Only what this character actually has: a charged action of another job has no bar slot
            // and no tooltip here, and a spell the player has not learned cannot be used.
            if (spellId != 0 && player.HasSpell(spellId))
            {
                FfxivActionChargeEntry entry;
                entry.spellId = spellId;
                entry.current = AvailableFfxivCharges(player.GetGUID(), actionId, stats.level, nowMs);
                entry.max = row->maxCharges;
                entry.rechargeMs = row->rechargeMs;
                entry.remainingMs = GetFfxivChargeRemainingMs(player.GetGUID(), actionId, stats.level, nowMs);
                // Whole seconds: the tooltip and the bar print seconds, and the client decays the
                // value locally, so a stable recharge must not re-send every tick.
                entry.remainingMs -= entry.remainingMs % 1000;
                entries.push_back(entry);
            }
        }
        i = at;
    }
    return entries;
}
} // namespace FFXIV::Charges
