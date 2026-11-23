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

#ifndef TRINITYCORE_FFXIV_ACTION_POTENCY_H
#define TRINITYCORE_FFXIV_ACTION_POTENCY_H

#include "Define.h"

/*
 * FFXIV action potency, server side (data only).
 *
 * Retail damage and healing are data, not a formula: the client computes neither, and every number
 * it prints comes from the action's ActionTransient description text
 * (work/stat-research/I-REFRESH-20261123.md §1.4). This header exposes that text as numbers for the
 * shipped action set, resolved for one (ClassJob, level):
 *
 *     Stone 140 at every level, Cure 450 / 500 from level 85 as White Mage, Medica 300 / 400,
 *     Fast Blade 150 / 200 / 220 by job and level, and so on.
 *
 * The data lives in FFXIVActionPotency.inc, which is GENERATED from the shipped sheet by
 * release/modules/ffxivhaven/tools/generate-ffxiv-action-potency.py (the same decode that produces
 * the module's TypeScript table; the generator asserts the two agree row for row). Never edit it by
 * hand.
 *
 * Scope and limits:
 *   - nothing consumes this yet: no Spell.dbc/PointsBase row, no combat path and no command reads
 *     it, so adding it cannot change gameplay;
 *   - `damage`/`cure` are the two slots the sheet states for the action itself. Damage over time
 *     (Aero's "Potency: 30", Thunder's 45) and the positional/combo/"increase" variants are not
 *     (ClassJob, level) and are not resolved here;
 *   - the branch conditions are the client's gnum68/gnum72 tests read as ClassJob/level, which is
 *     INFERRED from the values and the unlock/trait gates, not proven from the parameter binding
 *     (POTENCY-EXTRACTOR-20261123.md §3.1, §7.1). If that binding is ever proven different, the
 *     numbers stay and only the parameter names change.
 *
 * Units: potency, exactly as the tooltip prints it. A conversion to points, a coefficient or a
 * level scalar is a later slice's choice and is deliberately absent here.
 */

namespace FFXIV::Combat
{
// How the action's own potency was stated in the sheet. Mirrors the module table's `kind`.
enum FfxivActionPotencyKind : uint8
{
    FFXIV_POTENCY_KIND_NONE        = 0, // the description states no potency (correctly non-damaging)
    FFXIV_POTENCY_KIND_DAMAGE      = 1, // the action's own potency is a plain number
    FFXIV_POTENCY_KIND_HEAL        = 2, // the action's own potency is a Cure Potency
    FFXIV_POTENCY_KIND_CONDITIONAL = 3, // the own potency is an If tree, or only a pet/combo number
};

struct FfxivActionPotency
{
    uint16 damage; // 0 when the sheet states no damage potency for this (action, job, level)
    uint16 cure;   // 0 when the sheet states no Cure Potency for this (action, job, level)
    uint8 kind;    // FfxivActionPotencyKind
};

// True when the action is in the table (the 651 Action rows the module ships as spells plus the nine
// hand-authored starter actions).
bool HasFfxivActionPotency(uint32 actionId);

// The action's potency for one ClassJob and level. Unknown action ids return
// { 0, 0, FFXIV_POTENCY_KIND_NONE }. A conditional slot that no branch matches resolves to 0; the
// sheet has no value in that case, and no default is invented for it.
FfxivActionPotency GetFfxivActionPotency(uint32 actionId, uint8 classJobId, uint8 level);
}

#endif
