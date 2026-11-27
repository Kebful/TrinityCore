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

#include "FFXIVSpellStages.h"

#include "FFXIV/Combat/FFXIVActionDamage.h"
#include "SpellInfo.h"

namespace FFXIV::Combat
{
namespace
{
// SPELL_MISS_NONE. This file does not include SharedDefines.h; the values are the wire/AI contract.
// NONE 0, MISS 1, RESIST 2, DODGE 3, PARRY 4, BLOCK 5, EVADE 6, IMMUNE 7/8, DEFLECT 9, ABSORB 10,
// REFLECT 11.
constexpr uint8 FFXIV_SPELL_MISS_NONE = 0;
} // namespace

bool IsFfxivActionSpell(uint32 spellId)
{
    if (spellId == 0)
        return false;

    uint32 const actionId = ResolveFfxivActionIdFromSpell(spellId);
    return actionId != 0 && HasFfxivActionPotencyRowForAction(actionId);
}

bool IsFfxivActionSpell(SpellInfo const* spellInfo)
{
    return spellInfo && IsFfxivActionSpell(spellInfo->Id);
}

bool FfxivActionOwnsCombatStages(SpellInfo const* spellInfo)
{
    return IsFfxivActionSpell(spellInfo);
}

void NeutraliseFfxivAvoidance(uint32 spellId, uint8& miss)
{
    if (miss == FFXIV_SPELL_MISS_NONE || !IsFfxivActionSpell(spellId))
        return;

    // The six rolls the engine makes that an FFXIV action must not: miss, resist, dodge, parry,
    // block, deflect. Evade, immunity and reflect are not rolls and stay -- a spell that cannot land
    // must still say so.
    switch (miss)
    {
        case 1: // SPELL_MISS_MISS
        case 2: // SPELL_MISS_RESIST
        case 3: // SPELL_MISS_DODGE
        case 4: // SPELL_MISS_PARRY
        case 5: // SPELL_MISS_BLOCK
        case 9: // SPELL_MISS_DEFLECT
            miss = FFXIV_SPELL_MISS_NONE;
            break;
        default:
            break;
    }
}

void NeutraliseFfxivCritChance(uint32 spellId, float& critChance)
{
    if (IsFfxivActionSpell(spellId))
        critChance = 0.0f;
}

uint64 FfxivActionRollSeed(uint64 casterGuid, uint32 actionId, uint64 targetGuid, uint32 nowMs)
{
    // The same mix the live hook used, with the target's guid added between the action and the time:
    // two targets of one cast therefore draw different variance and crit, while a repeat of the same
    // (caster, action, target, ms) reproduces exactly -- which is what a fixture pins.
    return casterGuid * 0x9E3779B97F4A7C15ull
         + uint64(actionId) * 0xBF58476D1CE4E5B9ull
         + targetGuid * 0x94D049BB133111EBull
         + uint64(nowMs);
}
} // namespace FFXIV::Combat
