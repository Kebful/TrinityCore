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

#include "FFXIVStatus.h"
#include "DatabaseEnv.h"
#include "FFXIV/Combat/FFXIVActionDamage.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "Unit.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace FFXIV::Status
{
namespace
{
std::unordered_map<uint32, FfxivStatusRow> Statuses;
std::unordered_map<uint32, uint32> StatusBySpell;                  // aura spell id -> status id
std::unordered_map<uint32, std::vector<FfxivStatusEffectRow>> StatusEffects;
std::unordered_map<uint64, FfxivActionDotRow> ActionDots;         // (action, status) -> tick model
std::unordered_map<uint32, uint64> ActionDotByAction;              // action -> its (action, status) key
std::unordered_map<uint64, int32> ActionStatusDurations;
std::unordered_map<uint32, std::vector<uint32>> ActionStatuses;   // action -> status ids, table order
std::unordered_set<uint64> ComboOnlyActionStatuses;               // (action, status) granted only by a correct combo
std::unordered_map<uint64, uint32> ActionStatusTraits;            // (action, status) -> the trait the grant waits for
std::unordered_map<uint32, uint32> TraitSpells;                   // trait -> its passive spell

uint64 ActionStatusKey(uint32 actionId, uint32 statusId)
{
    return (uint64(actionId) << 32) | statusId;
}

void LoadFfxivActionStatusRows(std::vector<FfxivActionStatusRow>& rows)
{
    QueryResult result = WorldDatabase.Query(
        "SELECT actionId, statusId, duration, comboOnly, traitId FROM ffxiv_action_status");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 FFXIV action status durations. DB table `ffxiv_action_status` "
                                      "is missing or empty; statuses use their own default duration.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        FfxivActionStatusRow row;
        row.actionId = fields[0].GetUInt32();
        row.statusId = fields[1].GetUInt32();
        row.duration = fields[2].GetInt32();
        row.comboOnly = fields[3].GetUInt8() != 0;
        row.traitId = fields[4].GetUInt16();
        if (row.duration <= 0)
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_action_status` has action {} status {} with duration {}, ignored.",
                         row.actionId, row.statusId, row.duration);
            continue;
        }
        rows.push_back(row);
    } while (result->NextRow());
}

void LoadFfxivTraitRows(std::vector<FfxivTraitRow>& rows)
{
    QueryResult result = WorldDatabase.Query("SELECT traitId, spellId FROM ffxiv_trait");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 FFXIV traits. DB table `ffxiv_trait` is missing or empty; "
                                      "an action status that waits for a trait never applies.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        FfxivTraitRow row;
        row.traitId = fields[0].GetUInt16();
        row.spellId = fields[1].GetUInt32();
        if (!sSpellMgr->GetSpellInfo(row.spellId))
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_trait` has trait {} with missing spell {}, ignored.",
                         row.traitId, row.spellId);
            continue;
        }
        rows.push_back(row);
    } while (result->NextRow());
}

// The aura container seen from one (target, source) pair.
class UnitStatusContainer final : public FfxivStatusContainer
{
public:
    UnitStatusContainer(Unit* target, Unit* source) : _target(target), _source(source ? source : target) { }

    bool Find(uint32 spellId, FfxivStatusInstance& instance) const override
    {
        Aura const* aura = _target->GetAura(spellId, _source->GetGUID());
        if (!aura)
            return false;
        instance.stacks = aura->GetStackAmount();
        instance.duration = aura->GetDuration();
        instance.maxDuration = aura->GetMaxDuration();
        return true;
    }

    bool Add(uint32 spellId, FfxivStatusInstance const& instance) override
    {
        Aura* aura = _source->AddAura(spellId, _target);
        if (!aura)
            return false;
        Write(aura, instance);
        return true;
    }

    void Update(uint32 spellId, FfxivStatusInstance const& instance) override
    {
        if (Aura* aura = _target->GetAura(spellId, _source->GetGUID()))
            Write(aura, instance);
    }

