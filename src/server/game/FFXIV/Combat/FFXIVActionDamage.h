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

#ifndef TRINITYCORE_FFXIV_ACTION_DAMAGE_H
#define TRINITYCORE_FFXIV_ACTION_DAMAGE_H

#include "FFXIV/Combat/FFXIVCombatMath.h"
#include "ObjectGuid.h"
#include <vector>

/*
 * The one core rule of the FFXIV combat slice: an action's cast resolves to
 *
 *     potency (plain / combo / flank / front / rear, or the cure column)
 *       -> FFXIVCombatMath's published pipeline
 *       -> a damage or heal amount applied to the cast's target
 *
 * Data
 * ----
 * Three world tables, all owned by the data slice (FFXIVActionPotency's sibling):
 *
 *   ffxiv_action_potency(actionId, classJob, minLevel, kind, damage, cure, dot, combo, flank, front, rear)
 *   ffxiv_action_combo(actionId, nextActionId)
 *   ffxiv_action_spell(actionId, spellId, category, preservesCombo)
 *
 * A combo row is read as "actionId, then nextActionId is a correct combo" -- the same direction as
 * the reference's `nextCombo` list (Sapphire aab9cb23, data/actions/player.json: Fast Blade 9 ->
 * [11, 15, 5822]). A cast is a correct combo when a row (lastActionId, thisActionId) exists.
 *
 * The potency and combo tables are keyed by the retail Action id (Heavy Swing = 31), but the cast
 * only carries the WoW spell id the module created for that action. ffxiv_action_spell is the map
 * between the two: the module writes it when it creates the action spells, and the hook resolves
 * spellInfo->Id through it before any lookup. A spell with no row is not an FFXIV action cast. The
 * same row carries the two Action sheet columns the combo rule reads: ActionCategory (column 3) and
 * PreservesCombo (column 37, Sapphire's ComboContinue).
 *
 * All three tables are loaded once at startup from the world database. A missing or empty table
 * leaves the tables empty and therefore leaves every cast on its existing path: no mapping row, no
 * FFXIV handling, no number invented. That is the shipping state until the data slice lands.
 *
 * Lookup precedence
 * -----------------
 * Rows are selected for one (actionId, classJob, level). Only rows whose minLevel is <= the level
 * are candidates; among those, an exact classJob match outranks a generic row, and a higher minLevel
 * outranks a lower one. Concretely, the winner is the candidate with the highest
 * (classJob == job ? 2 : generic ? 1 : 0, minLevel) pair. A generic row is classJob 0 (the sheet's
 * fallthrough) or classJob 255 (the role lines: Tank / DPS / Healer). No other job's row is ever
 * used.
 *
 * Potency precedence
 * ------------------
 * On a hostile target: combo (when the last action makes this a correct combo and the row states a
 * combo potency), then the target-relative positional column (flank / front / rear, by the angle
 * from the target's facing -- FFXIV positionals are measured on the *target's* hitbox), then the
 * plain damage column. A cure column is used only when the row states no damage-side potency at all.
 *
 * Combo window
 * ------------
 * The last action id is tracked per player and is only read while the player is in combat and the
 * entry is at most 12500 ms old (the reference's MAX_COMBO_LENGTH, Sapphire aab9cb23
 * src/common/Common.h:61). A damaging cast that is neither a combo starter nor a combo step clears
 * it (a non-combo action); heals and actions outside the combo table do not touch it. See
 * FFXIVActionDamage.cpp for the exact rule.
 */

class Player;
class Unit;
class SpellInfo;

