/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "tc_catch2.h"

#include "PlayerClassSwitch.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cstring>
#include <limits>

using namespace ClassSwitch;

namespace
{
StoredClassProfile MinimalProfile()
{
    StoredClassProfile profile;
    profile.Revision = 1;
    profile.ClassId = 8;
    profile.Level = 1;
    profile.NextLevelXp = 400;
    profile.ActiveSpec = 0;
    profile.SpecCount = 1;
    profile.HealthPercent = 80.0f;
    profile.PowerPercent.fill(100.0f);
    profile.Spells.push_back({ 133, 2, true, false, false });
    profile.Spells.push_back({ 116, 2, true, false, false });
    profile.Groups[0].Actions[1] = 116;
    return profile;
}

ActiveClassSnapshot MinimalSnapshot()
{
    ActiveClassSnapshot snapshot;
    snapshot.Session = 12345;
    snapshot.Revision = 1;
    snapshot.Guid = 0x0000000000000001;
    snapshot.Race = 1;
    snapshot.ClassId = 8;
    snapshot.Level = 1;
    snapshot.TalentGroupCount = 1;
    snapshot.Resources.Powers[0] = 100;
    snapshot.Resources.MaxPowers[0] = 100;
    snapshot.Spells.push_back({ 133, 0 });
    snapshot.Resources.PredictedPowers.fill(0);
    snapshot.Resources.Runes.PredictedMask = 0;
    snapshot.Health = 100;
    snapshot.MaxHealth = 100;
    return snapshot;
}
}

TEST_CASE("Class switch class IDs and masks cover the uint32 boundary", "[ClassSwitch]")
{
    for (uint32 classId = 1; classId <= 32; ++classId)
    {
        REQUIRE(IsSupportedClassId(uint8(classId)));
        REQUIRE(ClassIdToMask(uint8(classId)) == (1u << (classId - 1)));
    }
    REQUIRE(ClassIdToMask(1) == 0x00000001u);
    REQUIRE(ClassIdToMask(11) == 0x00000400u);
    REQUIRE(ClassIdToMask(32) == 0x80000000u);
    REQUIRE_FALSE(IsSupportedClassId(0));
    REQUIRE_FALSE(IsSupportedClassId(33));
    uint8 sessionKey[40] = {};
    REQUIRE(ComputeSessionTag(sessionKey, sizeof(sessionKey)) == 0x40d69e0cf0f65c45ULL);
}

TEST_CASE("Class switch response decoder matches the frozen client vector", "[ClassSwitch]")
{
    uint8 const vector[] = {
        0x01, 0x00,             // version
        0x02,                   // rejected
        0x07,                   // class
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
        0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
        0x03, 0x00,             // reason length
        'b', 'a', 'd'
    };

    ClientClassSwitchResponse response;
    std::string error;
    REQUIRE(DecodeClientClassSwitchResponse(vector, sizeof(vector), &response, &error));
    REQUIRE(response.Version == RESPONSE_PROTOCOL_VERSION);
    REQUIRE(response.Disposition == ClientResponseDisposition::Rejected);
    REQUIRE(response.ClassId == 7);
    REQUIRE(response.Session == 0x0102030405060708ULL);
    REQUIRE(response.Revision == 0x1112131415161718ULL);
    REQUIRE(response.SnapshotDigest == 0x2122232425262728ULL);
    REQUIRE(response.Reason == "bad");

    REQUIRE_FALSE(DecodeClientClassSwitchResponse(vector, sizeof(vector) - 1, &response, &error));
    REQUIRE(error.find("truncated") != std::string::npos);

    uint8 trailing[sizeof(vector) + 1] = {};
    std::memcpy(trailing, vector, sizeof(vector));
    REQUIRE_FALSE(DecodeClientClassSwitchResponse(trailing, sizeof(trailing), &response, &error));
    REQUIRE(error.find("trailing") != std::string::npos);

    uint8 zeroDigest[sizeof(vector)];
    std::memcpy(zeroDigest, vector, sizeof(vector));
    std::fill(zeroDigest + 20, zeroDigest + 28, 0);
    REQUIRE_FALSE(DecodeClientClassSwitchResponse(zeroDigest, sizeof(zeroDigest), &response, &error));
    REQUIRE(error.find("digest") != std::string::npos);

    uint8 invalidUtf8[sizeof(vector)];
    std::memcpy(invalidUtf8, vector, sizeof(vector));
    invalidUtf8[30] = 0x80;
    REQUIRE_FALSE(DecodeClientClassSwitchResponse(
        invalidUtf8, sizeof(invalidUtf8), &response, &error));
    REQUIRE(error.find("UTF-8") != std::string::npos);
}

