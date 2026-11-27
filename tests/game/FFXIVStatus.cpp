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

#include "tc_catch2.h"

#include "FFXIV/Status/FFXIVStatus.h"

#include <map>
#include <set>

using namespace FFXIV::Status;

namespace
{
// Real sheet rows (tools/generate-ffxiv-statuses.py): Fight or Flight 76 (self buff, 20 s), Stun 2
// (debuff, 4 s from Holy 139 / 6 s from Shield Bash 16, no default), Aero 143 (DoT, 30 s), Berserk 86
// (3 stacks, 15 s), Aetherflow 304 (CanStatusOff). Spell ids are arbitrary.
FfxivStatusRow const FightOrFlight{ 76, 90076, 20000, 0, 0, 0 };
FfxivStatusRow const Stun{ 2, 90002, 0, 0, 0, FFXIV_STATUS_FLAG_DEBUFF | FFXIV_STATUS_FLAG_CAN_DISPEL
    | FFXIV_STATUS_FLAG_LOCK_MOVEMENT | FFXIV_STATUS_FLAG_LOCK_ACTIONS };
FfxivStatusRow const Aero{ 143, 90143, 30000, 0, 0, FFXIV_STATUS_FLAG_DEBUFF | FFXIV_STATUS_FLAG_CAN_DISPEL };
FfxivStatusRow const Berserk{ 86, 90086, 15000, 3, 0, 0 };
FfxivStatusRow const Aetherflow{ 304, 90304, 30000, 3, 0,
    FFXIV_STATUS_FLAG_PERMANENT | FFXIV_STATUS_FLAG_CAN_STATUS_OFF };
FfxivStatusRow const Extending{ 9001, 99001, 10000, 0, uint8(FfxivStatusRefreshPolicy::Extend), 0 };

// The aura container for one (target, source) pair, with the container's own countdown.
class FakeContainer final : public FfxivStatusContainer
{
public:
    bool Find(uint32 spellId, FfxivStatusInstance& instance) const override
    {
        auto itr = auras.find(spellId);
        if (itr == auras.end())
            return false;
        instance = itr->second;
        return true;
    }

    bool Add(uint32 spellId, FfxivStatusInstance const& instance) override
    {
        ++calls;
        auras[spellId] = instance;
        return true;
    }

    void Update(uint32 spellId, FfxivStatusInstance const& instance) override
    {
        ++calls;
        auras[spellId] = instance;
    }

    void Remove(uint32 spellId) override
    {
        ++calls;
        auras.erase(spellId);
    }

    void Advance(uint32 diffMs)
    {
        for (auto itr = auras.begin(); itr != auras.end();)
            itr = AdvanceFfxivStatus(itr->second, diffMs) ? std::next(itr) : auras.erase(itr);
    }

    std::map<uint32, FfxivStatusInstance> auras;
    uint32 calls = 0;
};

struct StatusTableFixture
{
    StatusTableFixture()
    {
        SetFfxivStatusTables({ FightOrFlight, Stun, Aero, Berserk, Aetherflow, Extending },
                             { { 139, 2, 4000 }, { 16, 2, 6000 } });
    }
    ~StatusTableFixture() { ClearFfxivStatusTables(); }
};
}

TEST_CASE("FFXIV status: an empty table changes nothing", "[ffxiv]")
{
    ClearFfxivStatusTables();
    FakeContainer container;
    REQUIRE_FALSE(ApplyFfxivStatusTo(container, 76));
    REQUIRE_FALSE(ApplyFfxivStatusTo(container, 76, 20000));
    REQUIRE_FALSE(RequestRemoveFfxivStatusFrom(container, 304));
    REQUIRE(container.calls == 0);
    REQUIRE(container.auras.empty());
    REQUIRE(GetFfxivStatus(76) == nullptr);
    REQUIRE(GetFfxivActionStatusDuration(139, 2) == 0);
    REQUIRE_FALSE(ApplyFfxivStatus(nullptr, 76, nullptr));
}

