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
 * FFXIV action cast -> potency -> published math -> damage/heal.
 *
 * Reference for the behaviour, read-only: Sapphire aab9cb23.
 *   src/world/Math/CalcStats.cpp      calcActionDamage: the stage order and the two divisors
 *   src/world/Action/Action.cpp       combo selection (isCorrectCombo -> ComboParent == last),
 *                                      combo advance/clear after a hit (weaponskills without
 *                                      ComboContinue only), potency vs comboPotency
 *   src/world/Action/Action.cpp       calcHealing (a TODO in the reference; not copied)
 *   data/actions/player.json          nextCombo / *Potency columns
 *   src/common/Common.h:61            MAX_COMBO_LENGTH = 12500 ms
 * The code here is ours; the reference is cited for the rules, never linked or included.
 *
 * The tables this file reads are the data slice's (FFXIVActionPotency's sibling). All are loaded
 * defensively: a missing or empty table leaves the vectors empty, FindFfxivActionPotencyRow returns
 * nullptr for every action, every cast takes its existing path, and no number is invented.
 */

#include "FFXIVActionDamage.h"

#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "Player.h"
#include "SpellInfo.h"
#include "Unit.h"
#include "FFXIV/Stats/FFXIVStatTables.h"
#include "TSEvent.h"
#include "TSEvents.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace FFXIV::Combat
{
// Defined below the table section; the row selector needs it first.
FfxivPositional FfxivPositionalFromAngleDegrees(int32 degrees);

namespace
{
std::vector<FfxivActionPotencyRow> g_potencyRows;
std::vector<FfxivActionComboRow> g_comboRows;
std::vector<FfxivActionSpellRow> g_spellRows;

// The last action a player cast, for the combo check. Module-owned state: the Player type is not
// extended by this slice (the snapshot is the only Player addition the stats slice made).
struct FfxivComboEntry
{
    uint32 lastActionId = 0;
    uint32 lastActionMs = 0;
};

std::unordered_map<ObjectGuid, FfxivComboEntry> g_comboState;

// Most specific first: exact job (2), generic (1), another job (0). Then the highest minLevel.
int32 JobSpecificity(FfxivActionPotencyRow const& row, uint8 classJobId)
{
    if (row.classJob == classJobId)
        return 2;
    return IsFfxivActionGenericJob(row.classJob) ? 1 : 0;
}

bool IsSortedPotency(std::vector<FfxivActionPotencyRow> const& rows)
{
    return std::is_sorted(rows.begin(), rows.end(),
        [](FfxivActionPotencyRow const& left, FfxivActionPotencyRow const& right)
        {
            if (left.actionId != right.actionId)
                return left.actionId < right.actionId;
            return left.minLevel < right.minLevel;
        });
}

bool IsSortedCombo(std::vector<FfxivActionComboRow> const& rows)
{
    return std::is_sorted(rows.begin(), rows.end(),
        [](FfxivActionComboRow const& left, FfxivActionComboRow const& right)
        {
            if (left.actionId != right.actionId)
                return left.actionId < right.actionId;
            return left.nextActionId < right.nextActionId;
        });
}

bool IsSortedSpell(std::vector<FfxivActionSpellRow> const& rows)
{
    return std::is_sorted(rows.begin(), rows.end(),
        [](FfxivActionSpellRow const& left, FfxivActionSpellRow const& right)
        {
            return left.spellId < right.spellId;
        });
}

FfxivPotencySelection MakeSelection(FfxivPotencySource source, uint16 potency)
{
    FfxivPotencySelection selection;
    selection.source = source;
    selection.potency = potency;
    return selection;
}

// The potency column for one cast, given the winning row. `angleDegrees` is only read when the
// target is hostile (a self/ally cast has no hitbox sector).
FfxivPotencySelection SelectFromRow(FfxivActionPotencyRow const& row, bool comboCorrect,
                                    int32 angleDegrees, bool hostileTarget)
{
    if (hostileTarget)
    {
        // Combo first (the task's rule; the reference also picks combo before any positional, which
        // it never applies -- Action.cpp only ever chooses isCorrectCombo ? comboPotency : potency).
        if (comboCorrect && row.combo > 0)
            return MakeSelection(FfxivPotencySource::Combo, row.combo);

        switch (FfxivPositionalFromAngleDegrees(angleDegrees))
        {
            case FfxivPositional::Flank:
                if (row.flank > 0)
                    return MakeSelection(FfxivPotencySource::Flank, row.flank);
                break;
            case FfxivPositional::Rear:
                if (row.rear > 0)
                    return MakeSelection(FfxivPotencySource::Rear, row.rear);
                break;
            case FfxivPositional::Front:
            default:
                if (row.front > 0)
                    return MakeSelection(FfxivPotencySource::Front, row.front);
                break;
        }

        if (row.damage > 0)
            return MakeSelection(FfxivPotencySource::Plain, row.damage);
    }
    else if (row.damage > 0)
    {
        return MakeSelection(FfxivPotencySource::Plain, row.damage);
    }

    // A heal-only row (Cure, Medica, Physick): the cure column is the action's own potency.
    if (row.cure > 0)
        return MakeSelection(FfxivPotencySource::Cure, row.cure);

    return MakeSelection(FfxivPotencySource::None, 0);
}

// Prune entries that can no longer affect a combo: the player is gone or the window has passed.
void PruneComboState(uint32 nowMs)
{
    for (auto it = g_comboState.begin(); it != g_comboState.end();)
    {
        bool const expired = !IsFfxivComboWindowOpen(it->second.lastActionMs, nowMs);
        bool const unused = it->second.lastActionId == 0;
        it = (expired || unused) ? g_comboState.erase(it) : std::next(it);
    }
}
} // namespace

// --------------------------------------------------------------------------
// tables
// --------------------------------------------------------------------------

bool IsFfxivActionGenericJob(uint8 classJob)
{
    return classJob == 0 || classJob == FFXIV_ACTION_ROLE_JOB;
}

bool IsValidFfxivActionPotencyClassJob(uint8 classJob)
{
    // A sheet ClassJob row, or the cross-job role value. Anything else names no job (and 255 is
    // above FFXIV_JOB_COUNT, so the loader must ask this before dropping the row as out of range).
    return classJob < Stats::FFXIV_JOB_COUNT || classJob == FFXIV_ACTION_ROLE_JOB;
}

void SetFfxivActionDamageTables(std::vector<FfxivActionPotencyRow> potency,
                                std::vector<FfxivActionComboRow> combo)
{
    if (!IsSortedPotency(potency))
        std::sort(potency.begin(), potency.end(),
            [](FfxivActionPotencyRow const& left, FfxivActionPotencyRow const& right)
            {
                if (left.actionId != right.actionId)
                    return left.actionId < right.actionId;
                return left.minLevel < right.minLevel;
            });
    if (!IsSortedCombo(combo))
        std::sort(combo.begin(), combo.end(),
            [](FfxivActionComboRow const& left, FfxivActionComboRow const& right)
            {
                if (left.actionId != right.actionId)
                    return left.actionId < right.actionId;
                return left.nextActionId < right.nextActionId;
            });

    g_potencyRows = std::move(potency);
    g_comboRows = std::move(combo);
    g_comboState.clear();
}

void SetFfxivActionSpellRows(std::vector<FfxivActionSpellRow> spellRows)
{
    if (!IsSortedSpell(spellRows))
        std::sort(spellRows.begin(), spellRows.end(),
            [](FfxivActionSpellRow const& left, FfxivActionSpellRow const& right)
            {
                return left.spellId < right.spellId;
            });

    g_spellRows = std::move(spellRows);
}

void ClearFfxivActionDamageTables()
{
    SetFfxivActionDamageTables({}, {});
    SetFfxivActionSpellRows({});
}

bool HasFfxivActionPotencyRows()
{
    return !g_potencyRows.empty();
}

bool HasFfxivActionSpellRows()
{
    return !g_spellRows.empty();
}

namespace
{
void LoadFfxivActionComboRows()
{
    QueryResult result = WorldDatabase.Query("SELECT actionId, nextActionId FROM ffxiv_action_combo");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 FFXIV action combo rows. DB table `ffxiv_action_combo` is "
                                      "missing or empty; combo potency stays off.");
        return;
    }

    std::vector<FfxivActionComboRow> rows;
    do
    {
        Field* fields = result->Fetch();
        FfxivActionComboRow row;
        row.actionId = fields[0].GetUInt32();
        row.nextActionId = fields[1].GetUInt32();
        if (row.actionId == 0 || row.nextActionId == 0)
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_action_combo` has a zero action id ({} -> {}), ignored.",
                         row.actionId, row.nextActionId);
            continue;
        }
        rows.push_back(row);
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} FFXIV action combo rows", uint32(rows.size()));
    // Stored directly; SetFfxivActionDamageTables would clear the potency rows already loaded.
    g_comboRows = std::move(rows);
    if (!IsSortedCombo(g_comboRows))
        std::sort(g_comboRows.begin(), g_comboRows.end(),
            [](FfxivActionComboRow const& left, FfxivActionComboRow const& right)
            {
                if (left.actionId != right.actionId)
                    return left.actionId < right.actionId;
                return left.nextActionId < right.nextActionId;
            });
}

void LoadFfxivActionSpellRows()
{
    QueryResult result = WorldDatabase.Query(
        "SELECT actionId, spellId, category, preservesCombo FROM ffxiv_action_spell");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 FFXIV action spell rows. DB table `ffxiv_action_spell` is "
                                      "missing or empty; no spell resolves to an action and every cast keeps "
                                      "its existing damage path.");
        return;
    }

    std::vector<FfxivActionSpellRow> rows;
    do
    {
        Field* fields = result->Fetch();
        FfxivActionSpellRow row;
        row.actionId = fields[0].GetUInt32();
        row.spellId = fields[1].GetUInt32();
        row.category = fields[2].GetUInt8();
        row.preservesCombo = fields[3].GetUInt8() != 0;
        if (row.actionId == 0 || row.spellId == 0)
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_action_spell` has a zero action or spell id ({} -> {}), "
                                    "ignored.", row.actionId, row.spellId);
            continue;
        }
        rows.push_back(row);
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} FFXIV action spell rows", uint32(rows.size()));
    SetFfxivActionSpellRows(std::move(rows));
}
} // namespace