TEST_CASE("Class switch response digest is FNV-1a 64", "[ClassSwitch]")
{
    uint8 const empty[] = {};
    uint8 const abc[] = { 'a', 'b', 'c' };
    REQUIRE(ComputeFnv1a64(empty, 0) == 14695981039346656037ULL);
    REQUIRE(ComputeFnv1a64(abc, sizeof(abc)) == 0xe71fa2190541574bULL);
}

TEST_CASE("Class switch response matching rejects stale and wrong-session acknowledgements", "[ClassSwitch]")
{
    ClientClassSwitchResponse response;
    response.Version = RESPONSE_PROTOCOL_VERSION;
    response.Disposition = ClientResponseDisposition::Accepted;
    response.ClassId = 7;
    response.Session = 11;
    response.Revision = 9;
    response.SnapshotDigest = 33;
    REQUIRE(MatchClientClassSwitchResponse(
        response, 11, 7, 9, 33, 8, 5, 0, 0, 0) == ClientResponseMatch::Accepted);
    REQUIRE(MatchClientClassSwitchResponse(
        response, 12, 7, 9, 33, 8, 5, 0, 0, 0) == ClientResponseMatch::StaleOrMismatched);
    REQUIRE(MatchClientClassSwitchResponse(
        response, 11, 7, 10, 33, 8, 5, 0, 0, 0) == ClientResponseMatch::StaleOrMismatched);
    REQUIRE(MatchClientClassSwitchResponse(
        response, 11, 7, 9, 34, 8, 5, 0, 0, 0) == ClientResponseMatch::StaleOrMismatched);
    REQUIRE(MatchClientClassSwitchResponse(
        response, 11, 7, 9, 33, 8, 5, 7, 9, 33) == ClientResponseMatch::DuplicateAccepted);
    REQUIRE(MatchClientClassSwitchResponse(
        response, 11, 8, 10, 44, 8, 5, 7, 9, 99) == ClientResponseMatch::StaleOrMismatched);

    response.Disposition = ClientResponseDisposition::ResyncRequested;
    response.ClassId = 8;
    response.Revision = 5;
    response.SnapshotDigest = 0;
    REQUIRE(MatchClientClassSwitchResponse(
        response, 11, 7, 9, 33, 8, 5, 0, 0, 0) == ClientResponseMatch::ResyncRequested);
    response.Revision = 6;
    REQUIRE(MatchClientClassSwitchResponse(
        response, 11, 7, 9, 33, 8, 5, 0, 0, 0) == ClientResponseMatch::StaleOrMismatched);
}

TEST_CASE("Class switch timeout recovery is bounded", "[ClassSwitch]")
{
    REQUIRE(EvaluateClassSwitchTimeout(false, false, true, 0)
        == ClientTimeoutAction::None);
    REQUIRE(EvaluateClassSwitchTimeout(true, false, false, 0)
        == ClientTimeoutAction::None);
    REQUIRE(EvaluateClassSwitchTimeout(true, false, true, 0)
        == ClientTimeoutAction::RecoverSource);
    REQUIRE(EvaluateClassSwitchTimeout(true, true, true, 1)
        == ClientTimeoutAction::Disconnect);
}

TEST_CASE("Class switch logout cannot persist staged state after restore failure", "[ClassSwitch]")
{
    ClassSwitchLogoutPlan plan = PlanClassSwitchLogout(false, false);
    REQUIRE(plan.PersistPlayer);
    REQUIRE(plan.RemovePet);
    REQUIRE(plan.ClearRuntime);

    plan = PlanClassSwitchLogout(true, true);
    REQUIRE(plan.PersistPlayer);
    REQUIRE(plan.RemovePet);
    REQUIRE(plan.ClearRuntime);

    plan = PlanClassSwitchLogout(true, false);
    REQUIRE_FALSE(plan.PersistPlayer);
    REQUIRE_FALSE(plan.RemovePet);
    REQUIRE(plan.ClearRuntime);

    REQUIRE_FALSE(IsClassSwitchGameplayBlocked(false, false));
    REQUIRE(IsClassSwitchGameplayBlocked(true, false));
    REQUIRE_FALSE(IsClassSwitchGameplayBlocked(true, true));
}

