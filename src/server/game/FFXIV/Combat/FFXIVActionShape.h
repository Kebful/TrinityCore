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

#ifndef TRINITYCORE_FFXIV_ACTION_SHAPE_H
#define TRINITYCORE_FFXIV_ACTION_SHAPE_H

#include "Define.h"
#include "Position.h"

/*
 * FFXIV action shapes (cone / line / circle), server side.
 *
 * An FFXIV action's area is not only a WoW radius: the retail Action sheet states a shape anchored on
 * the caster, the picked unit or a placed ground point (release/modules/ffxivhaven/tools/
 * generate-ffxiv-class-spells.py, section "area shape", names the columns). Two of those shapes have
 * no WoW implicit target that keeps them, a cone and a line, so the module marks them on the action's
 * one DUMMY effect and the server cuts them out of the circle that effect gathers.
 *
 * Conventions (all horizontal, the WoW x/y plane; z is ignored):
 *
 *   - the axis is the direction the shape points. A shape anchored on the caster points along the
 *     caster's facing (Position::GetOrientation(); 0 = +X, the same atan2(dy, dx) convention as
 *     Position::GetAbsoluteAngle). A shape anchored anywhere else points from the caster to that
 *     anchor, which is how a unit- or ground-anchored area travels with its anchor.
 *   - a cone is CENTRED on the axis: the unit's bearing may be up to `angle / 2` on either side. The
 *     angle is the full wedge (90 = 45 degrees each side), matching the omen fan the client draws
 *     (gl_fan090_1bf is a 90 degree fan).
 *   - a line starts at the anchor and runs along the axis: `0 <= forward <= radius` and
 *     `|lateral| <= width / 2`. Width is the full width.
 *   - a circle is `radius` around the anchor in every direction (anchor == caster is a point-blank
 *     area, anchor == the picked unit is the retail "around the target" area).
 *   - every test adds the unit's own hitbox (`unitRadius`, Unit::GetCombatReach) to the shape's
 *     radius: a unit is hit when its body overlaps the shape. The cone's angle is not widened.
 *   - an edge is inside (inclusive, with a 1e-4 float epsilon).
 *
 * Nothing here reads the database or the world: the geometry is a pure function of its arguments, so
 * tests/game/FFXIVActionShape.cpp checks it without a map, a unit or a spell.
 */

class SpellInfo;

namespace FFXIV::Combat
{
// Shape kind carried in Spell.dbc EffectMiscValue on the action's DUMMY effect. Mirrored by
// SHAPE_MARKER_CONE / SHAPE_MARKER_LINE in the module's shared/ffxiv-action-spell.ts.
enum FfxivActionShapeKind : uint8
{
    FFXIV_ACTION_SHAPE_NONE = 0,
    FFXIV_ACTION_SHAPE_CONE = 2, // the retail Action sheet CastType 3
    FFXIV_ACTION_SHAPE_LINE = 3, // the retail Action sheet CastType 4
};

struct FfxivActionShape
{
    FfxivActionShapeKind kind = FFXIV_ACTION_SHAPE_NONE;
    float radius = 0.0f; // circle radius, or cone/line length, in yalms (Action sheet col 29)
    float angle = 0.0f;  // cone only: the FULL angle in degrees (col 54 omen fan, 90 when absent)
    float width = 0.0f;  // line only: the FULL width in yalms (Action sheet col 30 XAxisModifier)
};

// True when the unit's body is inside the shape. `caster` supplies the axis origin and facing;
// `anchor` is where the shape starts (the caster for a self-centred shape).
bool IsPositionInsideFfxivActionShape(FfxivActionShape const& shape, Position const& caster,
    Position const& anchor, Position const& unit, float unitRadius);

// The shape marker of an FFXIV action spell, read off its DUMMY effect (MiscValue = kind,
// MiscValueB = the cone angle or the line width). `second` is that angle/width.
struct FfxivActionShapeMarker
{
    FfxivActionShapeKind kind = FFXIV_ACTION_SHAPE_NONE;
    float second = 0.0f;
    uint32 effectIndex = 0;
};

// Fills `marker` and returns true when the spell carries a cone or line marker; returns false (marker
// untouched) for every other spell, which keeps the core's own target selection exactly.
bool GetFfxivActionShapeMarker(SpellInfo const* spellInfo, FfxivActionShapeMarker& marker);

// The same decode for one effect's two fields, as a pure function (tests/game/FFXIVActionShape.cpp):
// `miscValue` is Spell.dbc EffectMiscValue, `miscValueB` EffectMiscValueB. Only the two kinds that
// need geometry are accepted; every other pair means "no shape".
bool DecodeFfxivActionShapeMarker(int32 miscValue, int32 miscValueB, FfxivActionShapeMarker& marker);
}

#endif // TRINITYCORE_FFXIV_ACTION_SHAPE_H