TEST_CASE_METHOD(StatusTableFixture, "FFXIV status: apply uses the sheet duration", "[ffxiv]")
{
    FakeContainer container;
    REQUIRE(ApplyFfxivStatusTo(container, 76));
    REQUIRE(container.auras.at(90076).stacks == 1);
    REQUIRE(container.auras.at(90076).duration == 20000);
    REQUIRE(container.auras.at(90076).maxDuration == 20000);

    // A debuff with no single default takes the action's duration.
    REQUIRE(GetFfxivActionStatusDuration(139, 2) == 4000);
    REQUIRE(GetFfxivActionStatusDuration(16, 2) == 6000);
    REQUIRE_FALSE(ApplyFfxivStatusTo(container, 2));
    REQUIRE(container.auras.count(90002) == 0);
    REQUIRE(ApplyFfxivStatusTo(container, 2, GetFfxivActionStatusDuration(139, 2)));
    REQUIRE(container.auras.at(90002).duration == 4000);
}

TEST_CASE_METHOD(StatusTableFixture, "FFXIV status: re-apply before expiry follows the refresh policy", "[ffxiv]")
{
    FakeContainer container;

    SECTION("reset: the timer restarts at the new duration")
    {
        REQUIRE(ApplyFfxivStatusTo(container, 143));
        container.Advance(12000);
        REQUIRE(container.auras.at(90143).duration == 18000);
        REQUIRE(ApplyFfxivStatusTo(container, 143));
        REQUIRE(container.auras.at(90143).duration == 30000);
        REQUIRE(container.auras.at(90143).maxDuration == 30000);
        REQUIRE(container.auras.at(90143).stacks == 1);
    }

    SECTION("extend: the new duration is added to what remains")
    {
        REQUIRE(ApplyFfxivStatusTo(container, 9001));
        container.Advance(4000);
        REQUIRE(ApplyFfxivStatusTo(container, 9001));
        REQUIRE(container.auras.at(99001).duration == 16000);
        REQUIRE(container.auras.at(99001).maxDuration == 16000);
    }
}

TEST_CASE_METHOD(StatusTableFixture, "FFXIV status: stacks cap at the sheet's MaxStacks", "[ffxiv]")
{
    FakeContainer container;
    REQUIRE(ApplyFfxivStatusTo(container, 86));
    // A stacking status arrives at its full count: "Grants 3 stacks of Berserk" (ActionTransient) and
    // Status column 4 is the cap. One stack of a three-stack buff is the defect the owner reported.
    REQUIRE(container.auras.at(90086).stacks == 3);
    REQUIRE(ApplyFfxivStatusTo(container, 86));
    REQUIRE(ApplyFfxivStatusTo(container, 86));
    REQUIRE(container.auras.at(90086).stacks == 3);

    container.Advance(10000);
    REQUIRE(ApplyFfxivStatusTo(container, 86));
    REQUIRE(container.auras.at(90086).stacks == 3);
    REQUIRE(container.auras.at(90086).duration == 15000);
}

TEST_CASE_METHOD(StatusTableFixture, "FFXIV status: a non-stacking status stays at one", "[ffxiv]")
{
    FakeContainer container;
    REQUIRE(ApplyFfxivStatusTo(container, 76)); // Fight or Flight, maxStacks 0
    REQUIRE(container.auras.at(90076).stacks == 1);
    REQUIRE(ApplyFfxivStatusTo(container, 76));
    REQUIRE(container.auras.at(90076).stacks == 1);
}

TEST_CASE_METHOD(StatusTableFixture, "FFXIV status: expiry", "[ffxiv]")
{
    FakeContainer container;
    REQUIRE(ApplyFfxivStatusTo(container, 76));
    container.Advance(19999);
    REQUIRE(container.auras.count(90076) == 1);
    container.Advance(1);
    REQUIRE(container.auras.count(90076) == 0);

    // IsPermanent: no timer, still there long after any duration.
    REQUIRE(ApplyFfxivStatusTo(container, 304));
    REQUIRE(container.auras.at(90304).duration == FFXIV_STATUS_UNTIL_REMOVED);
    container.Advance(3600000);
    REQUIRE(container.auras.count(90304) == 1);
}