void LoadFfxivActionDamageTables()
{
    uint32 const oldMSTime = getMSTime();

    ClearFfxivActionDamageTables();

    QueryResult result = WorldDatabase.Query(
        "SELECT actionId, classJob, minLevel, kind, damage, cure, combo, flank, front, rear "
        "FROM ffxiv_action_potency");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 FFXIV action potency rows. DB table `ffxiv_action_potency` "
                                      "is missing or empty; every action keeps its existing damage path.");
        // Still try the other two tables so a half-landed set is visible in the log.
        LoadFfxivActionComboRows();
        LoadFfxivActionSpellRows();
        return;
    }

    std::vector<FfxivActionPotencyRow> rows;
    do
    {
        Field* fields = result->Fetch();
        FfxivActionPotencyRow row;
        row.actionId = fields[0].GetUInt32();
        row.classJob = fields[1].GetUInt8();
        row.minLevel = fields[2].GetUInt8();
        row.kind = fields[3].GetUInt8();
        row.damage = fields[4].GetUInt16();
        row.cure = fields[5].GetUInt16();
        row.combo = fields[6].GetUInt16();
        row.flank = fields[7].GetUInt16();
        row.front = fields[8].GetUInt16();
        row.rear = fields[9].GetUInt16();

        if (row.actionId == 0)
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_action_potency` has a zero action id, ignored.");
            continue;
        }
        if (!IsValidFfxivActionPotencyClassJob(row.classJob))
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_action_potency` has action {} with classJob {} "
                                    "(expected 0..{} or {}), ignored.", row.actionId,
                         uint32(row.classJob), uint32(Stats::FFXIV_JOB_COUNT - 1),
                         uint32(FFXIV_ACTION_ROLE_JOB));
            continue;
        }
        // The model table goes to L100 even though the server caps gameplay at FFXIV_MAX_LEVEL; a
        // row above the cap is simply unreachable, not an error.
        if (row.minLevel > Stats::FFXIV_LEVEL_TABLE_MAX)
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_action_potency` has action {} with minLevel {} above {}, "
                                    "ignored.", row.actionId, uint32(row.minLevel),
                         uint32(Stats::FFXIV_LEVEL_TABLE_MAX));
            continue;
        }
        rows.push_back(row);
    } while (result->NextRow());

    TC_LOG_INFO("server.loading", ">> Loaded {} FFXIV action potency rows in {} ms", uint32(rows.size()),
                GetMSTimeDiffToNow(oldMSTime));
    SetFfxivActionDamageTables(std::move(rows), {});
    LoadFfxivActionComboRows();
    LoadFfxivActionSpellRows();

    // Startup proof that the `spells` logger/appenders are live; the action hooks below use the
    // same category, so a missing line here means a log-config problem, not an unreached hook.
    TC_LOG_ERROR("sql.sql", "[ffxiv-debug] spells logger active: potency {} combo {} spell {}",
                uint32(g_potencyRows.size()), uint32(g_comboRows.size()), uint32(g_spellRows.size()));
}

FfxivActionSpellRow const* FindFfxivActionSpellRow(uint32 spellId)
{
    if (spellId == 0)
        return nullptr;

    auto const it = std::lower_bound(g_spellRows.begin(), g_spellRows.end(), spellId,
        [](FfxivActionSpellRow const& row, uint32 id) { return row.spellId < id; });
    if (it == g_spellRows.end() || it->spellId != spellId)
        return nullptr;
    return &*it;
}

uint32 ResolveFfxivActionIdFromSpell(uint32 spellId)
{
    FfxivActionSpellRow const* row = FindFfxivActionSpellRow(spellId);
    return row ? row->actionId : 0;
}

FfxivActionPotencyRow const* FindFfxivActionPotencyRow(uint32 actionId, uint8 classJobId, uint8 level)
{
    auto const first = std::lower_bound(g_potencyRows.begin(), g_potencyRows.end(), actionId,
        [](FfxivActionPotencyRow const& row, uint32 id) { return row.actionId < id; });
    if (first == g_potencyRows.end() || first->actionId != actionId)
        return nullptr;

    FfxivActionPotencyRow const* best = nullptr;
    int32 bestSpecificity = 0;
    for (auto it = first; it != g_potencyRows.end() && it->actionId == actionId; ++it)
    {
        if (it->minLevel > level)
            continue;

        int32 const specificity = JobSpecificity(*it, classJobId);
        if (specificity == 0)
            continue; // another job's row is never used

        // Precedence: job specificity first, then the highest minLevel at or below the level.
        if (!best || specificity > bestSpecificity
            || (specificity == bestSpecificity && it->minLevel > best->minLevel))
        {
            best = &*it;
            bestSpecificity = specificity;
        }
    }
    return best;
}

int32 ComputeFfxivActionWeaponDamagePercent(FfxivDerivedStats const& stats)
{
    // No FFXIV gear model yet: the level term of f(WD) alone. This is the exact
    // expression the cast hook below uses, kept here so the tooltip scalar
    // (ComputeFfxivTooltipMultiplierBasisPoints) cannot drift from the live damage.
    return ComputeFfxivWeaponDamagePercent(stats, 0);
}

FfxivPartyComposition FfxivPartyFromGroup(::Player& player)
{
    FfxivPartyComposition party;
    auto addRole = [&party](Player* member)
    {
        if (!member || !member->IsInWorld())
            return;
        uint8 const classJobId = Stats::GetFfxivClassJobId(member->GetClass());
        FfxivPartyRole role;
        if (GetFfxivPartyRole(classJobId, role))
            party = party.With(role);
    };

    addRole(&player);
    if (Group* group = player.GetGroup())
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
            addRole(reference->GetSource());
    return party;
}

std::vector<FfxivActionPlainPotency> CollectFfxivActionPlainPotencies(uint8 classJobId, uint8 level)
{
    std::vector<FfxivActionPlainPotency> entries;
    for (std::size_t first = 0; first < g_potencyRows.size();)
    {
        uint32 const actionId = g_potencyRows[first].actionId;
        // One answer per action id: the same precedence the damage hook resolves with.
        FfxivActionPotencyRow const* const row = FindFfxivActionPotencyRow(actionId, classJobId, level);
        if (row && row->damage > 0)
            entries.push_back(FfxivActionPlainPotency{ actionId, row->damage });

        // Skip the rest of this action's rows (sorted by actionId, then minLevel).
        std::size_t next = first;
        while (next < g_potencyRows.size() && g_potencyRows[next].actionId == actionId)
            ++next;
        first = next;
    }
    return entries;
}

// --------------------------------------------------------------------------
// combo table
// --------------------------------------------------------------------------

bool IsFfxivComboCorrect(uint32 lastActionId, uint32 actionId)
{
    if (lastActionId == 0 || actionId == 0)
        return false;

    auto const first = std::lower_bound(g_comboRows.begin(), g_comboRows.end(), lastActionId,
        [](FfxivActionComboRow const& row, uint32 id) { return row.actionId < id; });
    for (auto it = first; it != g_comboRows.end() && it->actionId == lastActionId; ++it)
        if (it->nextActionId == actionId)
            return true;
    return false;
}

bool IsFfxivComboStep(uint32 actionId)
{
    if (actionId == 0)
        return false;

    return std::any_of(g_comboRows.begin(), g_comboRows.end(),
        [actionId](FfxivActionComboRow const& row) { return row.nextActionId == actionId; });
}

bool IsFfxivComboStarter(uint32 actionId)
{
    if (actionId == 0)
        return false;

    auto const first = std::lower_bound(g_comboRows.begin(), g_comboRows.end(), actionId,
        [](FfxivActionComboRow const& row, uint32 id) { return row.actionId < id; });
    return first != g_comboRows.end() && first->actionId == actionId;
}

bool IsFfxivComboWindowOpen(uint32 lastActionMs, uint32 nowMs)
{
    // GameTime is monotonic within a world session; a wrap is treated as closed rather than open.
    return nowMs >= lastActionMs && nowMs - lastActionMs <= FFXIV_COMBO_WINDOW_MS;
}

bool HasFfxivActionComboRows()
{
    return !g_comboRows.empty();
}

uint32 NextFfxivComboActionId(uint32 actionId)
{
    if (actionId == 0)
        return 0;

    auto const first = std::lower_bound(g_comboRows.begin(), g_comboRows.end(), actionId,
        [](FfxivActionComboRow const& row, uint32 id) { return row.actionId < id; });
    if (first == g_comboRows.end() || first->actionId != actionId)
        return 0;
    // Rows for one action are sorted, so the first one is the lowest successor.
    return first->nextActionId;
}

uint32 ResolveFfxivSpellFromActionId(uint32 actionId)
{
    if (actionId == 0)
        return 0;

    for (FfxivActionSpellRow const& row : g_spellRows)
        if (row.actionId == actionId)
            return row.spellId;
    return 0;
}

std::vector<uint32> SelectFfxivActionSpellsToLearn(uint8 classJobId, uint8 level,
    std::function<bool(uint32)> const& isKnown)
{
    std::vector<uint32> spellIds;
    if (g_potencyRows.empty() || g_spellRows.empty())
        return spellIds;

    // Rows are sorted by actionId: walk one action's rows at a time and use the same job rule the
    // potency lookup uses (exact job, or a generic 0/255 row), gated by the level.
    for (std::size_t first = 0; first < g_potencyRows.size();)
    {
        uint32 const actionId = g_potencyRows[first].actionId;
        bool matches = false;
        std::size_t next = first;
        for (; next < g_potencyRows.size() && g_potencyRows[next].actionId == actionId; ++next)
        {
            FfxivActionPotencyRow const& row = g_potencyRows[next];
            if (row.minLevel > level)
                continue;
            if (JobSpecificity(row, classJobId) > 0)
                matches = true;
        }
        first = next;

        if (!matches)
            continue;

        uint32 const spellId = ResolveFfxivSpellFromActionId(actionId);
        if (spellId == 0 || isKnown(spellId))
            continue;
        spellIds.push_back(spellId);
    }

    std::sort(spellIds.begin(), spellIds.end());
    spellIds.erase(std::unique(spellIds.begin(), spellIds.end()), spellIds.end());
    return spellIds;
}

uint32 GetFfxivLastActionId(ObjectGuid caster, uint32 nowMs)
{
    auto const stateIt = g_comboState.find(caster);
    if (stateIt == g_comboState.end())
        return 0;
    if (!IsFfxivComboWindowOpen(stateIt->second.lastActionMs, nowMs))
        return 0; // expired: the window, not the map, is the authority here (PruneComboState follows)
    return stateIt->second.lastActionId;
}

void AdvanceFfxivComboState(ObjectGuid caster, uint32 actionId, uint32 lastActionId, uint32 nowMs,
                            uint8 category, bool preservesCombo)
{
    // Sapphire Action.cpp: if (isWeaponskill() && !ComboContinue) { hit && (!isComboAction() ||
    // isCorrectCombo()) ? setLastComboActionId(id) : setLastComboActionId(0); }. The hook only runs
    // for a cast that hit, so the hit term is already true here.
    if (category != FFXIV_ACTION_CATEGORY_WEAPONSKILL)
        return; // spells and abilities (damaging oGCDs included) never touch the chain

    if (preservesCombo)
    {
        TC_LOG_ERROR("sql.sql", "[ffxiv-debug] combo action {}: PreservesCombo weaponskill, chain {} preserved",
                     actionId, lastActionId);
        return;
    }

    bool const isStep = IsFfxivComboStep(actionId);
    if (!isStep || IsFfxivComboCorrect(lastActionId, actionId))
    {
        g_comboState[caster] = FfxivComboEntry{ actionId, nowMs };
        return;
    }

    g_comboState.erase(caster);
    TC_LOG_ERROR("sql.sql", "[ffxiv-debug] combo action {}: weaponskill step after {}, chain broken",
                 actionId, lastActionId);
}

bool WasFfxivCorrectComboStep(ObjectGuid caster, uint32 actionId, uint32 castMs)
{
    if (actionId == 0 || !IsFfxivComboStep(actionId))
        return false;
    auto const stateIt = g_comboState.find(caster);
    if (stateIt == g_comboState.end() || stateIt->second.lastActionId != actionId)
    {
        TC_LOG_ERROR("sql.sql", "[ffxiv-debug] combo step {}: not recorded for {} (state {})", actionId,
                     caster.ToString(), stateIt == g_comboState.end() ? 0 : stateIt->second.lastActionId);
        return false;
    }
    // The damage hook records the step in the same cast, but its millisecond can differ from the
    // resolve hook's by a tick. The state only exists for a correct step (a wrong predecessor erases
    // it), so the open combo window is the right test; requiring exact millisecond equality made
    // every comboed Beast Gauge builder silently award nothing.
    bool const correct = IsFfxivComboWindowOpen(stateIt->second.lastActionMs, castMs);
    TC_LOG_ERROR("sql.sql", "[ffxiv-debug] combo step {}: recorded at {} (now {}), window open {}",
                 actionId, stateIt->second.lastActionMs, castMs, correct);
    return correct;
}

// --------------------------------------------------------------------------
// selection and math
// --------------------------------------------------------------------------

FfxivPositional FfxivPositionalFromAngleDegrees(int32 degrees)
{
    // Normalise into -180..180 inclusive, then compare the magnitude.
    degrees %= 360;
    if (degrees > 180)
        degrees -= 360;
    else if (degrees < -180)
        degrees += 360;

    int32 const magnitude = std::abs(degrees);
    if (magnitude < FFXIV_POSITIONAL_FRONT_DEGREES)
        return FfxivPositional::Front;
    if (magnitude < FFXIV_POSITIONAL_REAR_DEGREES)
        return FfxivPositional::Flank;
    return FfxivPositional::Rear;
}

FfxivPotencySelection SelectFfxivActionPotency(uint32 actionId, uint8 classJobId, uint8 level,
                                               uint32 lastActionId, int32 targetAngleDegrees,
                                               bool hostileTarget)
{
    FfxivActionPotencyRow const* row = FindFfxivActionPotencyRow(actionId, classJobId, level);
    if (!row)
        return MakeSelection(FfxivPotencySource::None, 0);

    bool const comboCorrect = IsFfxivComboCorrect(lastActionId, actionId);
    return SelectFromRow(*row, comboCorrect, targetAngleDegrees, hostileTarget);
}

FfxivDirectDamageResult ComputeFfxivActionDamage(FfxivDerivedStats const& stats,
                                                 FfxivPotencySelection const& selection,
                                                 int32 weaponDamagePercent, uint8 targetLevel,
                                                 FfxivPartyComposition party, FfxivRollSource& rolls)
{
    FfxivDirectDamageResult result;
    if (!selection.IsDamage() || selection.potency == 0)
        return result;

    // The published pipeline is FFXIVCombatMath's; this wrapper only supplies the selected potency.
    result = ComputeFfxivDirectDamage(stats, selection.potency, weaponDamagePercent, targetLevel, party,
                                      FfxivTraitState{}, rolls);
    return result;
}

FfxivHealResult ComputeFfxivActionHeal(FfxivDerivedStats const& stats,
                                       FfxivPotencySelection const& selection,
                                       int32 weaponDamagePercent, uint8 targetLevel,
                                       FfxivPartyComposition party, FfxivRollSource& rolls)
{
    FfxivHealResult result;
    if (selection.source != FfxivPotencySource::Cure || selection.potency == 0)
        return result;

    result = ComputeFfxivHeal(stats, selection.potency, weaponDamagePercent, targetLevel, party,
                              FfxivTraitState{}, rolls);
    return result;
}

// --------------------------------------------------------------------------
// the live hook
// --------------------------------------------------------------------------

bool TryApplyFfxivActionEffect(::Player& caster, ::Unit& target, ::SpellInfo const* spellInfo)
{
    if (!spellInfo || !HasFfxivActionPotencyRows())
        return false;

    FfxivDerivedStats const& stats = caster.GetFFXIVStats();
    // The tables are keyed by the retail Action id; the cast carries the WoW spell id. A spell with
    // no mapping row is not an FFXIV action cast: return before reading or touching any state, which
    // is exactly the behaviour of an absent map (and the shipping state today).
    FfxivActionSpellRow const* const spellRow = FindFfxivActionSpellRow(spellInfo->Id);
    if (!spellRow)
        return false;
    uint32 const actionId = spellRow->actionId;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    PruneComboState(nowMs);

    // Read the combo state first: this cast may replace it. An area action calls this hook once per
    // target, so `recordedThisCast` makes the update below idempotent within one cast.
    uint32 lastActionId = 0;
    bool recordedThisCast = false;
    auto const stateIt = g_comboState.find(caster.GetGUID());
    if (stateIt != g_comboState.end())
    {
        recordedThisCast = stateIt->second.lastActionId == actionId && stateIt->second.lastActionMs == nowMs;
        // The chain is the FFXIV action window, not WoW combat: a neutral BattleNpc training dummy
        // is a legal hostile target (Sapphire) but can never set WoW's combat state, so gating this
        // on IsInCombat() made every combo step read "no predecessor" and the gauge/highlight stay 0.
        if (IsFfxivComboWindowOpen(stateIt->second.lastActionMs, nowMs))
            lastActionId = stateIt->second.lastActionId;
    }

    bool const hostileTarget = &target != &caster && !caster.IsFriendlyTo(&target);
    int32 angleDegrees = 0;
    if (hostileTarget)
    {
        float const radians = target.GetRelativeAngle(&caster);
        angleDegrees = int32(std::lround(radians * 180.0f / float(M_PI)));
    }

    FfxivPotencySelection const selection =
        SelectFfxivActionPotency(actionId, stats.jobId, stats.level, lastActionId, angleDegrees,
                                 hostileTarget);
    if (selection.source == FfxivPotencySource::None)
    {
        TC_LOG_ERROR("sql.sql", "[ffxiv-debug] action {} job {} level {} last {} -> no potency row",
                    actionId, uint32(stats.jobId), uint32(stats.level), lastActionId);
        return false; // a table action with no usable potency: existing path untouched
    }
    TC_LOG_ERROR("sql.sql", "[ffxiv-debug] action {} job {} level {} last {} -> potency {} source {}",
                actionId, uint32(stats.jobId), uint32(stats.level), lastActionId,
                uint32(selection.potency), uint32(selection.source));

    // No FFXIV gear model yet: f(WD) is its level term alone (one definition, shared with the
    // tooltip scalar so the printed estimate describes this same number).
    int32 const weaponDamagePercent = ComputeFfxivActionWeaponDamagePercent(stats);
    uint8 const targetLevel = uint8(std::min<uint32>(target.GetLevel(), 255));

    FfxivPartyComposition const party = FfxivPartyFromGroup(caster);
    FfxivSeededRollSource rolls(uint64(caster.GetGUID().GetRawValue()) * 0x9E3779B97F4A7C15ull
                                + uint64(actionId) * 0xBF58476D1CE4E5B9ull + uint64(nowMs));

    bool applied = false;
    if (selection.IsDamage())
    {
        if (target.IsAlive())
        {
            FfxivDirectDamageResult const result = ComputeFfxivActionDamage(
                stats, selection, weaponDamagePercent, targetLevel, party, rolls);
            int32 value = int32(result.value);
            // @tswow-begin
            // FFXIV-named hook: the number is final (FFXIVCombatMath's own crit/direct-hit/variance/
            // level stages are inside) and it is applied here, not through Spell::m_damage, so WoW's
            // spell-power, armor, crit and spell-bonus stages never touch it. A livescript may adjust
            // the final value (Set 0 to take no damage); the crit flag below is the FFXIV roll.
            FIRE_ID(spellInfo->events.id, Spell, OnFfxivActionDamage, TSSpellInfo(spellInfo), TSUnit(&target),
                    TSMutableNumber<int32>(&value), TSNumber<uint8>(result.crit ? 1 : 0));
            // @tswow-end
            if (value > 0)
            {
                // The FFXIV number is already final: no WoW spell bonus or crit stage is applied on
                // top of it, which is why this hook deals the damage itself instead of filling
                // Spell::m_damage for the vanilla spell-damage path.
                SpellNonMeleeDamage damageInfo(&caster, &target, spellInfo->Id, spellInfo->GetSchoolMask());
                damageInfo.damage = uint32(value);
                damageInfo.HitInfo = result.crit ? SPELL_HIT_TYPE_CRIT : 0;
                caster.SendSpellNonMeleeDamageLog(&damageInfo);
                caster.DealSpellDamage(&damageInfo, true);
                // Sapphire's ActionResult::execute calls target->onActionHostile(source) after the
                // hit. The vanilla DealSpellDamage path does not start combat for a neutral/BattleNpc
                // training dummy, so IsInCombat() stayed false and the next combo step never read the
                // chain (gauge 0, no highlight). Starting it here opens the FFXIV combo window.
                caster.SetInCombatWith(&target);
                applied = true;
            }
        }
    }
    else if (selection.source == FfxivPotencySource::Cure && target.IsAlive())
    {
        FfxivHealResult const result = ComputeFfxivActionHeal(
            stats, selection, weaponDamagePercent, targetLevel, party, rolls);
        if (result.value > 0)
        {
            HealInfo healInfo(&caster, &target, uint32(result.value), spellInfo,
                              spellInfo->GetSchoolMask());
            caster.HealBySpell(healInfo, result.crit);
            applied = true;
        }
    }

    if (!applied)
    {
        TC_LOG_ERROR("sql.sql", "[ffxiv-debug] not applied (no target damage/heal)");
        return true; // the action was ours; nothing to apply, and the vanilla dummy has nothing to do
    }

    // Combo window (see the header): only a weaponskill without PreservesCombo touches it -- a
    // starter, a non-combo weaponskill or a correctly chained step becomes the new last action, a
    // step with the wrong predecessor breaks the chain. Spells, abilities (damaging oGCDs included)
    // and PreservesCombo weaponskills (the Warrior's gauge spenders) leave it alone.
    if (!recordedThisCast) // another target of the same cast already advanced it
        AdvanceFfxivComboState(caster.GetGUID(), actionId, lastActionId, nowMs, spellRow->category,
                               spellRow->preservesCombo);

    return true;
}

} // namespace FFXIV::Combat