namespace FFXIV::Combat
{
// One ffxiv_action_potency row. `kind` is FfxivActionPotencyKind; 0 is accepted for a row that
// only states a value. `dot` is the action's damage-over-time potency from the same description
// (the generator's ffxiv_action_dot carries the same number plus the cadence): a pure DoT row has
// `damage == 0` and `dot > 0`, so the direct-damage hook does not hit with it.
struct FfxivActionPotencyRow
{
    uint32 actionId = 0;
    uint8 classJob = 0;   // 0 or 255 = generic row, used when no exact job row applies
    uint8 minLevel = 0;   // applies at this level and above
    uint8 kind = 0;       // FfxivActionPotencyKind
    uint16 damage = 0;
    uint16 cure = 0;
    uint16 dot = 0;       // damage-over-time tick potency (0 when the action states none)
    uint16 combo = 0;     // potency when the previous action forms a correct combo
    uint16 flank = 0;     // potency when the attacker is on the target's flank
    uint16 front = 0;     // potency when the attacker is in front of the target
    uint16 rear = 0;      // potency when the attacker is behind the target
};

// One ffxiv_action_combo row: "actionId, then nextActionId" is a correct combo.
struct FfxivActionComboRow
{
    uint32 actionId = 0;
    uint32 nextActionId = 0;
};

// One ffxiv_action_spell row: the WoW spell the module created for a retail Action id. Both sides
// are unique; a spell appears at most once, and one action has at most one spell.
struct FfxivActionSpellRow
{
    uint32 actionId = 0;
    uint32 spellId = 0;
    uint8 category = 0;          // Action sheet column 3 ActionCategory (FFXIV_ACTION_CATEGORY_*)
    bool preservesCombo = false; // Action sheet column 37 PreservesCombo (Sapphire's ComboContinue)
};

// Action sheet column 3 ActionCategory: the only category the combo rule reads.
constexpr uint8 FFXIV_ACTION_CATEGORY_WEAPONSKILL = 3;

// The reference's combo window (Sapphire aab9cb23 src/common/Common.h:61 MAX_COMBO_LENGTH).
constexpr uint32 FFXIV_COMBO_WINDOW_MS = 12500;

// ffxiv_action_potency classJob 255: the role lines (Tank / DPS / Healer). Like 0 it applies to
// every job; unlike a ClassJob row (0..FFXIV_JOB_COUNT-1) it is cross-job data, not a sheet id.
constexpr uint8 FFXIV_ACTION_ROLE_JOB = 255;

// Positional sectors, measured on the target's facing: 0 degrees is the target facing the attacker.
// Front is the arc inside +-45 degrees, rear the arc at/beyond +-135 degrees, flank the rest (the
// standard 90/90/90 split; the split points themselves are INFERRED -- no shipped data states them).
constexpr int32 FFXIV_POSITIONAL_FRONT_DEGREES = 45;
constexpr int32 FFXIV_POSITIONAL_REAR_DEGREES = 135;

enum class FfxivPositional : uint8
{
    Front = 0,
    Flank,
    Rear,
};

// Which column of the row the cast resolved to; None means the action is not an FFXIV action here
// (no row, or a row with no usable potency) and the caller must take its existing path.
enum class FfxivPotencySource : uint8
{
    None = 0,
    Plain,
    Combo,
    Flank,
    Front,
    Rear,
    Cure,
};

struct FfxivPotencySelection
{
    FfxivPotencySource source = FfxivPotencySource::None;
    uint16 potency = 0;