TEST_CASE("Class switch resync response requires a zero digest", "[ClassSwitch]")
{
    uint8 const vector[] = {
        0x01, 0x00,             // version
        0x03,                   // resync requested
        0x08,                   // current class
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00
    };
    ClientClassSwitchResponse response;
    std::string error;
    REQUIRE(DecodeClientClassSwitchResponse(vector, sizeof(vector), &response, &error));
    REQUIRE(response.Disposition == ClientResponseDisposition::ResyncRequested);
    REQUIRE(response.SnapshotDigest == 0);

    uint8 badDigest[sizeof(vector)];
    std::memcpy(badDigest, vector, sizeof(vector));
    badDigest[20] = 1;
    REQUIRE_FALSE(DecodeClientClassSwitchResponse(badDigest, sizeof(badDigest), &response, &error));
}

TEST_CASE("Class switch selection policy rejects stale and unsafe requests", "[ClassSwitch]")
{
    SelectionPolicy policy;
    policy.ActiveRevision = 4;
    policy.ClientRevision = 4;
    REQUIRE(EvaluateSelectionPolicy(policy) == RequestResult::Ok);

    policy.Pending = true;
    REQUIRE(EvaluateSelectionPolicy(policy) == RequestResult::Busy);
    policy.Pending = false;
    policy.InCombat = true;
    REQUIRE(EvaluateSelectionPolicy(policy) == RequestResult::NotReady);
    policy.InCombat = false;
    policy.ClientRevision = 3;
    REQUIRE(EvaluateSelectionPolicy(policy) == RequestResult::StaleRevision);
    policy.ClientRevision = 4;
    policy.SameClass = true;
    REQUIRE(EvaluateSelectionPolicy(policy) == RequestResult::TargetUnchanged);
}

TEST_CASE("Class switch only admits target-family or shared spells", "[ClassSwitch]")
{
    REQUIRE(IsSpellAllowedForClassProfile(7, 7, false));
    REQUIRE(IsSpellAllowedForClassProfile(0, 7, false));
    REQUIRE(IsSpellAllowedForClassProfile(10, 7, false) == false);
    REQUIRE(IsSpellAllowedForClassProfile(10, 7, true));
}

TEST_CASE("Class switch start levels follow config without resetting existing profiles", "[ClassSwitch]")
{
    REQUIRE(ResolveClassStartLevel(CLASS_DEATH_KNIGHT, 55, 1, 1) == 1);
    REQUIRE(ResolveClassStartLevel(CLASS_WARRIOR, 7, 1, 1) == 7);
    REQUIRE(ResolveClassStartLevel(CLASS_MAGE, 0, 4, 1) == 4);

    REQUIRE(ResolveClassStatusLevel(
        CLASS_DEATH_KNIGHT, false, 55, 55, 1, 1) == 1);
    REQUIRE(ResolveClassStatusLevel(
        CLASS_DEATH_KNIGHT, true, 55, 55, 1, 1) == 55);
    REQUIRE(ResolveClassStatusLevel(
        CLASS_WARRIOR, true, 12, 1, 1, 1) == 12);
    REQUIRE(ResolveClassStatusLevel(
        CLASS_WARRIOR, false, 0, 1, 1, 1) == 1);
}

