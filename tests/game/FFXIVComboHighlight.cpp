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
 * FFXIV combo highlight: the pure part. Which chain action comes next, what happens at a chain break,
 * and the "clear" record.
 *
 * The inputs are the damage slice's own tables (ffxiv_action_combo, ffxiv_action_spell) fed through
 * their test setters, so these cases pin the answers the live hook and the per-tick clear produce
 * without building a Player. The player-driven paths (the once-per-cast send, the per-tick expiry and
 * the enter-world reconcile) are the same two calls: ResolveFfxivComboHighlightSpell and the payload
 * builder asserted here.
 */

#include "tc_catch2.h"

#include "FFXIV/Combat/FFXIVActionDamage.h"
#include "FFXIV/Combat/FFXIVComboHighlight.h"

#include <vector>

using namespace FFXIV::Combat;

namespace
{
// Retail Action ids and the module's synthetic spell ids (the module assigns the real ones).
constexpr uint32 FAST_BLADE = 9;   // -> Riot Blade
constexpr uint32 RIOT_BLADE = 15;  // -> Rage of Halone (a spellless action in the third fixture)
constexpr uint32 RAGE_OF_HALONE = 21;
constexpr uint32 HEAVY_SWING = 31; // a starter with no successor, the "chain ends" case

constexpr uint32 FAST_BLADE_SPELL = 90009;
constexpr uint32 RIOT_BLADE_SPELL = 90015;

std::vector<FfxivActionComboRow> Chain()
{
    return {
        { FAST_BLADE, RIOT_BLADE },
        { RIOT_BLADE, RAGE_OF_HALONE },
    };
}

std::vector<FfxivActionSpellRow> Spells()
{
    return {
        { FAST_BLADE, FAST_BLADE_SPELL },
        { RIOT_BLADE, RIOT_BLADE_SPELL },
    };
}

struct ComboFixture
{
    ComboFixture()
    {
        SetFfxivActionDamageTables({}, Chain());
        SetFfxivActionSpellRows(Spells());
        ClearFfxivComboHighlightState();
    }

    ~ComboFixture()
    {
        ClearFfxivActionDamageTables();
        ClearFfxivComboHighlightState();
    }
};
} // namespace

TEST_CASE("FFXIV combo highlight: the next action of the chain is the highlighted spell", "[ffxiv]")
{
    ComboFixture fixture;
    CHECK(NextFfxivComboActionId(FAST_BLADE) == RIOT_BLADE);
    CHECK(NextFfxivComboActionId(RIOT_BLADE) == RAGE_OF_HALONE);
    CHECK(ResolveFfxivComboHighlightSpell(FAST_BLADE) == RIOT_BLADE_SPELL);
    // The successor exists in the combo table but the module created no spell for it: nothing to
    // draw, so nothing is sent.
    CHECK(ResolveFfxivComboHighlightSpell(RIOT_BLADE) == 0);
}

TEST_CASE("FFXIV combo highlight: a broken chain clears", "[ffxiv]")
{
    ComboFixture fixture;
    // No combo row starts here (the chain ended, or the last action was cleared by the damage hook).
    CHECK(NextFfxivComboActionId(HEAVY_SWING) == 0);
    CHECK(ResolveFfxivComboHighlightSpell(HEAVY_SWING) == 0);
    // 0 is also the "no last action" state the tick and the expired window produce.
    CHECK(ResolveFfxivComboHighlightSpell(0) == 0);
}

TEST_CASE("FFXIV combo highlight: no tables means no highlight", "[ffxiv]")
{
    ClearFfxivActionDamageTables();
    ClearFfxivComboHighlightState();
    CHECK_FALSE(HasFfxivActionComboRows());
    CHECK(ResolveFfxivComboHighlightSpell(FAST_BLADE) == 0);
    CHECK(NextFfxivComboActionId(FAST_BLADE) == 0);
    CHECK(ResolveFfxivSpellFromActionId(FAST_BLADE) == 0);
}

TEST_CASE("FFXIV combo highlight: the 0x7A63 payload", "[ffxiv]")
{
    ComboFixture fixture;
    CHECK(FFXIV_COMBO_HIGHLIGHT_OPCODE == 0x7A63);
    CHECK(FFXIV_COMBO_HIGHLIGHT_SCHEMA == 1);

    std::vector<uint8> const highlight = BuildFfxivComboHighlightPayload(RIOT_BLADE_SPELL);
    CHECK(highlight.size() == FFXIV_COMBO_HIGHLIGHT_BYTES);
    // 90015 = 0x00015F9F, little-endian, then the schema byte (the client test pins the same bytes).
    std::vector<uint8> const expectedHighlight{ 0x9F, 0x5F, 0x01, 0x00, 0x01 };
    CHECK(highlight == expectedHighlight);

    std::vector<uint8> const clear = BuildFfxivComboHighlightPayload(0);
    std::vector<uint8> const expectedClear{ 0x00, 0x00, 0x00, 0x00, 0x01 };
    CHECK(clear == expectedClear);
}

TEST_CASE("FFXIV combo highlight: the reverse spell map", "[ffxiv]")
{
    ComboFixture fixture;
    CHECK(ResolveFfxivSpellFromActionId(FAST_BLADE) == FAST_BLADE_SPELL);
    CHECK(ResolveFfxivSpellFromActionId(RIOT_BLADE) == RIOT_BLADE_SPELL);
    CHECK(ResolveFfxivSpellFromActionId(RAGE_OF_HALONE) == 0);
    CHECK(ResolveFfxivSpellFromActionId(0) == 0);
}

TEST_CASE("FFXIV combo highlight: several successors resolve to the lowest", "[ffxiv]")
{
    // The retail sheets carry one successor per action outside PvP combo routes; if a second row ever
    // lands, the answer must still be deterministic rather than "whichever loaded last".
    SetFfxivActionDamageTables({}, { { FAST_BLADE, 15 }, { FAST_BLADE, 11 } });
    SetFfxivActionSpellRows({});
    CHECK(NextFfxivComboActionId(FAST_BLADE) == 11);
    ClearFfxivActionDamageTables();
    ClearFfxivComboHighlightState();
}
