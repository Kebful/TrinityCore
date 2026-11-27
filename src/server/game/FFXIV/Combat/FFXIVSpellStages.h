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

#ifndef TRINITYCORE_FFXIV_SPELL_STAGES_H
#define TRINITYCORE_FFXIV_SPELL_STAGES_H

#include "Define.h"

class SpellInfo;

/*
 * THE ONE PLACE where WoW's global damage/heal stages are switched off for an FFXIV action spell.
 *
 * An FFXIV action's number comes from FFXIVCombatMath (potency/stat/trait/crit/DH/variance), so the
 * engine stages that would otherwise scale or roll it a second time must be identity for exactly
 * those spells:
 *
 *   stage                                   replaced by
 *   --------------------------------------  ------------------------------------------------
 *   SpellDamageBonusDone / Taken            FFXIVCombatMath f(ATK)/f(DET)/f(TNC)/f(DOT)/f(WD)
 *   SpellHealingBonusDone / Taken           FFXIVCombatMath f(HMP)
 *   armour reduction                        none -- FFXIV has no WoW armour stage
 *   partial resist                          none -- never rolled
 *   block / dodge / parry / miss            none -- the spell carries IGNORE_HIT_RESULT and the
 *                                           AddUnitTarget stage is cleared here too
 *   crit chance / crit damage / heal crit   FFXIVCombatMath's per-target crit roll
 *
 * Selection is membership in the data: the spell has an ffxiv_action_spell row AND its Action has at
 * least one ffxiv_action_potency row. There is no id list here and there never should be; a spell
 * leaves these stages the moment its data does.
 *
 * Hot path: two binary searches over already-loaded, sorted vectors; no allocation, no logging.
 * A missing/empty table makes every predicate false, i.e. the engine is exactly as it was.
 *
 * Each guard site is a one-line `if (FFXIV::Combat::FfxivActionOwnsCombatStages(...))` in Unit.cpp /
 * Spell.cpp, so the audit is "grep for FFXIV::Combat::" in those two files. ENGINE-DEVIATIONS.md at
 * the core root lists every guard, the stage it replaces, and how to tell a deliberate change from a
 * bug.
 */
namespace FFXIV::Combat
{
// True when `spellId` is an FFXIV action spell by the data test above. Safe for 0.
bool IsFfxivActionSpell(uint32 spellId);

// SpellInfo overload; false for null.
bool IsFfxivActionSpell(SpellInfo const* spellInfo);

// The predicate every Unit/Spell stage guard reads. Same answer as IsFfxivActionSpell; named
// separately so the guard sites read as a rule, not as a spell lookup.
bool FfxivActionOwnsCombatStages(SpellInfo const* spellInfo);

// Avoidance: an FFXIV action never misses/dodges/parries/blocks/partially resists. `miss` is the
// value Spell::AddUnitTarget computed; unchanged for a non-FFXIV spell.
void NeutraliseFfxivAvoidance(uint32 spellId, uint8& miss);

// Crit: the engine's crit chance is zero for an FFXIV action; the per-target roll made by
// FFXIVCombatMath decides (`critChance` is the engine's chance before this call).
void NeutraliseFfxivCritChance(uint32 spellId, float& critChance);

// The roll seed for one target of one cast. FFXIVCombatMath's variance and crit are rolled per
// target; a cast-wide seed would make an area action crit or not as a block. Pure so a test can pin
// two targets apart.
uint64 FfxivActionRollSeed(uint64 casterGuid, uint32 actionId, uint64 targetGuid, uint32 nowMs);
} // namespace FFXIV::Combat

#endif // TRINITYCORE_FFXIV_SPELL_STAGES_H
