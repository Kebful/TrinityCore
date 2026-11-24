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
 * FFXIV action shapes (FFXIV/Combat/FFXIVActionShape.h): pure geometry, no map, unit or spell.
 *
 * The shapes and their raw sheet values are the ones the generator documents:
 *   cone  Quick Nock 106: CastType 3, EffectRange 12, XAxisModifier 0  -> 12 yalms, 90 degrees
 *   line  Doom Spike 86:  CastType 4, EffectRange 10, XAxisModifier 4  -> 10 long, 4 wide
 *   circle Holy 139:      CastType 2, EffectRange 8                    -> 8 yalms around the caster
 *
 * Coordinates are the WoW x/y plane and the orientation is Position's own (0 = +X, atan2(dy, dx)),
 * so every expectation below is plain arithmetic on the world position. Edges are inclusive.
 */

#include "tc_catch2.h"

#include "FFXIV/Combat/FFXIVActionShape.h"

#include <cmath>

using namespace FFXIV::Combat;

namespace
{
// A caster at the origin facing +X (Position's 0 orientation).
Position const CASTER = Position(0.0f, 0.0f, 0.0f, 0.0f);

FfxivActionShape Cone(float radius, float angle)
{
    FfxivActionShape shape;
    shape.kind = FFXIV_ACTION_SHAPE_CONE;
    shape.radius = radius;
    shape.angle = angle;
    return shape;
}

FfxivActionShape Line(float length, float width)
{
    FfxivActionShape shape;
    shape.kind = FFXIV_ACTION_SHAPE_LINE;
    shape.radius = length;
    shape.width = width;
    return shape;
}

FfxivActionShape Circle(float radius)
{
    FfxivActionShape shape;
    shape.kind = FFXIV_ACTION_SHAPE_NONE;
    shape.radius = radius;
    return shape;
}

// A unit at (x, y): no facing, no hitbox unless the test passes one.
Position At(float x, float y)
{
    return Position(x, y, 0.0f, 0.0f);
}
}