    void Remove(uint32 spellId) override
    {
        _target->RemoveAurasDueToSpell(spellId, _source->GetGUID(), 0, AURA_REMOVE_BY_CANCEL);
    }

private:
    static void Write(Aura* aura, FfxivStatusInstance const& instance)
    {
        if (aura->GetStackAmount() != instance.stacks)
            aura->SetStackAmount(instance.stacks);
        aura->SetMaxDuration(instance.maxDuration);
        aura->SetDuration(instance.duration);
    }

    Unit* _target;
    Unit* _source;
};
}

void SetFfxivStatusTables(std::vector<FfxivStatusRow> statuses, std::vector<FfxivActionStatusRow> actionStatuses,
                          std::vector<FfxivTraitRow> traits, std::vector<FfxivStatusEffectRow> effects,
                          std::vector<FfxivActionDotRow> dots)
{
    ClearFfxivStatusTables();
    for (FfxivStatusRow const& row : statuses)
    {
        Statuses[row.statusId] = row;
        StatusBySpell[row.spellId] = row.statusId;
    }
    for (FfxivActionStatusRow const& row : actionStatuses)
    {
        ActionStatusDurations[ActionStatusKey(row.actionId, row.statusId)] = row.duration;
        ActionStatuses[row.actionId].push_back(row.statusId);
        if (row.comboOnly)
            ComboOnlyActionStatuses.insert(ActionStatusKey(row.actionId, row.statusId));
        if (row.traitId)
            ActionStatusTraits[ActionStatusKey(row.actionId, row.statusId)] = row.traitId;
    }
    for (FfxivTraitRow const& row : traits)
        TraitSpells[row.traitId] = row.spellId;
    for (FfxivStatusEffectRow const& row : effects)
        StatusEffects[row.statusId].push_back(row);
    for (auto& [statusId, rows] : StatusEffects)
        std::sort(rows.begin(), rows.end(),
                  [](FfxivStatusEffectRow const& left, FfxivStatusEffectRow const& right)
                  { return left.kind < right.kind; });
    for (FfxivActionDotRow const& row : dots)
    {
        ActionDots[ActionStatusKey(row.actionId, row.statusId)] = row;
        ActionDotByAction.emplace(row.actionId, ActionStatusKey(row.actionId, row.statusId));
    }
}

void ClearFfxivStatusTables()
{
    Statuses.clear();
    StatusBySpell.clear();
    StatusEffects.clear();
    ActionDots.clear();
    ActionDotByAction.clear();
    ActionStatusDurations.clear();
    ActionStatuses.clear();
    ComboOnlyActionStatuses.clear();
    ActionStatusTraits.clear();
    TraitSpells.clear();
}