TEST_CASE("Character languages survive repeated class-profile application", "[ClassSwitch]")
{
    std::set<uint32> const languageSpells{ 669, 815, 7340 };
    std::set<uint32> const languageSkills{ 669, 815, 7340 };
    CharacterLanguageState const characterLanguages{
        { { 669, 0, true, false, false },
          { 7340, 0, true, false, false } },
        { { 669, 0, 300, 300, 0, 0, true },
          { 7340, 0, 300, 300, 0, 0, true } }
    };

    StoredClassProfile warrior = MinimalProfile();
    warrior.ClassId = CLASS_WARRIOR;
    warrior.Spells = { { 78, 0, true, false, false },
        { 815, 0, true, false, false } };
    warrior.Skills = { { 43, 0, 20, 100, 0, 0, false },
        { 815, 0, 300, 300, 0, 0, true } };

    StoredClassProfile mage = MinimalProfile();
    mage.ClassId = CLASS_MAGE;
    mage.Spells = { { 133, 0, true, false, false },
        { 815, 0, true, false, false } };
    mage.Skills = { { 6, 0, 20, 100, 0, 0, false },
        { 669, 0, 300, 300, 0, 0, true } };

    for (uint32 iteration = 0; iteration < 8; ++iteration)
    {
        StoredClassProfile applied = iteration % 2 ? warrior : mage;
        uint32 removedSpells = 0;
        uint32 removedSkills = 0;
        StripClassOwnedLanguages(&applied, languageSpells, languageSkills,
            &removedSpells, &removedSkills);
        REQUIRE(removedSpells == 1);
        REQUIRE(removedSkills == 1);
        MergeCharacterLanguages(&applied, characterLanguages, languageSpells, languageSkills);

        std::vector<uint32> appliedLanguageSpells;
        for (SpellProfile const& spell : applied.Spells)
            if (languageSpells.count(spell.Id))
                appliedLanguageSpells.push_back(spell.Id);
        REQUIRE(appliedLanguageSpells == std::vector<uint32>{ 669, 7340 });

        REQUIRE(std::any_of(applied.Spells.begin(), applied.Spells.end(),
            [&applied](SpellProfile const& spell) {
                return spell.Id == (applied.ClassId == CLASS_WARRIOR ? 78u : 133u);
            }));

        uint32 capturedLanguages = 0;
        StoredClassProfile captured = applied;
        StripClassOwnedLanguages(&captured, languageSpells, languageSkills,
            &capturedLanguages, nullptr);
        REQUIRE(capturedLanguages == 2);
        REQUIRE(std::none_of(captured.Spells.begin(), captured.Spells.end(),
            [&languageSpells](SpellProfile const& spell) {
                return languageSpells.count(spell.Id) != 0;
            }));
    }
}

TEST_CASE("Inactive action ranks normalize only to known active ranks", "[ClassSwitch]")
{
    std::vector<uint32> const chain{ 100, 200, 300, 400 };
    std::set<uint32> active{ 100, 300 };
    REQUIRE(NormalizeActionSpellRank(300, chain, active) == 300);
    REQUIRE(NormalizeActionSpellRank(400, chain, active) == 300);
    REQUIRE(NormalizeActionSpellRank(200, chain, active) == 300);
    active.clear();
    REQUIRE(NormalizeActionSpellRank(200, chain, active) == 0);
}

TEST_CASE("Equipment planning reserves permutations, freed target positions, and rollback slots", "[ClassSwitch]")
{
    std::vector<EquipmentBagCandidate> const candidates{
        { uint16(0) << 8 | 19, false },
        { uint16(0) << 8 | 20, false },
        { uint16(0) << 8 | 21, true }
    };
    std::vector<EquipmentBagDemand> const demands{
        { { candidates[0].Position, candidates[1].Position, candidates[2].Position } },
        { { candidates[0].Position, candidates[1].Position, candidates[2].Position } },
        { { candidates[2].Position } }
    };
    std::vector<uint16> reservations;
    REQUIRE(PlanEquipmentBagReservations(candidates, demands, &reservations));
    REQUIRE(reservations.size() == 3);
    REQUIRE(std::set<uint16>(reservations.begin(), reservations.end()).size() == 3);
    REQUIRE(reservations[2] == candidates[2].Position);

    std::vector<EquipmentBagDemand> const constrained{
        { { candidates[0].Position, candidates[1].Position } },
        { { candidates[0].Position } }
    };
    REQUIRE(PlanEquipmentBagReservations(candidates, constrained, &reservations));
    REQUIRE(reservations[0] == candidates[1].Position);
    REQUIRE(reservations[1] == candidates[0].Position);

    EquipmentBagDemand const pressureDemand{
        { candidates[0].Position, candidates[1].Position }
    };
    std::vector<EquipmentBagDemand> const overCapacity(3, pressureDemand);
    REQUIRE_FALSE(PlanEquipmentBagReservations(candidates, overCapacity, &reservations));
    REQUIRE(reservations.empty());
}