    bool IsDamage() const
    {
        return source != FfxivPotencySource::None && source != FfxivPotencySource::Cure;
    }
};

// --- table access ---------------------------------------------------------------------------

// True when a potency row's classJob applies to every job: 0 (the sheet's generic fallthrough)
// or 255 (the role line). Such a row is only used when no exact ClassJob row applies.
bool IsFfxivActionGenericJob(uint8 classJob);

// The loader's classJob gate: an exact ClassJob row (0..FFXIV_JOB_COUNT-1) or the role value 255.
// A row outside that set names no job and is dropped.
bool IsValidFfxivActionPotencyClassJob(uint8 classJob);

// Startup load from the world database. Missing tables log once and leave the tables empty.
void LoadFfxivActionDamageTables();

// Replace the potency and combo tables. Used by the loader and by tests; the vectors are copied
// and sorted. The spell map is independent and keeps its rows (see SetFfxivActionSpellRows).
void SetFfxivActionDamageTables(std::vector<FfxivActionPotencyRow> potency,
                                std::vector<FfxivActionComboRow> combo);

// Replace the spell -> action map. Used by the loader and by tests; sorted by spell id.
void SetFfxivActionSpellRows(std::vector<FfxivActionSpellRow> spellRows);

// Drop every table (tests; also the state missing tables load into).
void ClearFfxivActionDamageTables();

bool HasFfxivActionPotencyRows();
bool HasFfxivActionSpellRows();

// True when the retail Action has at least one ffxiv_action_potency row (any job, any level). This
// is the data test a spell's membership in the action model is made of: it is what
// FFXIVSpellStages uses to decide whether the engine's own stages own the spell or not.
bool HasFfxivActionPotencyRowForAction(uint32 actionId);

// The lowest minLevel any ffxiv_action_potency row for the action starts at. This is the action's
// base band: the spell chain's own literal is that band's number, and a winning row with a higher
// minLevel is a per-level band the table owns (Heavy Swing 150 base, 200 at 84, 240 at 94).
uint8 FfxivActionPotencyBaseLevel(uint32 actionId);

// The retail Action id the module created `spellId` for, or 0 when this spell is not a mapped
// FFXIV action (including spellId 0). The potency and combo tables are keyed by the returned id;
// 0 means the hook must leave the cast on its existing path.
uint32 ResolveFfxivActionIdFromSpell(uint32 spellId);

// The ffxiv_action_spell row for `spellId`, or nullptr under the same rule as above.
FfxivActionSpellRow const* FindFfxivActionSpellRow(uint32 spellId);

// The f(WD) the live hook feeds the pipeline for every action cast. There is no FFXIV gear model
// yet, so this is f(WD)'s level term alone (ComputeFfxivWeaponDamagePercent(stats, 0)). The tooltip
// multiplier must use the same value or its number would not describe the damage the hook applies.
int32 ComputeFfxivActionWeaponDamagePercent(FfxivDerivedStats const& stats);

// The player's party as unique roles, the same input the damage hook and the diagnostic command
// build (the player first, then every group member; pets are not roles).
FfxivPartyComposition FfxivPartyFromGroup(::Player& player);

// --- the plain damage column, for the tooltip (GATE 7) ---------------------------------------
//
// One entry per action id that has a usable row for (classJobId, level), holding the row's *plain*
// damage column, its cure column and its DoT tick potency. Combo/flank/front/rear are contextual and
// are not part of the tooltip, so they are not sent. An entry with all three 0 is omitted. Sorted by
// action id, no duplicates. Empty when the potency table is empty -- the same condition that keeps
// every cast on its existing damage path.
struct FfxivActionPlainPotency
{
    uint32 actionId = 0;
    uint16 damage = 0;
    uint16 cure = 0;
    uint16 dot = 0;
};

std::vector<FfxivActionPlainPotency> CollectFfxivActionPlainPotencies(uint8 classJobId, uint8 level);

// The winning row for (actionId, classJobId, level) under the precedence above, or nullptr.
FfxivActionPotencyRow const* FindFfxivActionPotencyRow(uint32 actionId, uint8 classJobId, uint8 level);

// --- combo table ----------------------------------------------------------------------------

// A row (lastActionId, actionId) exists: using lastActionId then actionId is a correct combo.
bool IsFfxivComboCorrect(uint32 lastActionId, uint32 actionId);

// actionId appears as the next action of some combo row (it can continue a chain).
bool IsFfxivComboStep(uint32 actionId);

// actionId appears as the first action of some combo row (it can start a chain).
bool IsFfxivComboStarter(uint32 actionId);

// nowMs - lastActionMs <= FFXIV_COMBO_WINDOW_MS (the window is inclusive).
bool IsFfxivComboWindowOpen(uint32 lastActionMs, uint32 nowMs);

// Whether any ffxiv_action_combo row is loaded.
bool HasFfxivActionComboRows();

// The action a correct combo expects after `actionId`: the lowest nextActionId among its
// ffxiv_action_combo rows, or 0 when no row starts at `actionId`. Several rows can name different
// successors (retail's PvP combo routes); the lowest keeps the answer deterministic and the port
// models no routing. Read-only.
uint32 NextFfxivComboActionId(uint32 actionId);

// The spell the module created for `actionId` (ffxiv_action_spell), or 0 when there is no row.
// The reverse of ResolveFfxivActionIdFromSpell; both sides of the map are unique.
uint32 ResolveFfxivSpellFromActionId(uint32 actionId);

// The last action `caster` cast that can still continue a chain: recorded inside the combo window.
// 0 means the chain is broken, expired, or not active. The in-combat gate is the caller's, because
// this signature carries no Player (the damage hook reads it as `caster.IsInCombat() && ...`).
uint32 GetFfxivLastActionId(ObjectGuid caster, uint32 nowMs);

// After a cast resolved: true when `actionId`, cast by `caster` at `castMs`, was a combo step used
// with the correct predecessor. The live hook records exactly that cast as the new last action
// (and erases the state for a wrong step), so a step recorded at `castMs` is a correct one. A pure
// starter is never a step and returns false. Read-only; used by the job gauge's "Combo Bonus" rule
// (FFXIV/Gauge/FFXIVJobGauge.h), which runs once per cast after every target was resolved.
bool WasFfxivCorrectComboStep(ObjectGuid caster, uint32 actionId, uint32 castMs);

// Advance `caster`'s combo chain for one resolved cast that hit (the damage hook's own step, exposed
// so a test can drive it without a Player). Retail's rule, as Sapphire's Action.cpp states it:
//   - only a weaponskill (`category` FFXIV_ACTION_CATEGORY_WEAPONSKILL) touches the chain; a spell or
//     an ability (a damaging oGCD included) leaves it alone;
//   - a weaponskill with `preservesCombo` (Action col 37, ComboContinue) leaves it alone too;
//   - any other weaponskill advances it when it is not a combo step or is the correct step, and
//     breaks it when it is a step with the wrong (or no) predecessor.
// A weaponskill that is no step records itself, as Sapphire's setLastComboActionId does: when it
// is not a starter either, no step follows it, so the chain is over all the same.
// `lastActionId` is the predecessor read before this cast (0 when none or expired).
void AdvanceFfxivComboState(ObjectGuid caster, uint32 actionId, uint32 lastActionId, uint32 nowMs,
                            uint8 category, bool preservesCombo);

// --- selection and math ---------------------------------------------------------------------

// The sector a target-relative angle falls in. `degrees` is normalised to -180..180 first; the
// boundaries belong to the wider sector (45 degrees is flank, 135 is rear).
FfxivPositional FfxivPositionalFromAngleDegrees(int32 degrees);

// Resolve one cast to a potency column. `targetAngleDegrees` is the attacker's angle on the target's
// hitbox (0 = attacker in front of the target) and is only consulted when `hostileTarget` is true.
FfxivPotencySelection SelectFfxivActionPotency(uint32 actionId, uint8 classJobId, uint8 level,
                                               uint32 lastActionId, int32 targetAngleDegrees,
                                               bool hostileTarget);

// The published damage pipeline for a resolved selection. `weaponDamage` is f(WD) (see
// ComputeFfxivWeaponDamagePercent), not the raw weapon damage. `buffs` are the caster's active
// ffxiv_status_effect damage_done_pct rows and the target's damage_taken_pct rows, and
// `guarantees` the caster's force_crit / force_direct_hit rows: the live hook builds both from the
// auras, the same rows the client shows.
FfxivDirectDamageResult ComputeFfxivActionDamage(FfxivDerivedStats const& stats,
                                                 FfxivPotencySelection const& selection,
                                                 int32 weaponDamagePercent, uint8 targetLevel,
                                                 FfxivPartyComposition party, FfxivRollSource& rolls,
                                                 std::span<FfxivPercentFactor const> buffs = {},
                                                 FfxivCombatGuarantees guarantees = {});

// The published heal pipeline for a cure selection (same inputs; no direct-hit term inside).
FfxivHealResult ComputeFfxivActionHeal(FfxivDerivedStats const& stats,
                                       FfxivPotencySelection const& selection,
                                       int32 weaponDamagePercent, uint8 targetLevel,
                                       FfxivPartyComposition party, FfxivRollSource& rolls,
                                       std::span<FfxivPercentFactor const> buffs = {});

// The active ffxiv_status_effect rows as the pipeline's inputs: damage_done_pct on `caster` and
// damage_taken_pct on `target` become buff factors; force_crit / force_direct_hit become guarantees.
// The status apply path uses the same function for a DoT tick, so an aura and our math read one row.
void CollectFfxivStatusCombatModifiers(::Unit& caster, ::Unit& target,
                                       std::vector<FfxivPercentFactor>& buffs,
                                       FfxivCombatGuarantees& guarantees);

// --- the effect-driven cast path (the one-time spine) ---------------------------------------
//
// A converted FFXIV action carries a real WoW effect (SCHOOL_DAMAGE / WEAPON_DAMAGE / HEAL /
// APPLY_AURA / ENERGIZE) instead of the routing DUMMY. The effect handler asks these functions for
// the number; the spell's own effect row is the tuning surface and the table is the default:
//
//     explicit value (effectInfo.BasePoints, written from shared/ffxiv-hand-authored.ts)
//         || ffxiv_action_potency / ffxiv_action_dot (per job and level)
//
// `handled` says the spell is an FFXIV action at all (the same data test FFXIVSpellStages uses), so
// an unconverted spell never enters this path. `crit` is the per-target FFXIV roll and is what the
// caller records for the hit event; the engine's own crit chance is neutral (FFXIVSpellStages).
struct FfxivActionEffectResult
{
    bool handled = false;
    int64 value = 0;
    bool crit = false;
};

// The action id behind the cast, or 0 when the spell is not a mapped FFXIV action. The effect path
// uses it for the table lookups; the dummy hook uses ResolveFfxivActionIdFromSpell directly.
uint32 FfxivActionIdForSpell(SpellInfo const* spellInfo);

// Direct damage for one target of one cast. The combo/positional selection is the same one the
// DUMMY hook uses (last action, target angle); the roll seed includes the target guid, so each
// target draws its own variance and crit.
FfxivActionEffectResult ComputeFfxivActionEffectDamage(::Player& caster, ::Unit& target,
                                                       ::SpellInfo const* spellInfo,
                                                       int32 explicitPotency);

// Direct heal for one target. `explicitCure` is the effect row's BasePoints (0 = the cure column).
FfxivActionEffectResult ComputeFfxivActionEffectHeal(::Player& caster, ::Unit& target,
                                                     ::SpellInfo const* spellInfo, int32 explicitCure);

// One tick of a periodic aura the action applies itself (Bio). `explicitPotency` is the aura
// effect's BasePoints (0 = the ffxiv_action_dot row). The returned value is the tick FFXIVCombatMath
// computes now, from the caster's apply-time snapshot; the aura stores it and the engine ticks it on
// the effect's own period (the chain's DamagePeriod), not a table column.
int64 ComputeFfxivActionEffectDotTick(::Player& caster, ::Unit& target, ::SpellInfo const* spellInfo,
                                      int32 explicitPotency);

// The action's one-shot resource grant (ffxiv_action_effect kind gauge delta). For an action with no
// grant row this returns false and the caller keeps the vanilla power path. For a grant it applies
// the value to the caster's FFXIV job gauge and returns true; the spell never energizes WoW rage
// (that is the gauge's display, see FFXIV/Gauge/FFXIVJobGauge.h).
bool TryApplyFfxivActionEnergize(::Player& caster, ::SpellInfo const* spellInfo,
                                 int32 explicitAmount, int32& appliedAmount);

// The magnitude an aura the action applies itself must carry. Two sources, in order:
//   * a periodic damage aura (Bio): the tick FFXIVCombatMath computes now, from ffxiv_action_dot
//     (or the aura effect's own value when the spell row tunes one);
//   * a status-shaped aura (damage-%, max HP, move speed): the action's ffxiv_status_effect row.
// Returns false for an aura this rule does not own; `amount` is then untouched.
bool ComputeFfxivActionAuraAmount(::Player& caster, ::Unit& target, ::SpellInfo const* spellInfo,
                                  uint32 auraType, int32 explicitValue, int32& amount);

// One action effect row's value, 0 when the table does not carry it.
int32 GetFfxivActionEffectValue(uint32 actionId, uint8 kind);

// ffxiv_action_effect kind 0: the "Increases <gauge> by N" grant its ActionTransient text states.
constexpr uint8 FFXIV_ACTION_EFFECT_GAUGE_DELTA = 0;

// --- the live hook --------------------------------------------------------------------------
//
// Called from Spell::EffectDummy, the only effect handler an unconverted FFXIV action spell reaches
// (every one of them ships a single DUMMY effect). Returns true when the cast was handled here and
// the vanilla DUMMY handling must be skipped. False means "not an FFXIV action cast", which is every
// cast while the tables are empty.

bool TryApplyFfxivActionEffect(::Player& caster, ::Unit& target, ::SpellInfo const* spellInfo);

} // namespace FFXIV::Combat

#endif // TRINITYCORE_FFXIV_ACTION_DAMAGE_H
