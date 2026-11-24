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

#include "FFXIVActionShape.h"
#include "SpellInfo.h"

#include <algorithm>
#include <cmath>

/*
 * The retail semantics this file implements were read from the Sapphire reference
 * (/tmp/ffxivhaven-sapphire-research, commit aab9cb23) as a SPEC only; the code below is ours:
 *
 *   - src/world/Util/ActorFilter.cpp ActorFilterCone: a cone is the angular difference between the
 *     unit's bearing and the shape's axis, with an inclusive bound at each edge, and the wrap-around
 *     band fixed into -pi..pi. Our axis is the caster's facing (or caster -> anchor); Sapphire's is
 *     always caster -> picked target, which is the same ray whenever the player faces the target.
 *   - src/world/Util/ActorFilter.cpp ActorFilterInRange: a circle is `distance(center, unit) <=
 *     radius`, with the unit's own radius added to the radius (not to the angle).
 *   - src/world/Util/ActorFilter.cpp ActorFilterBox is NOT a model: it tests the unit against a
 *     world-axis box from the area position, ignoring the caster's facing, so it is not a line.
 *   - src/world/Action/ActionShapeLut.h + data/actionShapes/cone.json: the only cone data Sapphire
 *     ships is a (start, end) pair of degrees keyed by action id: 106 Quick Nock and 453 Incinerate
 *     are both -45..45, i.e. a 90 degree wedge. That is the default our generator uses for a player
 *     cone, whose retail row carries no angle (see the module generator's "area shape" section).
 */

namespace FFXIV::Combat
{
namespace
{
// Float slack for the inclusive edges: a yalm is ~1 unit, so this is far below any game distance.
constexpr float FFXIV_SHAPE_EPSILON = 1e-4f;

// The bearing of `unit` seen from `origin`, in radians, in Position's own convention.
float BearingTo(Position const& origin, Position const& unit)
{
    return origin.GetAbsoluteAngle(unit);
}

// The direction a shape anchored at `anchor` points: the caster's facing when the shape starts on the
// caster, the caster -> anchor ray otherwise.
float ShapeAxis(Position const& caster, Position const& anchor)
{
    float const dx = anchor.GetPositionX() - caster.GetPositionX();
    float const dy = anchor.GetPositionY() - caster.GetPositionY();
    if (std::fabs(dx) <= FFXIV_SHAPE_EPSILON && std::fabs(dy) <= FFXIV_SHAPE_EPSILON)
        return caster.GetOrientation();
    return caster.GetAbsoluteAngle(anchor);
}

// The signed bearing of `unit` relative to `axis`, in -pi..pi.
float RelativeBearing(float axis, float bearing)
{
    float relative = Position::NormalizeOrientation(bearing - axis);
    if (relative > float(M_PI))
        relative -= 2.0f * float(M_PI);
    return relative;
}
}

bool IsPositionInsideFfxivActionShape(FfxivActionShape const& shape, Position const& caster,
    Position const& anchor, Position const& unit, float unitRadius)
{
    if (shape.radius <= 0.0f)
        return false;

    // A unit whose body overlaps the shape is hit, so the body radius extends the reach; it never
    // widens the cone (Sapphire's ActorFilterInRange adds the same radius to the circle).
    float const reach = shape.radius + std::max(unitRadius, 0.0f);
    float const distance = anchor.GetExactDist2d(&unit);

    switch (shape.kind)
    {
        case FFXIV_ACTION_SHAPE_CONE:
        {
            if (distance > reach)
                return false;
            // A zero angle is a shape the sheet never states (Sapphire treats an all-zero cone entry
            // as "no data"); do not accept a degenerate wedge.
            if (shape.angle <= 0.0f)
                return false;
            float const halfAngle = shape.angle * float(M_PI) / 360.0f;
            return std::fabs(RelativeBearing(ShapeAxis(caster, anchor), BearingTo(caster, unit)))
                <= halfAngle + FFXIV_SHAPE_EPSILON;
        }
        case FFXIV_ACTION_SHAPE_LINE:
        {
            if (shape.width <= 0.0f)
                return false;
            // Work in the shape's frame: forward along the axis from the anchor, lateral across it.
            float const axis = ShapeAxis(caster, anchor);
            float const dx = unit.GetPositionX() - anchor.GetPositionX();
            float const dy = unit.GetPositionY() - anchor.GetPositionY();
            float const forward = dx * std::cos(axis) + dy * std::sin(axis);
            float const lateral = -dx * std::sin(axis) + dy * std::cos(axis);
            float const halfWidth = shape.width / 2.0f;
            return forward >= -FFXIV_SHAPE_EPSILON
                && forward <= reach + FFXIV_SHAPE_EPSILON
                && std::fabs(lateral) <= halfWidth + std::max(unitRadius, 0.0f) + FFXIV_SHAPE_EPSILON;
        }
        case FFXIV_ACTION_SHAPE_NONE:
        default:
            // A plain circle, which is also what a spell without a marker keeps in the core.
            return distance <= reach + FFXIV_SHAPE_EPSILON;
    }
}

bool DecodeFfxivActionShapeMarker(int32 miscValue, int32 miscValueB, FfxivActionShapeMarker& marker)
{
    // Only the two kinds that need geometry are marked; a circle is already a radius, so the module
    // leaves MiscValue at 0 on those rows.
    if (miscValue != FFXIV_ACTION_SHAPE_CONE && miscValue != FFXIV_ACTION_SHAPE_LINE)
        return false;
    marker.kind = static_cast<FfxivActionShapeKind>(miscValue);
    marker.second = float(miscValueB);
    return true;
}

bool GetFfxivActionShapeMarker(SpellInfo const* spellInfo, FfxivActionShapeMarker& marker)
{
    if (!spellInfo)
        return false;

    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        SpellEffectInfo const& effectInfo = spellInfo->GetEffect(static_cast<SpellEffIndex>(i));
        if (!effectInfo.IsEffect())
            continue;
        FfxivActionShapeMarker decoded;
        if (DecodeFfxivActionShapeMarker(effectInfo.MiscValue, effectInfo.MiscValueB, decoded))
        {
            decoded.effectIndex = i;
            marker = decoded;
            return true;
        }
    }
    return false;
}
}