TEST_CASE("Equipment changes include both slots in two-hand transitions", "[ClassSwitch]")
{
    constexpr uint8 mainhand = 15;
    constexpr uint8 offhand = 16;
    std::array<uint64, EQUIPMENT_SLOTS> current{};
    std::array<uint64, EQUIPMENT_SLOTS> target{};
    current[mainhand] = 101;
    current[offhand] = 102;
    target[mainhand] = 201;
    target[offhand] = 202;
    std::vector<uint8> changed = BuildChangedEquipmentSlots(current, target);
    REQUIRE(changed == std::vector<uint8>{ mainhand, offhand });

    current = target;
    target[mainhand] = 301;
    target[offhand] = 0;
    changed = BuildChangedEquipmentSlots(current, target);
    REQUIRE(changed == std::vector<uint8>{ mainhand, offhand });
}

TEST_CASE("Class switch reserves one target revision above both profiles", "[ClassSwitch]")
{
    uint64 reserved = 0;
    REQUIRE(ReserveTransitionRevision(4, 2, &reserved));
    REQUIRE(reserved == 5);
    REQUIRE(ReserveTransitionRevision(3, 9, &reserved));
    REQUIRE(reserved == 10);
    REQUIRE_FALSE(ReserveTransitionRevision(0, 3, &reserved));
    REQUIRE_FALSE(ReserveTransitionRevision(3, 0, &reserved));
    REQUIRE_FALSE(ReserveTransitionRevision(
        std::numeric_limits<uint64>::max(), 1, &reserved));
}

TEST_CASE("Class switch only removes self-owned class auras", "[ClassSwitch]")
{
    REQUIRE(IsSwitchOwnedAura(10, 10, 8, 8, false));
    REQUIRE(IsSwitchOwnedAura(10, 10, 0, 0, true));
    REQUIRE_FALSE(IsSwitchOwnedAura(10, 11, 8, 8, false));
    REQUIRE_FALSE(IsSwitchOwnedAura(10, 10, 0, 0, false));
    REQUIRE_FALSE(IsSwitchOwnedAura(10, 10, 8, 3, false));
}

TEST_CASE("Stored class profile round trips through its bounded wire form", "[ClassSwitch]")
{
    StoredClassProfile profile = MinimalProfile();
    profile.Level = 42;
    profile.Xp = 1234;
    profile.NextLevelXp = 5678;
    profile.Skills.push_back({ 333, 1, 50, 100, 2, 3, true });
    profile.Groups[0].Talents.push_back({ 120, 0 });
    profile.Groups[0].Glyphs[0] = 42;
    profile.Groups[0].Actions[0] = 133;
    profile.Equipment[1].ItemId = 40403;
    profile.Equipment[1].Enchantments[0] = 3150;
    profile.Equipment[1].EquippedGuid = 0x4000000000000123ULL;
    profile.Equipment[15].ItemId = 40000;
    profile.Equipment[15].EquippedGuid = 0x4000000000000124ULL;
    profile.Runes.Count = 6;
    profile.Runes.Types[0] = 1;
    profile.Runes.BaseTypes[0] = 1;
    profile.Runes.Cooldowns[0] = 2500;
    profile.Runes.LastUsed[0] = 1;

    std::string error;
    REQUIRE(ValidateStoredProfile(profile, &error));
    std::vector<uint8> encoded = EncodeStoredProfile(profile);
    REQUIRE_FALSE(encoded.empty());

    StoredClassProfile decoded;
    REQUIRE(DecodeStoredProfile(encoded.data(), encoded.size(), &decoded, &error));
    REQUIRE(decoded.ClassId == profile.ClassId);
    REQUIRE(decoded.Level == profile.Level);
    REQUIRE(decoded.Xp == 1234);
    REQUIRE(decoded.NextLevelXp == 5678);
    REQUIRE(decoded.Spells.size() == 2);
    REQUIRE(decoded.Spells[0].Id == 133);
    REQUIRE(decoded.Spells[1].Id == 116);
    REQUIRE(decoded.Groups[0].Actions[0] == 133);
    REQUIRE(decoded.Groups[0].Actions[1] == 116);
    REQUIRE(decoded.Runes.Count == 6);
    REQUIRE(decoded.Runes.Cooldowns[0] == 2500);
    REQUIRE(decoded.Equipment[1].ItemId == 40403);
    REQUIRE(decoded.Equipment[1].Enchantments[0] == 3150);
    REQUIRE(decoded.Equipment[1].EquippedGuid == 0x4000000000000123ULL);
    REQUIRE(decoded.Equipment[15].ItemId == 40000);

    std::vector<uint8> legacy(encoded.begin(), encoded.end() - EQUIPMENT_SLOTS * 16);
    uint16 const legacyVersion = 1;
    std::memcpy(legacy.data() + sizeof(uint32), &legacyVersion, sizeof(legacyVersion));
    StoredClassProfile migrated;
    REQUIRE(DecodeStoredProfile(legacy.data(), legacy.size(), &migrated, &error));
    REQUIRE(migrated.Version == PROFILE_VERSION);
    for (StoredEquipmentItem const& item : migrated.Equipment)
        REQUIRE(item.EquippedGuid == 0);
}