TEST_CASE_METHOD(StatusTableFixture, "FFXIV status: only a CanStatusOff status may be removed by its bearer", "[ffxiv]")
{
    FakeContainer container;
    REQUIRE(ApplyFfxivStatusTo(container, 304));
    REQUIRE(ApplyFfxivStatusTo(container, 76));

    REQUIRE_FALSE(RequestRemoveFfxivStatusFrom(container, 76));
    REQUIRE(container.auras.count(90076) == 1);

    REQUIRE(RequestRemoveFfxivStatusFrom(container, 304));
    REQUIRE(container.auras.count(90304) == 0);
    REQUIRE_FALSE(RequestRemoveFfxivStatusFrom(container, 304));
}

TEST_CASE("FFXIV status: a comboOnly action status applies only on a correct combo step", "[ffxiv]")
{
    // Storm's Eye 45 -> Surging Tempest 2677 is "Combo Bonus: Grants Surging Tempest" (comboOnly 1); Inner
    // Release 7389 -> Inner Strength 2663 is a plain grant.
    SetFfxivStatusTables({ { 2677, 81814, 30000, 0, 0, 0 }, { 2663, 81815, 15000, 0, 0, 0 } },
                         { { 45, 2677, 30000, true }, { 7389, 2663, 15000, false } });

    CHECK(IsFfxivActionStatusComboOnly(45, 2677));
    CHECK_FALSE(IsFfxivActionStatusComboOnly(7389, 2663));
    CHECK_FALSE(IsFfxivActionStatusComboOnly(45, 2663));   // not a row at all

    CHECK(ShouldApplyFfxivActionStatus(45, 2677, true));
    CHECK_FALSE(ShouldApplyFfxivActionStatus(45, 2677, false));
    CHECK(ShouldApplyFfxivActionStatus(7389, 2663, false)); // a plain row ignores the combo state
    CHECK(ShouldApplyFfxivActionStatus(7389, 2663, true));

    // The duration and the status list are untouched by the flag.
    CHECK(GetFfxivActionStatusDuration(45, 2677) == 30000);
    REQUIRE(GetFfxivActionStatuses(45) != nullptr);
    CHECK(GetFfxivActionStatuses(45)->size() == 1);

    ClearFfxivStatusTables();
    CHECK_FALSE(IsFfxivActionStatusComboOnly(45, 2677));
}

TEST_CASE("FFXIV status: a trait-gated action status applies only once the trait's passive is known", "[ffxiv]")
{
    // Infuriate 52 -> Nascent Chaos 1897 waits for trait 267 (Warrior, level 72); Infuriate's Nascent Chaos row is the
    // generated [52, 1897, 30000, 0, 267]. Fight or Flight 20 -> 76 waits for none. Spell ids are arbitrary.
    SetFfxivStatusTables({ { 1897, 81897, 30000, 0, 0, 0 }, FightOrFlight },
                         { { 52, 1897, 30000, false, 267 }, { 20, 76, 20000 } },
                         { { 267, 82967 } });

    auto knows = [](std::set<uint32> spells) { return [spells](uint32 spellId) { return spells.count(spellId) != 0; }; };

    CHECK(GetFfxivActionStatusTrait(52, 1897) == 267);
    CHECK(GetFfxivActionStatusTrait(20, 76) == 0);
    CHECK(GetFfxivTraitSpell(267) == 82967);

    CHECK_FALSE(IsFfxivActionStatusTraitMet(52, 1897, knows({})));        // below 72: no trait 267
    CHECK(IsFfxivActionStatusTraitMet(52, 1897, knows({ 82967 })));       // 72+: the passive is learned
    CHECK(IsFfxivActionStatusTraitMet(20, 76, knows({})));                // no trait on the row

    // A trait ffxiv_trait does not carry can never be met.
    SetFfxivStatusTables({ { 1897, 81897, 30000, 0, 0, 0 } }, { { 52, 1897, 30000, false, 267 } });
    CHECK_FALSE(IsFfxivActionStatusTraitMet(52, 1897, [](uint32) { return true; }));

    ClearFfxivStatusTables();
    CHECK(GetFfxivActionStatusTrait(52, 1897) == 0);
    CHECK(IsFfxivActionStatusTraitMet(52, 1897, knows({})));
}