void LoadFfxivStatusTables()
{
    uint32 const oldMSTime = getMSTime();

    ClearFfxivStatusTables();

    QueryResult result = WorldDatabase.Query(
        "SELECT statusId, spellId, duration, maxStacks, refreshPolicy, flags FROM ffxiv_status");
    if (!result)
    {
        TC_LOG_INFO("server.loading", ">> Loaded 0 FFXIV statuses. DB table `ffxiv_status` is missing or empty; "
                                      "ApplyFfxivStatus applies nothing.");
        return;
    }

    std::vector<FfxivStatusRow> rows;
    do
    {
        Field* fields = result->Fetch();
        FfxivStatusRow row;
        row.statusId = fields[0].GetUInt32();
        row.spellId = fields[1].GetUInt32();
        row.duration = fields[2].GetInt32();
        row.maxStacks = fields[3].GetUInt8();
        row.refreshPolicy = fields[4].GetUInt8();
        row.flags = fields[5].GetUInt32();

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(row.spellId);
        if (!spellInfo || !spellInfo->HasEffect(SPELL_EFFECT_APPLY_AURA))
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_status` has status {} with spell {} that is missing or applies no aura, "
                                    "ignored.", row.statusId, row.spellId);
            continue;
        }
        if (row.refreshPolicy > FFXIV_STATUS_REFRESH_POLICY_MAX)
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_status` has status {} with refreshPolicy {} above {}, ignored.",
                         row.statusId, uint32(row.refreshPolicy), uint32(FFXIV_STATUS_REFRESH_POLICY_MAX));
            continue;
        }
        if (row.duration < 0)
        {
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_status` has status {} with duration {}; use the IsPermanent flag, "
                                    "ignored.", row.statusId, row.duration);
            continue;
        }
        rows.push_back(row);
    } while (result->NextRow());

    std::vector<FfxivActionStatusRow> actionRows;
    LoadFfxivActionStatusRows(actionRows);
    std::vector<FfxivTraitRow> traitRows;
    LoadFfxivTraitRows(traitRows);

    // ffxiv_status_effect: the hand-authored magnitudes (ffxiv-effect-values.ts). A status without a
    // row is a marker; the aura stays the identity DUMMY and a livescript owns its behaviour.
    std::vector<FfxivStatusEffectRow> effectRows;
    QueryResult effectResult = WorldDatabase.Query("SELECT statusId, kind, value FROM ffxiv_status_effect");
    if (!effectResult)
        TC_LOG_INFO("server.loading", ">> Loaded 0 FFXIV status effects. DB table `ffxiv_status_effect` is "
                                      "missing or empty; every status is a marker and our damage math reads no buffs.");
    else
    {
        do
        {
            Field* fields = effectResult->Fetch();
            FfxivStatusEffectRow row;
            row.statusId = fields[0].GetUInt32();
            row.kind = fields[1].GetUInt8();
            row.value = fields[2].GetInt16();
            if (row.statusId == 0 || row.kind > FFXIV_STATUS_EFFECT_FORCE_DIRECT_HIT)
            {
                TC_LOG_ERROR("sql.sql", "Table `ffxiv_status_effect` has (%u, kind %u, %d) with an unknown "
                                        "status or kind, ignored.", row.statusId, uint32(row.kind), row.value);
                continue;
            }
            effectRows.push_back(row);
        } while (effectResult->NextRow());
    }

    // ffxiv_action_dot: the tick model the status apply path writes into the carrier aura. periodMs
    // defaults to the 3 s base clock (INFERRED, no sheet column states it); a row that needs another
    // cadence overrides it explicitly.
    std::vector<FfxivActionDotRow> dotRows;
    QueryResult dotResult = WorldDatabase.Query(
        "SELECT actionId, statusId, potency, durationMs, comboOnly, traitId FROM ffxiv_action_dot");
    if (!dotResult)
        TC_LOG_INFO("server.loading", ">> Loaded 0 FFXIV action DoTs. DB table `ffxiv_action_dot` is missing "
                                      "or empty; periodic statuses tick the aura's own 0.");
    else
    {
        do
        {
            Field* fields = dotResult->Fetch();
            FfxivActionDotRow row;
            row.actionId = fields[0].GetUInt32();
            row.statusId = fields[1].GetUInt32();
            row.potency = fields[2].GetUInt16();
            row.durationMs = fields[3].GetUInt32();
            row.comboOnly = fields[4].GetUInt8() != 0;
            row.traitId = fields[5].GetUInt16();
            if (row.actionId == 0 || row.statusId == 0 || row.potency == 0)
            {
                TC_LOG_ERROR("sql.sql", "Table `ffxiv_action_dot` has (%u, %u, potency %u), ignored: a DoT "
                                        "row needs an action, a status and a potency.", row.actionId,
                             row.statusId, uint32(row.potency));
                continue;
            }
            dotRows.push_back(row);
        } while (dotResult->NextRow());
    }

    uint32 traitGates = 0;
    for (FfxivActionStatusRow const& row : actionRows)
    {
        if (!row.traitId)
            continue;
        ++traitGates;
        if (std::none_of(traitRows.begin(), traitRows.end(),
                         [&row](FfxivTraitRow const& trait) { return trait.traitId == row.traitId; }))
            TC_LOG_ERROR("sql.sql", "Table `ffxiv_action_status` has action {} status {} waiting for trait {}, which "
                                    "`ffxiv_trait` does not carry; the status will never apply.",
                         row.actionId, row.statusId, row.traitId);
    }

    TC_LOG_INFO("server.loading", ">> Loaded {} FFXIV statuses, {} action status durations ({} trait-gated), {} "
                                  "traits, {} status effects and {} action DoTs in {} ms", uint32(rows.size()),
                uint32(actionRows.size()), traitGates, uint32(traitRows.size()), uint32(effectRows.size()),
                uint32(dotRows.size()), GetMSTimeDiffToNow(oldMSTime));
    SetFfxivStatusTables(std::move(rows), std::move(actionRows), std::move(traitRows), std::move(effectRows),
                         std::move(dotRows));
}

FfxivStatusRow const* GetFfxivStatus(uint32 statusId)
{
    auto itr = Statuses.find(statusId);
    return itr == Statuses.end() ? nullptr : &itr->second;
}

std::vector<FfxivStatusEffectRow> const* GetFfxivStatusEffects(uint32 statusId)
{
    auto itr = StatusEffects.find(statusId);
    return itr == StatusEffects.end() ? nullptr : &itr->second;
}

std::vector<FfxivStatusEffectRow> CollectActiveFfxivStatusEffects(Unit& unit)
{
    std::vector<uint32> active;
    for (auto const& pair : unit.GetAppliedAuras())
    {
        AuraApplication const* application = pair.second;
        Aura const* aura = application ? application->GetBase() : nullptr;
        if (!aura)
            continue;
        auto const itr = StatusBySpell.find(aura->GetId());
        if (itr != StatusBySpell.end())
            active.push_back(itr->second);
    }
    std::sort(active.begin(), active.end());
    active.erase(std::unique(active.begin(), active.end()), active.end());

    std::vector<FfxivStatusEffectRow> rows;
    for (uint32 statusId : active)
        if (std::vector<FfxivStatusEffectRow> const* effects = GetFfxivStatusEffects(statusId))
            rows.insert(rows.end(), effects->begin(), effects->end());
    return rows;
}

FfxivActionDotRow const* GetFfxivActionDot(uint32 actionId, uint32 statusId)
{
    auto itr = ActionDots.find(ActionStatusKey(actionId, statusId));
    return itr == ActionDots.end() ? nullptr : &itr->second;
}

FfxivActionDotRow const* GetFfxivActionDotForAction(uint32 actionId)
{
    auto const key = ActionDotByAction.find(actionId);
    if (key == ActionDotByAction.end())
        return nullptr;
    auto const itr = ActionDots.find(key->second);
    return itr == ActionDots.end() ? nullptr : &itr->second;
}

int32 GetFfxivActionStatusDuration(uint32 actionId, uint32 statusId)
{
    auto itr = ActionStatusDurations.find(ActionStatusKey(actionId, statusId));
    return itr == ActionStatusDurations.end() ? 0 : itr->second;
}

std::vector<uint32> const* GetFfxivActionStatuses(uint32 actionId)
{
    auto itr = ActionStatuses.find(actionId);
    return itr == ActionStatuses.end() ? nullptr : &itr->second;
}

bool IsFfxivActionStatusComboOnly(uint32 actionId, uint32 statusId)
{
    return ComboOnlyActionStatuses.count(ActionStatusKey(actionId, statusId)) != 0;
}

bool ShouldApplyFfxivActionStatus(uint32 actionId, uint32 statusId, bool comboCorrect)
{
    return comboCorrect || !IsFfxivActionStatusComboOnly(actionId, statusId);
}

uint32 GetFfxivActionStatusTrait(uint32 actionId, uint32 statusId)
{
    auto itr = ActionStatusTraits.find(ActionStatusKey(actionId, statusId));
    return itr == ActionStatusTraits.end() ? 0 : itr->second;
}

uint32 GetFfxivTraitSpell(uint32 traitId)
{
    auto itr = TraitSpells.find(traitId);
    return itr == TraitSpells.end() ? 0 : itr->second;
}

bool IsFfxivActionStatusTraitMet(uint32 actionId, uint32 statusId, std::function<bool(uint32 spellId)> const& hasSpell)
{
    uint32 const traitId = GetFfxivActionStatusTrait(actionId, statusId);
    if (!traitId)
        return true;
    uint32 const spellId = GetFfxivTraitSpell(traitId);
    return spellId && hasSpell(spellId);
}

namespace
{
// Caster buffs are applied once per cast: an area effect calls EffectDummy once per target, and a
// stacking buff (Inner Release's 3 stacks) must not gain a stack per target.
struct CasterBuffApplication
{
    uint32 actionId = 0;
    uint32 statusId = 0;
    uint32 ms = 0;
};

std::unordered_map<uint64, CasterBuffApplication> LastCasterBuff;

bool FirstCasterBuffThisCast(uint64 casterGuid, uint32 actionId, uint32 statusId, uint32 nowMs)
{
    CasterBuffApplication& last = LastCasterBuff[casterGuid];
    if (last.actionId == actionId && last.statusId == statusId && nowMs >= last.ms && nowMs - last.ms < 500)
        return false;
    last = CasterBuffApplication{actionId, statusId, nowMs};
    return true;
}

// Write the tick FFXIVCombatMath computes into the aura's periodic effect. The amount is snapshotted
// once per application from the casting player's current stats (a re-apply overwrites it), and the
// engine then ticks that number on the aura's own cadence: no second timer, no per-tick recompute.
// Retail snapshots 500 ms before cast finish (B-FORMULA §F23.13); this slice uses the apply-time
// snapshot, an INFERRED simplification stated in the report.
void WriteFfxivActionDotTick(Player& caster, Unit& target, uint32 actionId, uint32 statusId, Aura& aura)
{
    FfxivActionDotRow const* dot = GetFfxivActionDot(actionId, statusId);
    if (!dot || dot->potency == 0)
        return;

    AuraEffect* periodic = nullptr;
    for (uint8 index = 0; index < MAX_SPELL_EFFECTS; ++index)
    {
        AuraEffect* effect = aura.GetEffect(index);
        if (effect && effect->GetAuraType() == SPELL_AURA_PERIODIC_DAMAGE)
        {
            periodic = effect;
            break;
        }
    }
    if (!periodic)
    {
        TC_LOG_ERROR("sql.sql", "[ffxiv-debug] action {} status {}: ffxiv_action_dot has potency {} but the "
                                "aura carries no PERIODIC_DAMAGE effect", actionId, statusId, uint32(dot->potency));
        return;
    }
    if (dot->durationMs && aura.GetMaxDuration() != int32(dot->durationMs))
        TC_LOG_ERROR("sql.sql", "[ffxiv-debug] action {} status {}: aura duration {} != ffxiv_action_dot.durationMs {}",
                     actionId, statusId, aura.GetMaxDuration(), dot->durationMs);

    auto const& stats = caster.GetFFXIVStats();
    std::vector<Combat::FfxivPercentFactor> buffs;
    Combat::FfxivCombatGuarantees guarantees;
    Combat::CollectFfxivStatusCombatModifiers(caster, target, buffs, guarantees);
    Combat::FfxivSeededRollSource rolls(uint64(caster.GetGUID().GetRawValue()) * 0x9E3779B97F4A7C15ull
                                        + uint64(actionId) * 0xBF58476D1CE4E5B9ull
                                        + uint64(statusId) * 0x94D049BB133111EBull
                                        + uint64(GameTime::GetGameTimeMS()));
    uint8 const targetLevel = uint8(std::min<uint32>(target.GetLevel(), 255));
    int32 const weaponDamagePercent = Combat::ComputeFfxivActionWeaponDamagePercent(stats);
    int64 const tick = Combat::ComputeFfxivDotTick(stats, dot->potency, weaponDamagePercent, targetLevel,
                                                   Combat::FfxivPartyFromGroup(caster), Combat::FfxivTraitState{},
                                                   rolls, buffs, nullptr, guarantees);
    periodic->SetAmount(int32(std::min<int64>(tick, std::numeric_limits<int32>::max())));
    TC_LOG_ERROR("sql.sql", "[ffxiv-debug] action {} status {}: DoT potency {} every {} ms -> tick {}",
                 actionId, statusId, uint32(dot->potency), uint32(periodic->GetAmplitude()),
                 periodic->GetAmount());
}
}

int ApplyFfxivActionStatuses(Player& caster, Unit& effectTarget, uint32 actionId)
{
    std::vector<uint32> const* statuses = GetFfxivActionStatuses(actionId);
    if (!statuses)
        return 0;

    uint32 const nowMs = GameTime::GetGameTimeMS();
    int applied = 0;
    // A "Combo Bonus: Grants <status>" row (comboOnly) goes out only when this cast was the correct step of its
    // chain. The damage hook of the same EffectDummy has already advanced the chain; the answer is the same for
    // every status of the cast, so it is asked for at most once, and only when a comboOnly row is met.
    std::optional<bool> comboCorrect;
    for (uint32 statusId : *statuses)
    {
        FfxivStatusRow const* row = GetFfxivStatus(statusId);
        if (!row)
            continue;
        // A grant that waits for a trait (Infuriate -> Nascent Chaos waits for trait 267) needs the trait's passive.
        if (!IsFfxivActionStatusTraitMet(actionId, statusId, [&caster](uint32 spellId) { return caster.HasSpell(spellId); }))
        {
            uint32 const traitId = GetFfxivActionStatusTrait(actionId, statusId);
            TC_LOG_ERROR("sql.sql", "[ffxiv-debug] status {} from action {} skipped: trait {} (spell {}) not learned",
                         statusId, actionId, traitId, GetFfxivTraitSpell(traitId));
            continue;
        }
        if (IsFfxivActionStatusComboOnly(actionId, statusId))
        {
            if (!comboCorrect)
                comboCorrect = Combat::WasFfxivCorrectComboStep(caster.GetGUID(), actionId, nowMs);
            if (!ShouldApplyFfxivActionStatus(actionId, statusId, *comboCorrect))
                continue;
        }
        // A debuff goes to the unit the action resolved on. A buff goes to that unit only when it is
        // friendly (self or an ally); a hostile effect's buff (e.g. Surging Tempest from Storm's Eye)
        // goes to the caster.
        bool const debuff = row->HasFlag(FFXIV_STATUS_FLAG_DEBUFF);
        Unit* destination = nullptr;
        if (debuff)
            destination = &effectTarget;
        else if (effectTarget.IsFriendlyTo(&caster))
            destination = &effectTarget;
        else
            destination = &caster;
        if (destination == &caster && !debuff &&
            !FirstCasterBuffThisCast(caster.GetGUID().GetRawValue(), actionId, statusId, nowMs))
            continue;
        bool const ok = ApplyFfxivStatus(destination, statusId, &caster,
                                         GetFfxivActionStatusDuration(actionId, statusId));
        uint32 stacks = 0;
        if (Aura* aura = ok ? destination->GetAura(row->spellId, caster.GetGUID()) : nullptr)
        {
            stacks = aura->GetStackAmount();
            // The DoT's tick is ours; write it into the periodic effect the engine ticks.
            WriteFfxivActionDotTick(caster, *destination, actionId, statusId, *aura);
        }
        TC_LOG_ERROR("sql.sql", "[ffxiv-debug] status {} from action {} -> {} (applied {}, stacks {})",
                     statusId, actionId, destination == &caster ? "caster" : "effect target", ok, stacks);
        if (ok)
            ++applied;
    }
    if (applied > 0)
        TC_LOG_DEBUG("spells", "FFXIV action {} applied {} status(es)", actionId, applied);
    return applied;
}

FfxivStatusApplyResult ResolveFfxivStatusApply(FfxivStatusRow const& row, FfxivStatusInstance const* existing,
                                               int32 durationMs)
{
    FfxivStatusApplyResult result;

    int32 duration = durationMs != 0 ? durationMs : row.duration;
    if (row.HasFlag(FFXIV_STATUS_FLAG_PERMANENT) || duration < 0)
        duration = FFXIV_STATUS_UNTIL_REMOVED;
    else if (duration == 0)
        return result;

    uint8 const stackCap = std::max<uint8>(row.maxStacks, 1);

    if (!existing)
    {
        // A status that stacks arrives at its full stack count: the sheet's grant line states the
        // number ("Grants 3 stacks of Inner Release" / Berserk) and Status column 4 is the cap, so a
        // new instance at 1 stack would render one stack of a three-stack buff. No sheet states a
        // "grants 1 of 3" case, so the cap is the count.
        result.outcome = FfxivStatusApplyOutcome::Added;
        result.instance = { stackCap, duration, duration };
        return result;
    }

    result.outcome = FfxivStatusApplyOutcome::Refreshed;
    result.instance.stacks = std::min<uint8>(std::max<uint8>(existing->stacks, 1), stackCap);
    if (result.instance.stacks < stackCap)
        ++result.instance.stacks;

    if (duration == FFXIV_STATUS_UNTIL_REMOVED || existing->duration == FFXIV_STATUS_UNTIL_REMOVED)
        result.instance.duration = FFXIV_STATUS_UNTIL_REMOVED;
    else if (FfxivStatusRefreshPolicy(row.refreshPolicy) == FfxivStatusRefreshPolicy::Extend)
        result.instance.duration = int32(std::min<int64>(int64(existing->duration) + duration,
                                                         std::numeric_limits<int32>::max()));
    else
        result.instance.duration = duration;
    result.instance.maxDuration = result.instance.duration;
    return result;
}

bool AdvanceFfxivStatus(FfxivStatusInstance& instance, uint32 diffMs)
{
    if (instance.duration == FFXIV_STATUS_UNTIL_REMOVED)
        return true;
    instance.duration = diffMs >= uint32(instance.duration) ? 0 : instance.duration - int32(diffMs);
    return instance.duration > 0;
}

bool ApplyFfxivStatusTo(FfxivStatusContainer& container, uint32 statusId, int32 durationMs)
{
    FfxivStatusRow const* row = GetFfxivStatus(statusId);
    if (!row)
        return false;

    FfxivStatusInstance existing;
    bool const present = container.Find(row->spellId, existing);
    FfxivStatusApplyResult const result = ResolveFfxivStatusApply(*row, present ? &existing : nullptr, durationMs);
    switch (result.outcome)
    {
        case FfxivStatusApplyOutcome::Added:
            return container.Add(row->spellId, result.instance);
        case FfxivStatusApplyOutcome::Refreshed:
            container.Update(row->spellId, result.instance);
            return true;
        default:
            TC_LOG_DEBUG("spells", "FFXIV status {} has no duration to apply (no default, none passed).", statusId);
            return false;
    }
}

bool RequestRemoveFfxivStatusFrom(FfxivStatusContainer& container, uint32 statusId)
{
    FfxivStatusRow const* row = GetFfxivStatus(statusId);
    if (!row || !row->HasFlag(FFXIV_STATUS_FLAG_CAN_STATUS_OFF))
        return false;

    FfxivStatusInstance existing;
    if (!container.Find(row->spellId, existing))
        return false;
    container.Remove(row->spellId);
    return true;
}

bool ApplyFfxivStatus(Unit* target, uint32 statusId, Unit* source, int32 durationMs)
{
    if (!target || !GetFfxivStatus(statusId))
        return false;
    UnitStatusContainer container(target, source);
    return ApplyFfxivStatusTo(container, statusId, durationMs);
}

bool RequestRemoveFfxivStatus(Unit* target, uint32 statusId, Unit* source)
{
    if (!target || !GetFfxivStatus(statusId))
        return false;
    UnitStatusContainer container(target, source);
    return RequestRemoveFfxivStatusFrom(container, statusId);
}
}