TEST_CASE("Druid profile preserves its starter action and reports a missing spell precisely", "[ClassSwitch]")
{
    StoredClassProfile druid;
    druid.Revision = 9;
    druid.ClassId = 11;
    druid.Level = 13;
    druid.NextLevelXp = 11000;
    druid.ActiveSpec = 0;
    druid.SpecCount = 1;
    druid.HealthPercent = 100.0f;
    druid.PowerPercent.fill(100.0f);
    druid.Spells.push_back({ 6603, 0, true, false, false });
    druid.Spells.push_back({ 5185, 0, true, false, false });
    druid.Spells.push_back({ 80900, 0, true, false, false });
    druid.Groups[0].Actions[1] = 5185;

    std::string error;
    REQUIRE(ValidateStoredProfile(druid, &error));
    std::vector<uint8> encoded = EncodeStoredProfile(druid);
    REQUIRE_FALSE(encoded.empty());

    StoredClassProfile decoded;
    REQUIRE(DecodeStoredProfile(encoded.data(), encoded.size(), &decoded, &error));
    REQUIRE(decoded.ClassId == 11);
    REQUIRE(decoded.Groups[0].Actions[1] == 5185);
    REQUIRE(std::any_of(decoded.Spells.begin(), decoded.Spells.end(),
        [](SpellProfile const& spell) { return spell.Id == 5185; }));

    druid.Spells.erase(std::find_if(druid.Spells.begin(), druid.Spells.end(),
        [](SpellProfile const& spell) { return spell.Id == 5185; }));
    REQUIRE_FALSE(ValidateStoredProfile(druid, &error));
    REQUIRE(error.find("action 1") != std::string::npos);
    REQUIRE(error.find("specialization 0") != std::string::npos);
    REQUIRE(error.find("spell 5185") != std::string::npos);
}

TEST_CASE("Class profile actions may reference a known inactive spell rank", "[ClassSwitch]")
{
    StoredClassProfile profile = MinimalProfile();
    profile.Spells[0].Active = false;
    profile.Groups[0].Actions[1] = profile.Spells[0].Id;

    std::string error;
    REQUIRE(ValidateStoredProfile(profile, &error));

    profile.Spells.erase(profile.Spells.begin());
    REQUIRE_FALSE(ValidateStoredProfile(profile, &error));
    REQUIRE(error.find("spell 133") != std::string::npos);
}

