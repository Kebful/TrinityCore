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
 * FFXIV MP on WoW mana, the table side: the rows are the ones
 * datascripts/classes/ffxiv-class-resources.ts writes (one per MP class, 3000 ms tick, 200 MP per tick
 * in and out of combat). Class ids are the module's ChrClasses ids; the code reads none of them.
 */

#include "tc_catch2.h"

#include "FFXIV/Mana/FFXIVClassMana.h"

using namespace FFXIV::Mana;

namespace
{
constexpr uint8 CONJURER = 18;
constexpr uint8 WHITE_MAGE = 27;
constexpr uint8 BLACK_MAGE = 28;
constexpr uint8 WARRIOR = 22;

std::vector<FfxivClassManaRow> Rows()
{
    return {
        { WHITE_MAGE, 3000, 200, 200 },
        { CONJURER, 3000, 200, 200 },
        { BLACK_MAGE, 3000, 0, 600 },
    };
}
}

TEST_CASE("FFXIV class mana: a row marks an MP class and sets its regen rate", "[ffxiv][mana]")
{
    SetFfxivClassManaRows(Rows());

    SECTION("lookup by class; a class without a row keeps WoW's mana rules")
    {
        REQUIRE(GetFfxivClassManaRow(CONJURER) != nullptr);
        REQUIRE(GetFfxivClassManaRow(WHITE_MAGE) != nullptr);
        REQUIRE(GetFfxivClassManaRow(WHITE_MAGE)->classId == WHITE_MAGE);
        REQUIRE(GetFfxivClassManaRow(WARRIOR) == nullptr);
        REQUIRE(GetFfxivClassManaRow(0) == nullptr);
    }

    SECTION("rate is per tick over the tick length: 200 MP / 3 s = 66.67 MP/s, 2 % of 10000 per tick")
    {
        FfxivClassManaRow const& whm = *GetFfxivClassManaRow(WHITE_MAGE);
        REQUIRE(FfxivManaRegenPerSecond(whm, false) == Approx(200.f / 3.f));
        REQUIRE(FfxivManaRegenPerSecond(whm, true) == Approx(200.f / 3.f));
        // One 3 s tick, the way Player::Regenerate accrues it (rate * 0.001 * elapsed ms).
        REQUIRE(FfxivManaRegenPerSecond(whm, true) * 0.001f * 3000.f == Approx(200.f));
    }

    SECTION("in-combat and out-of-combat rates are separate columns")
    {
        FfxivClassManaRow const& blm = *GetFfxivClassManaRow(BLACK_MAGE);
        REQUIRE(FfxivManaRegenPerSecond(blm, true) == 0.f);
        REQUIRE(FfxivManaRegenPerSecond(blm, false) == Approx(200.f));
    }

    SECTION("a zero tick is dropped, never divided by")
    {
        SetFfxivClassManaRows({ { WHITE_MAGE, 0, 200, 200 } });
        REQUIRE(GetFfxivClassManaRow(WHITE_MAGE) == nullptr);
        REQUIRE(FfxivManaRegenPerSecond(FfxivClassManaRow{ WHITE_MAGE, 0, 200, 200 }, false) == 0.f);
    }

    SECTION("cleared tables: no class is an MP class")
    {
        ClearFfxivClassManaRows();
        REQUIRE(GetFfxivClassManaRow(WHITE_MAGE) == nullptr);
    }

    ClearFfxivClassManaRows();
}