TEST_CASE("FFXIV action shape: cone is centred on the caster's facing", "[ffxiv]")
{
    FfxivActionShape const quickNock = Cone(12.0f, 90.0f);
    float constexpr NO_BODY = 0.0f;

    // Dead ahead is the centre line.
    CHECK(IsPositionInsideFfxivActionShape(quickNock, CASTER, CASTER, At(10.0f, 0.0f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(quickNock, CASTER, CASTER, At(-10.0f, 0.0f), NO_BODY));

    // 90 degrees is +/-45 degrees around the facing, so the edges sit at 45 degrees off it. Points at
    // 45 degrees are inside; (7.0, 7.3) and its mirror are 46.2 degrees off, still within 12 yalms.
    CHECK(IsPositionInsideFfxivActionShape(quickNock, CASTER, CASTER, At(7.0f, 7.0f), NO_BODY));
    CHECK(IsPositionInsideFfxivActionShape(quickNock, CASTER, CASTER, At(7.0f, -7.0f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(quickNock, CASTER, CASTER, At(7.0f, 7.3f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(quickNock, CASTER, CASTER, At(7.0f, -7.3f), NO_BODY));

    // The length is the cone's radius: 12 yalms, the retail EffectRange.
    CHECK(IsPositionInsideFfxivActionShape(quickNock, CASTER, CASTER, At(12.0f, 0.0f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(quickNock, CASTER, CASTER, At(12.1f, 0.0f), NO_BODY));

    // A unit's body is hit when it overlaps: 12.5 yalms away is outside a bare shape but inside when
    // the unit is 0.6 yalms across.
    CHECK_FALSE(IsPositionInsideFfxivActionShape(quickNock, CASTER, CASTER, At(12.5f, 0.0f), NO_BODY));
    CHECK(IsPositionInsideFfxivActionShape(quickNock, CASTER, CASTER, At(12.5f, 0.0f), 0.6f));

    // Rotating the caster rotates the cone: facing +Y (pi/2) puts (0, 10) inside and (10, 0) outside.
    Position const facingNorth = Position(0.0f, 0.0f, 0.0f, float(M_PI) / 2.0f);
    CHECK(IsPositionInsideFfxivActionShape(quickNock, facingNorth, facingNorth, At(0.0f, 10.0f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(quickNock, facingNorth, facingNorth, At(10.0f, 0.0f), NO_BODY));

    // A 120 degree cone (the omen fan gl_fan120_1bf) is wider than the 90 degree one: 46.2 degrees is
    // inside it, 61.2 degrees is not.
    FfxivActionShape const wide = Cone(12.0f, 120.0f);
    CHECK(IsPositionInsideFfxivActionShape(wide, CASTER, CASTER, At(7.0f, 7.3f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(wide, CASTER, CASTER, At(5.0f, 9.1f), NO_BODY));
}

TEST_CASE("FFXIV action shape: line length and width", "[ffxiv]")
{
    FfxivActionShape const doomSpike = Line(10.0f, 4.0f);
    float constexpr NO_BODY = 0.0f;

    // 10 yalms long, 4 wide: +/-2 yalms from the axis.
    CHECK(IsPositionInsideFfxivActionShape(doomSpike, CASTER, CASTER, At(5.0f, 0.0f), NO_BODY));
    CHECK(IsPositionInsideFfxivActionShape(doomSpike, CASTER, CASTER, At(5.0f, 1.9f), NO_BODY));
    CHECK(IsPositionInsideFfxivActionShape(doomSpike, CASTER, CASTER, At(5.0f, -1.9f), NO_BODY));
    CHECK(IsPositionInsideFfxivActionShape(doomSpike, CASTER, CASTER, At(5.0f, 2.0f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(doomSpike, CASTER, CASTER, At(5.0f, 2.1f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(doomSpike, CASTER, CASTER, At(5.0f, -2.1f), NO_BODY));

    // The length starts at the caster, not behind it.
    CHECK(IsPositionInsideFfxivActionShape(doomSpike, CASTER, CASTER, At(0.0f, 0.0f), NO_BODY));
    CHECK(IsPositionInsideFfxivActionShape(doomSpike, CASTER, CASTER, At(10.0f, 0.0f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(doomSpike, CASTER, CASTER, At(10.1f, 0.0f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(doomSpike, CASTER, CASTER, At(-0.1f, 0.0f), NO_BODY));

    // The unit's body extends the width but not the length.
    CHECK_FALSE(IsPositionInsideFfxivActionShape(doomSpike, CASTER, CASTER, At(5.0f, 2.5f), NO_BODY));
    CHECK(IsPositionInsideFfxivActionShape(doomSpike, CASTER, CASTER, At(5.0f, 2.5f), 0.6f));

    // Facing +Y runs the same rectangle along the other axis.
    Position const facingWest = Position(0.0f, 0.0f, 0.0f, float(M_PI));
    CHECK(IsPositionInsideFfxivActionShape(doomSpike, facingWest, facingWest, At(-5.0f, 0.0f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(doomSpike, facingWest, facingWest, At(5.0f, 0.0f), NO_BODY));
}

TEST_CASE("FFXIV action shape: circle", "[ffxiv]")
{
    FfxivActionShape const holy = Circle(8.0f);
    float constexpr NO_BODY = 0.0f;

    CHECK(IsPositionInsideFfxivActionShape(holy, CASTER, CASTER, At(7.9f, 0.0f), NO_BODY));
    CHECK(IsPositionInsideFfxivActionShape(holy, CASTER, CASTER, At(0.0f, -7.9f), NO_BODY));
    CHECK(IsPositionInsideFfxivActionShape(holy, CASTER, CASTER, At(5.6f, 5.6f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(holy, CASTER, CASTER, At(8.1f, 0.0f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(holy, CASTER, CASTER, At(5.7f, 5.7f), NO_BODY));

    // The facing plays no part in a circle.
    Position const facingWest = Position(0.0f, 0.0f, 0.0f, float(M_PI));
    CHECK(IsPositionInsideFfxivActionShape(holy, facingWest, CASTER, At(7.9f, 0.0f), NO_BODY));
}

TEST_CASE("FFXIV action shape: an anchor at the target", "[ffxiv]")
{
    // Two yalms ahead of the caster is the picked enemy; the shape travels with it. The axis is the
    // caster -> anchor ray, which is the same ray the caster faces in this case.
    Position const target = At(10.0f, 0.0f);
    FfxivActionShape const cone = Cone(6.0f, 90.0f);
    float constexpr NO_BODY = 0.0f;
    CHECK(IsPositionInsideFfxivActionShape(cone, CASTER, target, At(13.0f, 0.0f), NO_BODY));
    CHECK(IsPositionInsideFfxivActionShape(cone, CASTER, target, At(15.0f, 3.0f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(cone, CASTER, target, At(16.1f, 0.0f), NO_BODY));

    // A target behind the caster points the shape backwards, whatever way the caster faces.
    Position const behind = At(-10.0f, 0.0f);
    CHECK(IsPositionInsideFfxivActionShape(cone, CASTER, behind, At(-13.0f, 0.0f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(cone, CASTER, behind, At(13.0f, 0.0f), NO_BODY));

    // A unit-anchored circle is the retail "around the picked target" area (Cure III): the picked unit
    // is inside, the caster 30 yalms away is not.
    FfxivActionShape const aroundTarget = Circle(8.0f);
    Position const farTarget = At(30.0f, 0.0f);
    CHECK(IsPositionInsideFfxivActionShape(aroundTarget, CASTER, farTarget, At(37.0f, 0.0f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(aroundTarget, CASTER, farTarget, At(39.0f, 0.0f), NO_BODY));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(aroundTarget, CASTER, farTarget, CASTER, NO_BODY));
}

TEST_CASE("FFXIV action shape: a shape that contains no units", "[ffxiv]")
{
    FfxivActionShape const cone = Cone(10.0f, 90.0f);
    FfxivActionShape const line = Line(10.0f, 4.0f);
    FfxivActionShape const circle = Circle(10.0f);

    // Every unit is behind the caster, so the forward cone and the line are empty while the circle is
    // not: the geometry really does reject, it is not just always false.
    int insideCone = 0;
    int insideLine = 0;
    int insideCircle = 0;
    for (int x = -20; x <= -1; ++x)
    {
        for (int y = -20; y <= 20; ++y)
        {
            Position const unit = At(float(x), float(y));
            if (IsPositionInsideFfxivActionShape(cone, CASTER, CASTER, unit, 0.0f))
                ++insideCone;
            if (IsPositionInsideFfxivActionShape(line, CASTER, CASTER, unit, 0.0f))
                ++insideLine;
            if (IsPositionInsideFfxivActionShape(circle, CASTER, CASTER, unit, 0.0f))
                ++insideCircle;
        }
    }
    CHECK(insideCone == 0);
    CHECK(insideLine == 0);
    CHECK(insideCircle > 0);

    // A zero radius, a zero cone angle and a zero line width are shapes with no area.
    FfxivActionShape const noRadius = Circle(0.0f);
    FfxivActionShape const noAngle = Cone(10.0f, 0.0f);
    FfxivActionShape const noWidth = Line(10.0f, 0.0f);
    CHECK_FALSE(IsPositionInsideFfxivActionShape(noRadius, CASTER, CASTER, CASTER, 0.0f));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(noAngle, CASTER, CASTER, At(5.0f, 0.0f), 0.0f));
    CHECK_FALSE(IsPositionInsideFfxivActionShape(noWidth, CASTER, CASTER, At(5.0f, 0.0f), 0.0f));
}

TEST_CASE("FFXIV action shape: the effect marker decodes only cone and line", "[ffxiv]")
{
    // The module writes the marker into EffectMiscValue / EffectMiscValueB of the action's DUMMY
    // effect (shared/ffxiv-action-spell.ts applyShape): 2 = cone, 3 = line, anything else = no shape.
    FfxivActionShapeMarker marker;
    REQUIRE(DecodeFfxivActionShapeMarker(FFXIV_ACTION_SHAPE_CONE, 90, marker));
    CHECK(marker.kind == FFXIV_ACTION_SHAPE_CONE);
    CHECK(marker.second == 90.0f);

    REQUIRE(DecodeFfxivActionShapeMarker(FFXIV_ACTION_SHAPE_LINE, 4, marker));
    CHECK(marker.kind == FFXIV_ACTION_SHAPE_LINE);
    CHECK(marker.second == 4.0f);

    // A circle (0), a stock WotLK DUMMY effect (0/0) and an unrelated misc value keep no shape.
    CHECK_FALSE(DecodeFfxivActionShapeMarker(0, 90, marker));
    CHECK_FALSE(DecodeFfxivActionShapeMarker(0, 0, marker));
    CHECK_FALSE(DecodeFfxivActionShapeMarker(7, 90, marker));

    // No SpellInfo (every non-spell caller) means no shape.
    CHECK_FALSE(GetFfxivActionShapeMarker(nullptr, marker));
}
