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

#ifndef TRINITYCORE_FFXIV_CLASS_MANA_H
#define TRINITYCORE_FFXIV_CLASS_MANA_H

#include "Define.h"
#include <vector>

class Player;

/*
 * FFXIV MP on WoW mana (owner decision, 2026-11-25).
 *
 * Data
 * ----
 * One world table, written by release/modules/ffxivhaven/datascripts/classes/ffxiv-class-resources.ts:
 *
 *   ffxiv_class_mana(classId, tickMs, inCombatPerTick, outOfCombatPerTick)
 *
 * A row means "this class's mana is FFXIV MP". The pool itself is not here: it is the class's
 * player_classlevelstats.basemana (ParamGrow col 4, 10000 at every level), i.e. GetCreateMana().
 * No action, spell or ClassJob id appears in this code; an empty or missing table changes nothing.
 *
 * What a row changes (only for a player whose class has one)
 * --------------------------------------------------------
 *   - Player::UpdateMaxPower(POWER_MANA) adds no GetManaBonusFromIntellect(): the maximum is the
 *     base mana (plus any mana aura mods, which no FFXIV status uses today).
 *   - Player::UpdatePowerRegen(POWER_MANA) writes the row's rate, in MP per second, into BOTH
 *     UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER and ..._INTERRUPTED_FLAT_MODIFIER, plus any
 *     SPELL_AURA_MOD_POWER_REGEN (mana) amount / 5. Spirit, Intellect, the five-second rule, the
 *     level < 15 bonus and Rate.Mana do not apply. The server's Player::Regenerate and the client's
 *     CGUnit_C::DoPowerRegen both read these two fields, so the bar's prediction and the server agree;
 *     with both fields equal, the five-second rule cannot make them disagree.
 *   - Player::AtEnterCombat / AtExitCombat re-run UpdatePowerRegen(POWER_MANA), so the in-combat or
 *     out-of-combat rate is live.
 *   - SPELL_AURA_PREVENT_REGENERATE_POWER still wins (checked first, as for every class).
 *
 * The cost side needs no core code: an MP action's Spell.dbc PowerType is MANA and its ManaCost the
 * sheet's cost, so Spell::CheckPower refuses a cast the player cannot pay (SPELL_FAILED_NO_POWER).
 */

namespace FFXIV::Mana
{
// One ffxiv_class_mana row.
struct FfxivClassManaRow
{
    uint8 classId = 0;
    uint16 tickMs = 0;
    uint16 inCombatPerTick = 0;
    uint16 outOfCombatPerTick = 0;
};

// Reads ffxiv_class_mana. A missing table leaves every class on WoW's mana rules.
void LoadFfxivClassManaTable();
// Test/reload seam: replaces the loaded rows. Rows with tickMs 0 are dropped.
void SetFfxivClassManaRows(std::vector<FfxivClassManaRow> rows);
void ClearFfxivClassManaRows();

// The row for a ChrClasses id, or nullptr (stock mana rules).
FfxivClassManaRow const* GetFfxivClassManaRow(uint8 classId);
// The row for the player's FFXIV job: a hosted job's own row (ffxiv_class_mana keyed by the job id,
// e.g. Dark Knight 32), else their class's row. FFXIV/Jobs/FFXIVJobSlot.h.
FfxivClassManaRow const* GetFfxivManaRow(Player const& player);
bool HasFfxivMana(Player const& player);

// MP per second for one row: perTick * 1000 / tickMs. Pure.
float FfxivManaRegenPerSecond(FfxivClassManaRow const& row, bool inCombat);
}

#endif