TEST_CASE("Class profile backing-table writes are ordered and non-destructive", "[ClassSwitch]")
{
    StoredClassProfile profile = MinimalProfile();
    profile.SpecCount = 2;
    profile.Groups[0].Talents.push_back({ 120, 0 });
    profile.Groups[0].Glyphs[0] = 42;
    profile.Groups[0].Actions[0] = 133;
    profile.Groups[0].Actions[1] = 0x80000001u;
    profile.Groups[1].Actions[2] = 116;
    profile.Skills.push_back({ 333, 1, 50, 100, 0, 0, true });
    profile.Spells.push_back({ 669, 0, true, false, false });
    profile.Skills.push_back({ 669, 0, 300, 300, 0, 0, true });

    std::vector<std::string> statements = BuildProfileBackingTableStatements(
        77, profile, { 669, 815 }, { 669, 815 });
    REQUIRE_FALSE(statements.empty());
    REQUIRE(std::any_of(statements.begin(), statements.end(), [](std::string const& sql) {
        return sql.find("DELETE FROM character_spell WHERE guid = 77 AND spell NOT IN (669, 815)") == 0;
    }));
    REQUIRE(std::any_of(statements.begin(), statements.end(), [](std::string const& sql) {
        return sql.find("DELETE FROM character_skills WHERE guid = 77 AND skill NOT IN (669, 815)") == 0;
    }));
    REQUIRE(std::any_of(statements.begin(), statements.end(), [](std::string const& sql) {
        return sql.find("DELETE FROM character_action WHERE guid = 77") == 0;
    }));
    REQUIRE(std::any_of(statements.begin(), statements.end(), [](std::string const& sql) {
        return sql.find("INSERT INTO character_spell") != std::string::npos
            && sql.find(", 133, 1, 0)") != std::string::npos;
    }));
    REQUIRE(std::any_of(statements.begin(), statements.end(), [](std::string const& sql) {
        return sql.find("INSERT INTO character_skills") != std::string::npos
            && sql.find(", 333, 50, 100)") != std::string::npos;
    }));
    REQUIRE(std::none_of(statements.begin(), statements.end(), [](std::string const& sql) {
        return sql.find("INSERT INTO character_spell") != std::string::npos
            && sql.find(", 669, ") != std::string::npos;
    }));
    REQUIRE(std::none_of(statements.begin(), statements.end(), [](std::string const& sql) {
        return sql.find("INSERT INTO character_skills") != std::string::npos
            && sql.find(", 669, ") != std::string::npos;
    }));
    REQUIRE(std::any_of(statements.begin(), statements.end(), [](std::string const& sql) {
        return sql.find("INSERT INTO character_action") != std::string::npos
            && sql.find(", 0, 0, 133, 0)") != std::string::npos;
    }));
    REQUIRE(std::any_of(statements.begin(), statements.end(), [](std::string const& sql) {
        return sql.find("INSERT INTO character_action") != std::string::npos
            && sql.find(", 1, 2, 116, 0)") != std::string::npos;
    }));
    REQUIRE(std::any_of(statements.begin(), statements.end(), [](std::string const& sql) {
        return sql.find("character_talent") != std::string::npos
            && sql.find("INSERT") != std::string::npos;
    }));
    for (std::string const& sql : statements)
        REQUIRE(sql.find("DROP TABLE") == std::string::npos);
}

TEST_CASE("Stored class profile rejects duplicate spell references", "[ClassSwitch]")
{
    StoredClassProfile profile = MinimalProfile();
    profile.Spells.push_back(profile.Spells.front());
    std::string error;
    REQUIRE_FALSE(ValidateStoredProfile(profile, &error));
    REQUIRE_FALSE(error.empty());
}

TEST_CASE("Stored class profile rejects incomplete or duplicate equipment", "[ClassSwitch]")
{
    StoredClassProfile profile = MinimalProfile();
    profile.Equipment[0].ItemId = 123;
    std::string error;
    REQUIRE_FALSE(ValidateStoredProfile(profile, &error));
    REQUIRE(error.find("equipment slot 0") != std::string::npos);

    profile.Equipment[0].EquippedGuid = 0x4000000000000001ULL;
    REQUIRE(ValidateStoredProfile(profile, &error));
    profile.Equipment[1] = profile.Equipment[0];
    REQUIRE_FALSE(ValidateStoredProfile(profile, &error));
    REQUIRE(error.find("duplicated") != std::string::npos);
}

TEST_CASE("Stored class profile rejects rune state outside its active slots", "[ClassSwitch]")
{
    StoredClassProfile profile = MinimalProfile();
    profile.Runes.Count = 6;
    profile.Runes.Types[7] = 1;
    std::string error;
    REQUIRE_FALSE(ValidateStoredProfile(profile, &error));
    REQUIRE(error.find("beyond") != std::string::npos);
}

TEST_CASE("Active class snapshot v1 encodes with the required field envelope", "[ClassSwitch]")
{
    ActiveClassSnapshot snapshot = MinimalSnapshot();
    std::string error;
    REQUIRE(ValidateActiveClassSnapshot(snapshot, &error));

    std::vector<uint8> encoded = EncodeActiveClassSnapshot(snapshot);
    REQUIRE(encoded.size() > 256);

    uint32 magic = 0;
    uint16 version = 0;
    std::memcpy(&magic, encoded.data(), sizeof(magic));
    std::memcpy(&version, encoded.data() + sizeof(magic), sizeof(version));
    REQUIRE(magic == SNAPSHOT_MAGIC);
    REQUIRE(version == SNAPSHOT_VERSION);
}

