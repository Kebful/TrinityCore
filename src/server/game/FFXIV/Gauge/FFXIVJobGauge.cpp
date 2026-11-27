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
 * FFXIV job gauge: the engine rules only -- the rage suppression/waiver and the once-per-cast
 * script event. The value, the deltas, the mirror and the 0x7A62 packet are the livescript's
 * (livescripts/job-resources/jobgauge.ts); see FFXIVJobGauge.h for the wire specification.
 */

#include "FFXIVJobGauge.h"

#include "GameTime.h"
#include "Player.h"
#include "SpellInfo.h"
#include "FFXIV/Combat/FFXIVActionDamage.h"
#include "FFXIV/Stats/FFXIVStatTables.h"
#include "TSEvent.h"
#include "TSEvents.h"

namespace FFXIV::Gauge
{
bool IsFfxivRageOwner(Unit const& unit)
{
    Player const* player = unit.ToPlayer();
    if (!player)
        return false;
    // Only the module's ChrClasses rows. A stock WoW class keeps its rage untouched.
    return FFXIV::Stats::FindFfxivClassJobMapRow(player->GetClass()) != nullptr;
}

bool SuppressesWowRage(Unit const& unit)
{
    return IsFfxivRageOwner(unit);
}

bool IsFfxivActionRageCostWaived(Player const& player)
{
    // Rage is the gauge's display, not a second resource: the script is the only cost authority
    // (Spell.OnCheckCast) and the only writer of POWER_RAGE. For a class whose display power is not
    // rage the mirror pins it to 0, so charging a rage cost would refuse every spender.
    return IsFfxivRageOwner(player);
}

void OnFfxivActionResolved(Player& caster, SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return;

    uint32 actionId = Combat::ResolveFfxivActionIdFromSpell(spellInfo->Id);
    if (actionId == 0)
        return;

    // The damage hook ran for every target of this cast in this same tick, so a correct combo step
    // is recorded at this GameTime (FFXIVActionDamage.h WasFfxivCorrectComboStep).
    bool const comboCorrect =
        Combat::WasFfxivCorrectComboStep(caster.GetGUID(), actionId, GameTime::GetGameTimeMS());

    FIRE_ID(spellInfo->events.id, Spell, OnFfxivActionResolved, TSPlayer(&caster), TSSpellInfo(spellInfo),
            TSNumber<uint32>(actionId), comboCorrect);
}
} // namespace FFXIV::Gauge