TEST_CASE("Class switch status encodes the final client field order", "[ClassSwitch]")
{
    std::vector<ActiveClassStatusDescriptor> classes{
        { 8, 23, CLASS_FLAG_ACTIVE | CLASS_FLAG_AVAILABLE, "Mage" },
        { 1, 1, CLASS_FLAG_PENDING | CLASS_FLAG_AVAILABLE, "Warrior" }
    };
    std::vector<uint8> encoded = EncodeActiveClassStatus(StatusStage::Accepted,
        RequestResult::Ok, 8, 1, 7, 0x1020304050607080ULL, classes);
    REQUIRE(encoded.size() == 23 + (5 + 4) + (5 + 7));

    uint16 protocol = 0;
    uint8 stage = 0;
    uint8 result = 0;
    uint8 active = 0;
    uint8 pending = 0;
    uint64 revision = 0;
    uint64 session = 0;
    uint8 count = 0;
    std::memcpy(&protocol, encoded.data(), sizeof(protocol));
    std::memcpy(&stage, encoded.data() + 2, sizeof(stage));
    std::memcpy(&result, encoded.data() + 3, sizeof(result));
    std::memcpy(&active, encoded.data() + 4, sizeof(active));
    std::memcpy(&pending, encoded.data() + 5, sizeof(pending));
    std::memcpy(&revision, encoded.data() + 6, sizeof(revision));
    std::memcpy(&session, encoded.data() + 14, sizeof(session));
    std::memcpy(&count, encoded.data() + 22, sizeof(count));
    REQUIRE(protocol == REQUEST_PROTOCOL_VERSION);
    REQUIRE(stage == static_cast<uint8>(StatusStage::Accepted));
    REQUIRE(result == static_cast<uint8>(RequestResult::Ok));
    REQUIRE(active == 8);
    REQUIRE(pending == 1);
    REQUIRE(revision == 7);
    REQUIRE(session == 0x1020304050607080ULL);
    REQUIRE(count == 2);

    classes.push_back(classes.front());
    REQUIRE(EncodeActiveClassStatus(StatusStage::State, RequestResult::Ok,
        8, 0, 1, 2, classes).empty());

    std::vector<ActiveClassStatusDescriptor> invalidUtf8{
        { 8, 23, CLASS_FLAG_ACTIVE, std::string("\xFF", 1) }
    };
    REQUIRE(EncodeActiveClassStatus(StatusStage::State, RequestResult::Ok,
        8, 0, 1, 2, invalidUtf8).empty());
}

TEST_CASE("Active class snapshot rejects class-family action leakage", "[ClassSwitch]")
{
    ActiveClassSnapshot snapshot = MinimalSnapshot();
    snapshot.Actions[0] = 9999;
    std::string error;
    REQUIRE_FALSE(ValidateActiveClassSnapshot(snapshot, &error));
    REQUIRE(error.find("unavailable spell") != std::string::npos);
}

TEST_CASE("Active class snapshot matches client shape constraints", "[ClassSwitch]")
{
    ActiveClassSnapshot snapshot = MinimalSnapshot();
    std::string error;
    ActiveClassSpellModifier modifier;
    modifier.Group = 1;
    modifier.Operation = 31;
    snapshot.Modifiers.push_back(modifier);
    REQUIRE_FALSE(ValidateActiveClassSnapshot(snapshot, &error));

    snapshot.Modifiers.front().Operation = 1;
    snapshot.ActiveClassAuras.push_back({ 1, 133, 0, 1, 1, 0, 0 });
    REQUIRE_FALSE(ValidateActiveClassSnapshot(snapshot, &error));

    snapshot.ActiveClassAuras.clear();
    snapshot.Pet.Mode = 1;
    REQUIRE_FALSE(ValidateActiveClassSnapshot(snapshot, &error));

    snapshot.Pet.Mode = 0;
    snapshot.SharedProgression.SharedDataVersion = 2;
    REQUIRE_FALSE(ValidateActiveClassSnapshot(snapshot, &error));
}
