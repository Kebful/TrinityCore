/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "PlayerClassSwitch.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace ClassSwitch
{
namespace
{
class ByteWriter
{
public:
    explicit ByteWriter(std::vector<uint8>& data) : _data(data) { }

    template <typename T>
    void Write(T const& value)
    {
        uint8 const* bytes = reinterpret_cast<uint8 const*>(&value);
        _data.insert(_data.end(), bytes, bytes + sizeof(T));
    }

private:
    std::vector<uint8>& _data;
};

class ByteReader
{
public:
    ByteReader(uint8 const* data, size_t size) : _data(data), _size(size), _offset(0) { }

    template <typename T>
    bool Read(T& value)
    {
        if (!_data || _offset > _size || _size - _offset < sizeof(T))
            return false;

        std::memcpy(&value, _data + _offset, sizeof(T));
        _offset += sizeof(T);
        return true;
    }

    bool ReadCount(uint32& count, uint32 maximum)
    {
        return Read(count) && count <= maximum;
    }

    bool ReadBytes(uint8* output, size_t count)
    {
        if (!output || !_data || _offset > _size || _size - _offset < count)
            return false;

        std::memcpy(output, _data + _offset, count);
        _offset += count;
        return true;
    }

    bool AtEnd() const
    {
        return _offset == _size;
    }

private:
    uint8 const* _data;
    size_t _size;
    size_t _offset;
};

bool SetError(std::string* error, char const* message)
{
    if (error)
        *error = message;
    return false;
}

template <typename T>
bool WriteBoundedCount(ByteWriter& writer, std::vector<T> const& values, uint32 maximum, std::string* error)
{
    if (values.size() > maximum)
        return SetError(error, "collection exceeds its wire limit");

    writer.Write(static_cast<uint32>(values.size()));
    return true;
}

template <typename Container>
bool AllFinite(Container const& values)
{
    for (float value : values)
        if (!std::isfinite(value))
            return false;
    return true;
}

bool IsValidUtf8(uint8 const* bytes, size_t size)
{
    size_t offset = 0;
    while (offset < size)
    {
        uint8 const first = bytes[offset++];
        if (first <= 0x7F)
            continue;

        uint8 continuationCount = 0;
        uint32 codePoint = 0;
        uint32 minimum = 0;
        if ((first & 0xE0) == 0xC0)
        {
            continuationCount = 1;
            codePoint = first & 0x1F;
            minimum = 0x80;
        }
        else if ((first & 0xF0) == 0xE0)
        {
            continuationCount = 2;
            codePoint = first & 0x0F;
            minimum = 0x800;
        }
        else if ((first & 0xF8) == 0xF0)
        {
            continuationCount = 3;
            codePoint = first & 0x07;
            minimum = 0x10000;
        }
        else
            return false;

        if (size - offset < continuationCount)
            return false;
        for (uint8 index = 0; index < continuationCount; ++index)
        {
            uint8 const continuation = bytes[offset++];
            if ((continuation & 0xC0) != 0x80)
                return false;
            codePoint = (codePoint << 6) | (continuation & 0x3F);
        }
        if (codePoint < minimum || codePoint > 0x10FFFF ||
            (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            return false;
    }
    return true;
}

std::string FormatActionError(uint8 spec, uint8 button, uint32 actionId)
{
    std::ostringstream message;
    message << "stored profile action " << uint32(button)
        << " in specialization " << uint32(spec)
        << " references unavailable spell " << actionId;
    return message.str();
}
}

uint32 ClassIdToMask(uint8 classId)
{
    if (!IsSupportedClassId(classId))
        return 0;
    return 1u << (classId - 1);
}

bool IsSupportedClassId(uint8 classId)
{
    return classId >= 1 && classId <= 32;
}

uint64 ComputeSessionTag(uint8 const* bytes, size_t size)
{
    return ComputeFnv1a64(bytes, size);
}

uint64 ComputeFnv1a64(uint8 const* bytes, size_t size)
{
    uint64 hash = 14695981039346656037ULL;
    for (size_t i = 0; bytes && i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool DecodeClientClassSwitchResponse(uint8 const* data, size_t size,
    ClientClassSwitchResponse* response, std::string* error)
{
    if (!response || !data || size > 30 + MAX_RESPONSE_REASON_BYTES)
        return SetError(error, "client class-switch response is missing or exceeds its limit");

    ByteReader reader(data, size);
    ClientClassSwitchResponse decoded;
    uint8 disposition = 0;
    uint16 reasonLength = 0;
    if (!reader.Read(decoded.Version) || !reader.Read(disposition) ||
        !reader.Read(decoded.ClassId) || !reader.Read(decoded.Session) ||
        !reader.Read(decoded.Revision) || !reader.Read(decoded.SnapshotDigest) ||
        !reader.Read(reasonLength))
        return SetError(error, "client class-switch response header is truncated");
    if (decoded.Version != RESPONSE_PROTOCOL_VERSION)
        return SetError(error, "client class-switch response version is unsupported");
    if (disposition < static_cast<uint8>(ClientResponseDisposition::Accepted) ||
        disposition > static_cast<uint8>(ClientResponseDisposition::ResyncRequested))
        return SetError(error, "client class-switch response disposition is invalid");
    if (!decoded.Session || !decoded.Revision || !IsSupportedClassId(decoded.ClassId))
        return SetError(error, "client class-switch response identity is invalid");
    if (reasonLength > MAX_RESPONSE_REASON_BYTES)
        return SetError(error, "client class-switch response reason exceeds its limit");

    decoded.Disposition = ClientResponseDisposition(disposition);
    if (decoded.Disposition == ClientResponseDisposition::ResyncRequested)
    {
        if (decoded.SnapshotDigest)
            return SetError(error, "class-switch resync responses must not carry a snapshot digest");
    }
    else if (!decoded.SnapshotDigest)
        return SetError(error, "class-switch acceptance responses require a snapshot digest");

    decoded.Reason.resize(reasonLength);
    if (reasonLength &&
        !reader.ReadBytes(reinterpret_cast<uint8*>(decoded.Reason.data()), reasonLength))
        return SetError(error, "client class-switch response reason is truncated");
    if (!reader.AtEnd())
        return SetError(error, "client class-switch response has trailing bytes");
    if (!IsValidUtf8(reinterpret_cast<uint8 const*>(decoded.Reason.data()), decoded.Reason.size()))
        return SetError(error, "client class-switch response reason is not valid UTF-8");

    *response = std::move(decoded);
    return true;
}

ClientResponseMatch MatchClientClassSwitchResponse(ClientClassSwitchResponse const& response,
    uint64 expectedSession, uint8 expectedClass, uint64 expectedRevision, uint64 expectedDigest,
    uint8 sourceClass, uint64 sourceRevision, uint8 completedClass, uint64 completedRevision,
    uint64 completedDigest)
{
    if (!expectedSession || response.Session != expectedSession)
        return ClientResponseMatch::StaleOrMismatched;
    if (response.Disposition == ClientResponseDisposition::Accepted &&
        completedRevision && completedDigest && response.Revision == completedRevision &&
        response.ClassId == completedClass && response.SnapshotDigest == completedDigest)
        return ClientResponseMatch::DuplicateAccepted;
    if (response.Disposition == ClientResponseDisposition::ResyncRequested)
    {
        return !response.SnapshotDigest && response.ClassId == sourceClass &&
            response.Revision == sourceRevision
            ? ClientResponseMatch::ResyncRequested
            : ClientResponseMatch::StaleOrMismatched;
    }
    if (response.ClassId != expectedClass || response.Revision != expectedRevision ||
        response.SnapshotDigest != expectedDigest)
        return ClientResponseMatch::StaleOrMismatched;
    return response.Disposition == ClientResponseDisposition::Accepted
        ? ClientResponseMatch::Accepted : ClientResponseMatch::Rejected;
}

ClientTimeoutAction EvaluateClassSwitchTimeout(bool pending, bool recoveringSource,
    bool deadlineReached, uint8 recoveryAttempts)
{
    if (!pending || !deadlineReached)
        return ClientTimeoutAction::None;
    if (recoveringSource ||
        recoveryAttempts >= MAX_CLASS_SWITCH_RECOVERY_ATTEMPTS)
        return ClientTimeoutAction::Disconnect;
    return ClientTimeoutAction::RecoverSource;
}

bool ReserveTransitionRevision(uint64 baseRevision, uint64 targetRevision, uint64* reserved)
{
    if (!reserved || !baseRevision || !targetRevision)
        return false;
    uint64 const maximum = std::max(baseRevision, targetRevision);
    if (maximum == std::numeric_limits<uint64>::max())
        return false;
    *reserved = maximum + 1;
    return true;
}

bool IsSwitchOwnedAura(uint64 playerGuid, uint64 creatorGuid, uint32 classFamily,
    uint32 spellFamily, bool shapeshift)
{
    return playerGuid && creatorGuid == playerGuid &&
        ((classFamily && spellFamily == classFamily) || shapeshift);
}

RequestResult EvaluateSelectionPolicy(SelectionPolicy const& policy)
{
    if (policy.Pending)
        return RequestResult::Busy;
    if (policy.InCombat || policy.Casting)
        return RequestResult::NotReady;
    if (policy.SameClass)
        return RequestResult::TargetUnchanged;
    if (!policy.ActiveRevision || policy.ClientRevision != policy.ActiveRevision)
        return RequestResult::StaleRevision;
    return RequestResult::Ok;
}

bool IsSpellAllowedForClassProfile(uint32 spellFamily, uint32 classFamily, bool professionOrRiding)
{
    return !spellFamily || professionOrRiding || spellFamily == classFamily;
}

bool IsSharedClassProfileSpell(uint32 spellFamily, bool profession, bool riding)
{
    return profession || (!spellFamily && !riding);
}

uint8 ResolveClassStartLevel(uint8 classId, uint8 configuredClassInitialLevel,
    uint8 configuredPlayerStartLevel, uint8 configuredDeathKnightStartLevel)
{
    if (classId == CLASS_DEATH_KNIGHT)
        return configuredDeathKnightStartLevel;
    if (configuredClassInitialLevel)
        return configuredClassInitialLevel;
    return configuredPlayerStartLevel;
}

uint16 ResolveClassStatusLevel(uint8 classId, bool hasPersistedProfile, uint16 persistedLevel,
    uint8 configuredClassInitialLevel, uint8 configuredPlayerStartLevel,
    uint8 configuredDeathKnightStartLevel)
{
    if (hasPersistedProfile && persistedLevel)
        return persistedLevel;
    return ResolveClassStartLevel(classId, configuredClassInitialLevel,
        configuredPlayerStartLevel, configuredDeathKnightStartLevel);
}

uint32 NormalizeActionSpellRank(uint32 requestedRank, std::vector<uint32> const& spellChain,
    std::set<uint32> const& activeRanks)
{
    if (!requestedRank)
        return 0;
    uint32 selected = 0;
    for (uint32 rank : spellChain)
    {
        if (!activeRanks.count(rank))
            continue;
        selected = rank;
        if (rank == requestedRank)
            return rank;
    }
    return selected;
}

void StripClassOwnedLanguages(StoredClassProfile* profile,
    std::set<uint32> const& languageSpellIds, std::set<uint32> const& languageSkillIds,
    uint32* removedSpells, uint32* removedSkills)
{
    if (removedSpells)
        *removedSpells = 0;
    if (removedSkills)
        *removedSkills = 0;
    if (!profile)
        return;

    size_t const oldSpellCount = profile->Spells.size();
    profile->Spells.erase(std::remove_if(profile->Spells.begin(), profile->Spells.end(),
        [&languageSpellIds](SpellProfile const& spell) {
            return languageSpellIds.find(spell.Id) != languageSpellIds.end();
        }), profile->Spells.end());
    size_t const oldSkillCount = profile->Skills.size();
    profile->Skills.erase(std::remove_if(profile->Skills.begin(), profile->Skills.end(),
        [&languageSkillIds](SkillProfile const& skill) {
            return languageSkillIds.find(skill.Id) != languageSkillIds.end();
        }), profile->Skills.end());
    if (removedSpells)
        *removedSpells = static_cast<uint32>(oldSpellCount - profile->Spells.size());
    if (removedSkills)
        *removedSkills = static_cast<uint32>(oldSkillCount - profile->Skills.size());
}

void MergeCharacterLanguages(StoredClassProfile* profile,
    CharacterLanguageState const& languages,
    std::set<uint32> const& languageSpellIds, std::set<uint32> const& languageSkillIds)
{
    if (!profile)
        return;
    StripClassOwnedLanguages(profile, languageSpellIds, languageSkillIds);

    for (SpellProfile const& language : languages.Spells)
    {
        if (languageSpellIds.find(language.Id) == languageSpellIds.end())
            continue;
        if (std::none_of(profile->Spells.begin(), profile->Spells.end(),
            [&language](SpellProfile const& value) { return value.Id == language.Id; }))
            profile->Spells.push_back(language);
    }
    for (SkillProfile const& language : languages.Skills)
    {
        if (languageSkillIds.find(language.Id) == languageSkillIds.end())
            continue;
        if (std::none_of(profile->Skills.begin(), profile->Skills.end(),
            [&language](SkillProfile const& value) { return value.Id == language.Id; }))
            profile->Skills.push_back(language);
    }
}

bool PlanEquipmentBagReservations(std::vector<EquipmentBagCandidate> const& candidates,
    std::vector<EquipmentBagDemand> const& demands, std::vector<uint16>* destinations)
{
    if (!destinations)
        return false;
    destinations->clear();
    destinations->reserve(demands.size());

    std::map<uint16, size_t> candidateIndexes;
    for (size_t index = 0; index < candidates.size(); ++index)
        if (!candidates[index].Position ||
            !candidateIndexes.emplace(candidates[index].Position, index).second)
            return false;

    std::vector<size_t> demandForPosition(candidates.size(), std::numeric_limits<size_t>::max());
    std::vector<size_t> positionForDemand(demands.size(), std::numeric_limits<size_t>::max());

    std::function<bool(size_t, std::set<size_t>&)> assign = [&](size_t demandIndex,
        std::set<size_t>& seen) {
        for (uint16 position : demands[demandIndex].AllowedPositions)
        {
            auto candidate = candidateIndexes.find(position);
            if (candidate == candidateIndexes.end())
                continue;
            size_t const candidateIndex = candidate->second;
            if (!seen.insert(candidateIndex).second)
                continue;
            if (demandForPosition[candidateIndex] == std::numeric_limits<size_t>::max() ||
                assign(demandForPosition[candidateIndex], seen))
            {
                demandForPosition[candidateIndex] = demandIndex;
                positionForDemand[demandIndex] = candidateIndex;
                return true;
            }
        }
        return false;
    };

    for (size_t demandIndex = 0; demandIndex < demands.size(); ++demandIndex)
    {
        std::set<size_t> seen;
        if (!assign(demandIndex, seen))
        {
            destinations->clear();
            return false;
        }
    }

    for (size_t candidateIndex : positionForDemand)
        destinations->push_back(candidates[candidateIndex].Position);
    return true;
}

std::vector<uint8> BuildChangedEquipmentSlots(
    std::array<uint64, EQUIPMENT_SLOTS> const& current,
    std::array<uint64, EQUIPMENT_SLOTS> const& target)
{
    std::vector<uint8> changed;
    for (uint8 slot = 0; slot < EQUIPMENT_SLOTS; ++slot)
        if (current[slot] != target[slot])
            changed.push_back(slot);
    return changed;
}

ClassSwitchLogoutPlan PlanClassSwitchLogout(bool hadPendingTransition,
    bool sourceRestoreSucceeded)
{
    ClassSwitchLogoutPlan plan;
    if (hadPendingTransition && !sourceRestoreSucceeded)
    {
        plan.PersistPlayer = false;
        plan.RemovePet = false;
    }
    return plan;
}

bool IsClassSwitchGameplayBlocked(bool pendingTransition, bool controlPacket)
{
    return pendingTransition && !controlPacket;
}

bool ValidateStoredProfile(StoredClassProfile const& profile, std::string* error,
    bool validateActionSpells)
{
    if (profile.Version != PROFILE_VERSION)
        return SetError(error, "unsupported stored profile version");
    if (!profile.Revision)
        return SetError(error, "stored profile revision is missing");
    if (!IsSupportedClassId(profile.ClassId))
        return SetError(error, "stored profile class is outside the uint32 class-mask range");
    if (!profile.Level || profile.Level > 80)
        return SetError(error, "stored profile level is invalid");
    if (profile.NextLevelXp && profile.Xp > profile.NextLevelXp)
        return SetError(error, "stored profile XP exceeds its next-level requirement");
    if (!profile.SpecCount || profile.SpecCount > TALENT_GROUPS || profile.ActiveSpec >= profile.SpecCount)
        return SetError(error, "stored profile specialization metadata is invalid");
    if (!std::isfinite(profile.HealthPercent) || profile.HealthPercent < 0.0f || profile.HealthPercent > 100.0f)
        return SetError(error, "stored profile health percentage is invalid");
    if (!AllFinite(profile.PowerPercent))
        return SetError(error, "stored profile power percentage is invalid");
    for (float value : profile.PowerPercent)
        if (value < 0.0f || value > 100.0f)
            return SetError(error, "stored profile power percentage is out of range");
    if (profile.Runes.Count > RUNE_SLOTS)
        return SetError(error, "stored profile rune count is invalid");
    for (size_t rune = 0; rune < RUNE_SLOTS; ++rune)
    {
        if (profile.Runes.Types[rune] < 0 || profile.Runes.Types[rune] > 3 ||
            profile.Runes.BaseTypes[rune] < 0 || profile.Runes.BaseTypes[rune] > 3)
            return SetError(error, "stored profile rune type is invalid");
        if (rune >= profile.Runes.Count &&
            (profile.Runes.Types[rune] || profile.Runes.BaseTypes[rune] ||
             profile.Runes.Cooldowns[rune] || profile.Runes.LastUsed[rune]))
            return SetError(error, "stored profile retains rune state beyond its count");
    }

    if (profile.Spells.size() > MAX_SPELLS || profile.Skills.size() > MAX_SKILLS)
        return SetError(error, "stored profile collection exceeds its limit");

    std::set<uint32> spellIds;
    std::set<uint32> knownSpellIds;
    for (SpellProfile const& spell : profile.Spells)
    {
        if (!spell.Id || spell.State > 4 || !spellIds.insert(spell.Id).second)
        {
            if (error)
                *error = "stored profile spell " + std::to_string(spell.Id)
                    + " has an invalid state or is duplicated";
            return false;
        }
        knownSpellIds.insert(spell.Id);
    }

    std::set<uint32> skillIds;
    for (SkillProfile const& skill : profile.Skills)
    {
        if (!skill.Id || skill.Value > skill.Max || !skillIds.insert(skill.Id).second)
        {
            if (error)
                *error = "stored profile skill " + std::to_string(skill.Id)
                    + " is invalid or duplicated";
            return false;
        }
    }

    for (uint8 group = 0; group < profile.SpecCount; ++group)
    {
        TalentGroupProfile const& spec = profile.Groups[group];
        if (spec.Talents.size() > MAX_TALENTS)
            return SetError(error, "stored profile talent count exceeds its limit");

        std::set<uint32> talentIds;
        for (TalentProfile const& talent : spec.Talents)
        {
            if (!talent.SpellId || talent.Spec != group || !talentIds.insert(talent.SpellId).second)
                return SetError(error, "stored profile talent is invalid or duplicated");
        }

        for (uint8 button = 0; button < ACTION_SLOTS; ++button)
        {
            uint32 const action = spec.Actions[button];
            uint32 const actionId = action & 0x00FFFFFFu;
            uint8 const type = uint8(action >> 24);
            if (validateActionSpells && actionId && type == 0 &&
                knownSpellIds.find(actionId) == knownSpellIds.end())
                return SetError(error, FormatActionError(group, button, actionId).c_str());
        }
    }

    std::set<uint64> equipmentGuids;
    for (uint8 slot = 0; slot < EQUIPMENT_SLOTS; ++slot)
    {
        StoredEquipmentItem const& item = profile.Equipment[slot];
        if ((item.ItemId != 0) != (item.EquippedGuid != 0) ||
            (!item.EquippedGuid &&
             (item.Enchantments[0] || item.Enchantments[1])) ||
            (item.EquippedGuid && !equipmentGuids.insert(item.EquippedGuid).second))
        {
            if (error)
                *error = "stored profile equipment slot " + std::to_string(slot)
                    + " is invalid or duplicated";
            return false;
        }
    }

    return true;
}

std::vector<uint8> EncodeStoredProfile(StoredClassProfile const& profile)
{
    std::string error;
    if (!ValidateStoredProfile(profile, &error))
        return {};

    std::vector<uint8> data;
    data.reserve(64 * 1024);
    ByteWriter writer(data);
    writer.Write(PROFILE_MAGIC);
    writer.Write(profile.Version);
    writer.Write(profile.Revision);
    writer.Write(profile.ClassId);
    writer.Write(profile.Level);
    writer.Write(profile.Xp);
    writer.Write(profile.NextLevelXp);
    writer.Write(profile.ActiveSpec);
    writer.Write(profile.SpecCount);
    writer.Write(profile.FreeTalentPoints);
    writer.Write(profile.HealthPercent);
    for (float value : profile.PowerPercent)
        writer.Write(value);
    writer.Write(profile.Runes.Count);
    for (size_t rune = 0; rune < RUNE_SLOTS; ++rune)
    {
        writer.Write(profile.Runes.Types[rune]);
        writer.Write(profile.Runes.BaseTypes[rune]);
        writer.Write(profile.Runes.Cooldowns[rune]);
        writer.Write(profile.Runes.LastUsed[rune]);
    }

    writer.Write(static_cast<uint32>(profile.Spells.size()));
    for (SpellProfile const& spell : profile.Spells)
    {
        writer.Write(spell.Id);
        writer.Write(spell.State);
        writer.Write(static_cast<uint8>(spell.Active ? 1 : 0));
        writer.Write(static_cast<uint8>(spell.Dependent ? 1 : 0));
        writer.Write(static_cast<uint8>(spell.Disabled ? 1 : 0));
    }

    writer.Write(static_cast<uint32>(profile.Skills.size()));
    for (SkillProfile const& skill : profile.Skills)
    {
        writer.Write(skill.Id);
        writer.Write(skill.Step);
        writer.Write(skill.Value);
        writer.Write(skill.Max);
        writer.Write(skill.TemporaryBonus);
        writer.Write(skill.PermanentBonus);
        writer.Write(static_cast<uint8>(skill.Shared ? 1 : 0));
    }

    for (uint8 group = 0; group < profile.SpecCount; ++group)
    {
        TalentGroupProfile const& spec = profile.Groups[group];
        writer.Write(static_cast<uint32>(spec.Talents.size()));
        for (TalentProfile const& talent : spec.Talents)
        {
            writer.Write(talent.SpellId);
            writer.Write(talent.Spec);
        }
        for (uint32 glyph : spec.Glyphs)
            writer.Write(glyph);
        for (uint32 action : spec.Actions)
            writer.Write(action);
    }
    for (StoredEquipmentItem const& item : profile.Equipment)
    {
        writer.Write(item.ItemId);
        writer.Write(item.Enchantments[0]);
        writer.Write(item.Enchantments[1]);
        writer.Write(item.EquippedGuid);
    }

    return data;
}

bool DecodeStoredProfile(uint8 const* data, size_t size, StoredClassProfile* profile, std::string* error)
{
    if (!profile || !data || size > 1024 * 1024)
        return SetError(error, "stored profile payload is invalid");

    ByteReader reader(data, size);
    uint32 magic = 0;
    StoredClassProfile decoded;
    uint16 encodedVersion = 0;
    if (!reader.Read(magic) || magic != PROFILE_MAGIC ||
        !reader.Read(encodedVersion) || (encodedVersion != 1 && encodedVersion != PROFILE_VERSION) ||
        !reader.Read(decoded.Revision) ||
        !reader.Read(decoded.ClassId) ||
        !reader.Read(decoded.Level) ||
        !reader.Read(decoded.Xp) ||
        !reader.Read(decoded.NextLevelXp) ||
        !reader.Read(decoded.ActiveSpec) ||
        !reader.Read(decoded.SpecCount) ||
        !reader.Read(decoded.FreeTalentPoints) ||
        !reader.Read(decoded.HealthPercent))
        return SetError(error, "stored profile header is invalid");
    decoded.Version = PROFILE_VERSION;

    for (float& value : decoded.PowerPercent)
        if (!reader.Read(value))
            return SetError(error, "stored profile powers are truncated");
    if (!reader.Read(decoded.Runes.Count))
        return SetError(error, "stored profile rune count is truncated");
    for (size_t rune = 0; rune < RUNE_SLOTS; ++rune)
        if (!reader.Read(decoded.Runes.Types[rune]) ||
            !reader.Read(decoded.Runes.BaseTypes[rune]) ||
            !reader.Read(decoded.Runes.Cooldowns[rune]) ||
            !reader.Read(decoded.Runes.LastUsed[rune]))
            return SetError(error, "stored profile rune state is truncated");

    uint32 count = 0;
    if (!reader.ReadCount(count, MAX_SPELLS))
        return SetError(error, "stored profile spell count is invalid");
    decoded.Spells.resize(count);
    for (SpellProfile& spell : decoded.Spells)
    {
        uint8 active = 0;
        uint8 dependent = 0;
        uint8 disabled = 0;
        if (!reader.Read(spell.Id) || !reader.Read(spell.State) ||
            !reader.Read(active) || !reader.Read(dependent) || !reader.Read(disabled) ||
            active > 1 || dependent > 1 || disabled > 1)
            return SetError(error, "stored profile spell is truncated");
        spell.Active = active != 0;
        spell.Dependent = dependent != 0;
        spell.Disabled = disabled != 0;
    }

    if (!reader.ReadCount(count, MAX_SKILLS))
        return SetError(error, "stored profile skill count is invalid");
    decoded.Skills.resize(count);
    for (SkillProfile& skill : decoded.Skills)
    {
        uint8 shared = 0;
        if (!reader.Read(skill.Id) || !reader.Read(skill.Step) || !reader.Read(skill.Value) ||
            !reader.Read(skill.Max) || !reader.Read(skill.TemporaryBonus) ||
            !reader.Read(skill.PermanentBonus) || !reader.Read(shared) || shared > 1)
            return SetError(error, "stored profile skill is truncated");
        skill.Shared = shared != 0;
    }

    for (uint8 group = 0; group < decoded.SpecCount; ++group)
    {
        if (group >= TALENT_GROUPS)
            return SetError(error, "stored profile specialization count is invalid");

        TalentGroupProfile& spec = decoded.Groups[group];
        if (!reader.ReadCount(count, MAX_TALENTS))
            return SetError(error, "stored profile talent count is invalid");
        spec.Talents.resize(count);
        for (TalentProfile& talent : spec.Talents)
            if (!reader.Read(talent.SpellId) || !reader.Read(talent.Spec))
                return SetError(error, "stored profile talent is truncated");
        for (uint32& glyph : spec.Glyphs)
            if (!reader.Read(glyph))
                return SetError(error, "stored profile glyph is truncated");
        for (uint32& action : spec.Actions)
            if (!reader.Read(action))
                return SetError(error, "stored profile action is truncated");
    }

    // v1 did not carry class-owned equipment. Its explicit migration is an
    // empty loadout; the active class captures its real equipment on the next
    // profile write, while target classes never inherit the source gear.
    if (encodedVersion == PROFILE_VERSION)
    {
        for (StoredEquipmentItem& item : decoded.Equipment)
            if (!reader.Read(item.ItemId) ||
                !reader.Read(item.Enchantments[0]) ||
                !reader.Read(item.Enchantments[1]) ||
                !reader.Read(item.EquippedGuid))
                return SetError(error, "stored profile equipment is truncated");
    }
    else
        decoded.EquipmentMigrated = true;

    if (!reader.AtEnd() || !ValidateStoredProfile(decoded, error, false))
        return false;

    *profile = std::move(decoded);
    return true;
}

std::vector<std::string> BuildProfileBackingTableStatements(uint64 guid,
    StoredClassProfile const& profile, std::vector<uint32> const& languageSpellIds,
    std::vector<uint32> const& languageSkillIds)
{
    std::vector<std::string> statements;
    std::ostringstream sql;
    sql << "DELETE FROM character_spell WHERE guid = " << guid;
    if (!languageSpellIds.empty())
    {
        sql << " AND spell NOT IN (";
        for (size_t index = 0; index < languageSpellIds.size(); ++index)
        {
            if (index)
                sql << ", ";
            sql << languageSpellIds[index];
        }
        sql << ')';
    }
    statements.push_back(sql.str());
    for (SpellProfile const& spell : profile.Spells)
    {
        if (std::find(languageSpellIds.begin(), languageSpellIds.end(), spell.Id)
            != languageSpellIds.end())
            continue;
        if (spell.Dependent)
            continue;
        sql.str("");
        sql.clear();
        sql << "INSERT INTO character_spell (guid, spell, active, disabled) VALUES ("
            << guid << ", " << spell.Id << ", " << uint32(spell.Active ? 1 : 0)
            << ", " << uint32(spell.Disabled ? 1 : 0) << ")";
        statements.push_back(sql.str());
    }

    sql.str("");
    sql.clear();
    sql << "DELETE FROM character_skills WHERE guid = " << guid;
    if (!languageSkillIds.empty())
    {
        sql << " AND skill NOT IN (";
        for (size_t index = 0; index < languageSkillIds.size(); ++index)
        {
            if (index)
                sql << ", ";
            sql << languageSkillIds[index];
        }
        sql << ')';
    }
    statements.push_back(sql.str());
    for (SkillProfile const& skill : profile.Skills)
    {
        if (std::find(languageSkillIds.begin(), languageSkillIds.end(), skill.Id)
            != languageSkillIds.end())
            continue;
        sql.str("");
        sql.clear();
        sql << "INSERT INTO character_skills (guid, skill, value, max) VALUES ("
            << guid << ", " << skill.Id << ", " << skill.Value << ", "
            << skill.Max << ")";
        statements.push_back(sql.str());
    }

    sql.str("");
    sql.clear();
    sql << "DELETE FROM character_action WHERE guid = " << guid;
    statements.push_back(sql.str());
    for (uint8 spec = 0; spec < profile.SpecCount; ++spec)
    {
        for (uint8 button = 0; button < ACTION_SLOTS; ++button)
        {
            uint32 const packed = profile.Groups[spec].Actions[button];
            if (!packed)
                continue;
            sql.str("");
            sql.clear();
            sql << "INSERT INTO character_action (guid, spec, button, action, type) VALUES ("
                << guid << ", " << uint32(spec) << ", " << uint32(button)
                << ", " << (packed & 0x00FFFFFFu) << ", "
                << uint32(uint8(packed >> 24)) << ")";
            statements.push_back(sql.str());
        }
    }

    sql.str("");
    sql.clear();
    sql << "DELETE FROM character_talent WHERE guid = " << guid;
    statements.push_back(sql.str());
    sql.str("");
    sql.clear();
    sql << "DELETE FROM character_glyphs WHERE guid = " << guid;
    statements.push_back(sql.str());

    for (uint8 spec = 0; spec < profile.SpecCount; ++spec)
    {
        for (TalentProfile const& talent : profile.Groups[spec].Talents)
        {
            sql.str("");
            sql.clear();
            sql << "INSERT INTO character_talent (guid, spell, talentGroup) VALUES ("
                << guid << ", " << talent.SpellId << ", " << uint32(spec) << ")";
            statements.push_back(sql.str());
        }
        sql.str("");
        sql.clear();
        sql << "INSERT INTO character_glyphs "
            << "(guid, talentGroup, glyph1, glyph2, glyph3, glyph4, glyph5, glyph6) VALUES ("
            << guid << ", " << uint32(spec);
        for (uint32 glyph : profile.Groups[spec].Glyphs)
            sql << ", " << glyph;
        sql << ")";
        statements.push_back(sql.str());
    }
    return statements;
}

std::vector<uint8> EncodeActiveClassStatus(StatusStage stage, RequestResult result,
    uint8 activeClassId, uint8 pendingClassId, uint64 revision, uint64 session,
    std::vector<ActiveClassStatusDescriptor> const& classes)
{
    if (stage < StatusStage::State || stage > StatusStage::Confirmed ||
        activeClassId > 32 || pendingClassId > 32 || !revision || !session ||
        classes.size() > 32)
        return {};

    std::set<uint8> classIds;
    for (ActiveClassStatusDescriptor const& entry : classes)
    {
        if (!IsSupportedClassId(entry.ClassId) || !entry.Level || entry.Level > 80 ||
            (entry.Flags & ~(CLASS_FLAG_ACTIVE | CLASS_FLAG_PENDING | CLASS_FLAG_AVAILABLE)) ||
            entry.Name.empty() || entry.Name.size() > std::numeric_limits<uint16>::max() ||
            !IsValidUtf8(reinterpret_cast<uint8 const*>(entry.Name.data()), entry.Name.size()) ||
            !classIds.insert(entry.ClassId).second)
            return {};
    }

    std::vector<uint8> payload;
    ByteWriter writer(payload);
    writer.Write(REQUEST_PROTOCOL_VERSION);
    writer.Write(static_cast<uint8>(stage));
    writer.Write(static_cast<uint8>(result));
    writer.Write(activeClassId);
    writer.Write(pendingClassId);
    writer.Write(revision);
    writer.Write(session);
    writer.Write(static_cast<uint8>(classes.size()));
    for (ActiveClassStatusDescriptor const& entry : classes)
    {
        writer.Write(entry.ClassId);
        writer.Write(entry.Level);
        writer.Write(entry.Flags);
        writer.Write(static_cast<uint16>(entry.Name.size()));
        payload.insert(payload.end(), entry.Name.begin(), entry.Name.end());
    }
    return payload;
}

bool ValidateActiveClassSnapshot(ActiveClassSnapshot const& snapshot, std::string* error)
{
    if (snapshot.Version != SNAPSHOT_VERSION || snapshot.SectionMask != SNAPSHOT_SECTION_REQUIRED)
        return SetError(error, "snapshot version or section mask is invalid");
    if (snapshot.SharedProgression.SharedDataVersion != 1)
        return SetError(error, "snapshot shared progression version is invalid");
    if (!snapshot.Session || !snapshot.Revision || !snapshot.Guid || !snapshot.Race ||
        !IsSupportedClassId(snapshot.ClassId) || !snapshot.Level)
        return SetError(error, "snapshot identity, class, race, or level is missing");
    if (snapshot.DisplayPower >= POWER_TYPES || snapshot.Resources.PowerType >= POWER_TYPES)
        return SetError(error, "snapshot power type is invalid");
    for (uint8 power = 0; power < POWER_TYPES; ++power)
    {
        if (snapshot.Resources.Powers[power] < 0 || snapshot.Resources.MaxPowers[power] < 0 ||
            (snapshot.Resources.MaxPowers[power] &&
             snapshot.Resources.Powers[power] > snapshot.Resources.MaxPowers[power]) ||
            snapshot.Resources.PredictedPowers[power] < -1)
            return SetError(error, "snapshot power state is invalid");
    }
    if (snapshot.Resources.MaxPowers[snapshot.Resources.PowerType] <= 0)
        return SetError(error, "snapshot active power maximum is missing");
    if (!AllFinite(snapshot.Resources.RegenerationFlat) ||
        !AllFinite(snapshot.Resources.RegenerationInterruptedFlat) ||
        !AllFinite(snapshot.Resources.RuneRegeneration))
        return SetError(error, "snapshot regeneration state is not finite");
    if (snapshot.ComboPoints > 5 ||
        (!snapshot.ComboPoints && snapshot.ComboTargetGuid) ||
        (snapshot.ComboPoints && !snapshot.ComboTargetGuid))
        return SetError(error, "snapshot combo-point state is invalid");
    if (snapshot.Health <= 0 || snapshot.MaxHealth <= 0 || snapshot.Health > snapshot.MaxHealth)
        return SetError(error, "snapshot health state is invalid");

    if (!std::isfinite(snapshot.ExternalStats.AttackPowerMultiplier) ||
        !std::isfinite(snapshot.ExternalStats.RangedAttackPowerMultiplier) ||
        !std::isfinite(snapshot.ExternalStats.BlockPercentage) ||
        !std::isfinite(snapshot.ExternalStats.DodgePercentage) ||
        !std::isfinite(snapshot.ExternalStats.ParryPercentage) ||
        !std::isfinite(snapshot.ExternalStats.CritPercentage) ||
        !std::isfinite(snapshot.ExternalStats.RangedCritPercentage) ||
        !std::isfinite(snapshot.ExternalStats.OffhandCritPercentage) ||
        !std::isfinite(snapshot.ExternalStats.ShieldBlockCritPercentage) ||
        !AllFinite(snapshot.ExternalStats.PowerCostMultipliers) ||
        !AllFinite(snapshot.ExternalStats.SpellCritPercentages))
        return SetError(error, "snapshot external stats contain a non-finite value");

    if (snapshot.Spells.size() > MAX_SPELLS || snapshot.Modifiers.size() > MAX_MODIFIERS ||
        snapshot.Cooldowns.size() > MAX_COOLDOWNS || snapshot.Skills.size() > MAX_SKILLS ||
        snapshot.ActiveClassAuras.size() > ACTIVE_CLASS_MAX_AURAS ||
        snapshot.Pet.SpellActions.size() > MAX_PET_SPELLS)
        return SetError(error, "snapshot collection exceeds its limit");
    if (snapshot.Resources.Runes.Count > RUNE_SLOTS)
        return SetError(error, "snapshot rune count is invalid");
    uint32 const runeMask = snapshot.Resources.Runes.Count
        ? (1u << snapshot.Resources.Runes.Count) - 1u : 0;
    if ((!snapshot.Resources.Runes.Count && snapshot.Resources.Runes.PredictedMask) ||
        (snapshot.Resources.Runes.PredictedMask != 0xFFFFFFFFu &&
         (snapshot.Resources.Runes.PredictedMask & ~runeMask)))
        return SetError(error, "snapshot predicted rune mask is invalid");
    for (uint8 rune = 0; rune < RUNE_SLOTS; ++rune)
    {
        if (snapshot.Resources.Runes.Types[rune] < 0 || snapshot.Resources.Runes.Types[rune] > 3 ||
            snapshot.Resources.Runes.BaseTypes[rune] < 0 ||
            snapshot.Resources.Runes.BaseTypes[rune] > 3)
            return SetError(error, "snapshot rune types are invalid");
        if (rune >= snapshot.Resources.Runes.Count &&
            (snapshot.Resources.Runes.Types[rune] || snapshot.Resources.Runes.BaseTypes[rune] ||
             snapshot.Resources.Runes.Cooldowns[rune] || snapshot.Resources.Runes.LastUsed[rune]))
            return SetError(error, "snapshot retains rune state beyond its rune count");
    }

    std::set<int32> knownSpells;
    for (ActiveClassSpell const& spell : snapshot.Spells)
        if (spell.Id <= 0 || spell.Slot < 0 || spell.Slot > std::numeric_limits<uint16>::max() ||
            !knownSpells.insert(spell.Id).second)
            return SetError(error, "snapshot spell is invalid or duplicated");

    std::set<std::tuple<uint8, uint8, bool>> modifierKeys;
    for (ActiveClassSpellModifier const& modifier : snapshot.Modifiers)
        if (modifier.Group >= 96 || modifier.Operation >= 31 ||
            !modifierKeys.emplace(modifier.Group, modifier.Operation, modifier.Percent).second)
            return SetError(error, "snapshot spell modifier is out of range or duplicated");

    if (snapshot.TalentGroupCount > TALENT_GROUPS ||
        (snapshot.TalentGroupCount == 0 && snapshot.ActiveTalentGroup != 0) ||
        (snapshot.TalentGroupCount && snapshot.ActiveTalentGroup >= snapshot.TalentGroupCount))
        return SetError(error, "snapshot talent group metadata is invalid");

    for (uint32 group = 0; group < snapshot.TalentGroupCount; ++group)
    {
        if (snapshot.TalentGroups[group].Talents.size() > MAX_TALENTS)
            return SetError(error, "snapshot talent count exceeds its limit");
        std::set<int32> talentIds;
        for (ActiveClassTalent const& talent : snapshot.TalentGroups[group].Talents)
            if (talent.Id <= 0 || talent.Rank > 8 || !talentIds.insert(talent.Id).second)
                return SetError(error, "snapshot talent is invalid or duplicated");
    }

    for (uint32 action : snapshot.Actions)
    {
        if (action && (action & 0xF0000000u) == 0 &&
            knownSpells.find(static_cast<int32>(action)) == knownSpells.end())
            return SetError(error, "snapshot action references an unavailable spell");
    }

    std::set<std::tuple<int32, uint32, uint8>> cooldownKeys;
    for (ActiveClassCooldown const& cooldown : snapshot.Cooldowns)
        if (cooldown.SpellId <= 0 || cooldown.Pet > 1 ||
            cooldown.CooldownLeft > uint32(std::numeric_limits<int32>::max()) ||
            cooldown.CategoryCooldownLeft > uint32(std::numeric_limits<int32>::max()) ||
            !cooldownKeys.emplace(cooldown.SpellId, cooldown.ItemId, cooldown.Pet).second)
            return SetError(error, "snapshot cooldown is invalid or duplicated");

    std::set<int32> skillIds;
    for (ActiveClassSkill const& skill : snapshot.Skills)
        if (skill.Id <= 0 || skill.MaxRank < skill.Rank || !skillIds.insert(skill.Id).second)
            return SetError(error, "snapshot skill is invalid or duplicated");

    std::set<uint32> sharedSpellIds;
    for (uint32 spellId : snapshot.SharedProgression.SpellIds)
        if (knownSpells.find(static_cast<int32>(spellId)) == knownSpells.end() ||
            !sharedSpellIds.insert(spellId).second)
            return SetError(error, "snapshot shared spell is invalid or duplicated");

    std::set<int32> auraSpellIds;
    for (ActiveClassAura const& aura : snapshot.ActiveClassAuras)
    {
        uint64 const durationMs = static_cast<uint64>(std::max(aura.Duration, 0)) * 1000u;
        if (!aura.CreatorGuid || aura.SpellId <= 0 || !(aura.Flags & 0x7) || aura.Duration < 0 ||
            !aura.Level || !aura.StackCount ||
            !auraSpellIds.insert(aura.SpellId).second ||
            (aura.Duration && aura.RemainingMilliseconds > durationMs))
            return SetError(error, "snapshot aura state is invalid or duplicated");
    }

    if (!snapshot.Pet.Guid)
    {
        if (snapshot.Pet.LinkType || snapshot.Pet.ExpirationRemainingMilliseconds || snapshot.Pet.Mode ||
            !snapshot.Pet.SpellActions.empty() ||
            std::any_of(snapshot.Pet.Actions.begin(), snapshot.Pet.Actions.end(),
                [](uint32 action) { return action != 0; }))
            return SetError(error, "snapshot pet state exists without a pet");
    }
    else if (snapshot.Pet.LinkType != 1 && snapshot.Pet.LinkType != 2)
        return SetError(error, "snapshot pet link type is invalid");

    for (ActiveClassEquipmentItem const& item : snapshot.Equipment)
        if ((item.ItemId != 0) != (item.EquippedGuid != 0))
            return SetError(error, "snapshot equipment item and GUID are inconsistent");

    return true;
}

std::vector<uint8> EncodeActiveClassSnapshot(ActiveClassSnapshot const& snapshot)
{
    std::string error;
    if (!ValidateActiveClassSnapshot(snapshot, &error))
        return {};

    std::vector<uint8> data;
    data.reserve(64 * 1024);
    ByteWriter writer(data);
    writer.Write(SNAPSHOT_MAGIC);
    writer.Write(snapshot.Version);
    writer.Write(snapshot.SectionMask);
    writer.Write(snapshot.Session);
    writer.Write(snapshot.Revision);
    writer.Write(snapshot.Guid);
    writer.Write(snapshot.Race);
    writer.Write(snapshot.ClassId);
    writer.Write(snapshot.Level);
    writer.Write(snapshot.DisplayPower);
    writer.Write(snapshot.ShapeshiftForm);
    writer.Write(snapshot.Xp);
    writer.Write(snapshot.NextLevelXp);
    writer.Write(snapshot.Resources.PowerType);

    for (int32 value : snapshot.Resources.Powers)
        writer.Write(value);
    for (int32 value : snapshot.Resources.MaxPowers)
        writer.Write(value);
    for (int32 value : snapshot.Resources.PredictedPowers)
        writer.Write(value);
    for (size_t i = 0; i < snapshot.Resources.RegenerationFlat.size(); ++i)
    {
        writer.Write(snapshot.Resources.RegenerationFlat[i]);
        writer.Write(snapshot.Resources.RegenerationInterruptedFlat[i]);
    }
    for (float value : snapshot.Resources.RuneRegeneration)
        writer.Write(value);
    writer.Write(snapshot.Resources.Runes.Count);
    writer.Write(snapshot.Resources.Runes.PredictedMask);
    for (size_t i = 0; i < RUNE_SLOTS; ++i)
    {
        writer.Write(snapshot.Resources.Runes.Types[i]);
        writer.Write(snapshot.Resources.Runes.BaseTypes[i]);
        writer.Write(snapshot.Resources.Runes.Cooldowns[i]);
        writer.Write(snapshot.Resources.Runes.LastUsed[i]);
    }
    writer.Write(snapshot.ComboTargetGuid);
    writer.Write(snapshot.ComboPoints);
    writer.Write(snapshot.Health);
    writer.Write(snapshot.MaxHealth);

    ActiveClassExternalStats const& stats = snapshot.ExternalStats;
    for (size_t i = 0; i < stats.Stats.size(); ++i)
    {
        writer.Write(stats.Stats[i]);
        writer.Write(stats.PositiveStats[i]);
        writer.Write(stats.NegativeStats[i]);
    }
    for (size_t i = 0; i < stats.Resistances.size(); ++i)
    {
        writer.Write(stats.Resistances[i]);
        writer.Write(stats.PositiveResistances[i]);
        writer.Write(stats.NegativeResistances[i]);
    }
    writer.Write(stats.AttackPower);
    writer.Write(stats.AttackPowerModifiers[0]);
    writer.Write(stats.AttackPowerModifiers[1]);
    writer.Write(stats.AttackPowerMultiplier);
    writer.Write(stats.RangedAttackPower);
    writer.Write(stats.RangedAttackPowerModifiers[0]);
    writer.Write(stats.RangedAttackPowerModifiers[1]);
    writer.Write(stats.RangedAttackPowerMultiplier);
    for (size_t i = 0; i < stats.PowerCostModifiers.size(); ++i)
    {
        writer.Write(stats.PowerCostModifiers[i]);
        writer.Write(stats.PowerCostMultipliers[i]);
    }
    writer.Write(stats.BlockPercentage);
    writer.Write(stats.DodgePercentage);
    writer.Write(stats.ParryPercentage);
    writer.Write(stats.Expertise);
    writer.Write(stats.OffhandExpertise);
    writer.Write(stats.CritPercentage);
    writer.Write(stats.RangedCritPercentage);
    writer.Write(stats.OffhandCritPercentage);
    for (float value : stats.SpellCritPercentages)
        writer.Write(value);
    writer.Write(stats.ShieldBlock);
    writer.Write(stats.ShieldBlockCritPercentage);
    for (uint32 value : stats.CombatRatings)
        writer.Write(value);

    writer.Write(static_cast<uint32>(snapshot.Modifiers.size()));
    for (ActiveClassSpellModifier const& modifier : snapshot.Modifiers)
    {
        writer.Write(modifier.Group);
        writer.Write(modifier.Operation);
        writer.Write(modifier.Value);
        writer.Write(static_cast<uint8>(modifier.Percent ? 1 : 0));
    }
    writer.Write(static_cast<uint32>(snapshot.Spells.size()));
    for (ActiveClassSpell const& spell : snapshot.Spells)
    {
        writer.Write(spell.Id);
        writer.Write(spell.Slot);
    }
    for (uint32 action : snapshot.Actions)
        writer.Write(action);
    writer.Write(snapshot.ActiveTalentGroup);
    writer.Write(snapshot.TalentGroupCount);
    for (uint32 points : snapshot.CharacterPoints)
        writer.Write(points);
    for (uint32 group = 0; group < snapshot.TalentGroupCount; ++group)
    {
        ActiveClassTalentGroup const& profile = snapshot.TalentGroups[group];
        writer.Write(profile.UnspentPoints);
        writer.Write(static_cast<uint32>(profile.Talents.size()));
        for (ActiveClassTalent const& talent : profile.Talents)
        {
            writer.Write(talent.Id);
            writer.Write(talent.Rank);
        }
        for (uint32 glyph : profile.Glyphs)
            writer.Write(glyph);
    }

    writer.Write(static_cast<uint32>(snapshot.Cooldowns.size()));
    for (ActiveClassCooldown const& cooldown : snapshot.Cooldowns)
    {
        writer.Write(cooldown.SpellId);
        writer.Write(cooldown.ItemId);
        writer.Write(cooldown.Category);
        writer.Write(cooldown.CooldownLeft);
        writer.Write(cooldown.CategoryCooldownLeft);
        writer.Write(static_cast<uint8>(cooldown.OnHold ? 1 : 0));
        writer.Write(cooldown.Pet);
        writer.Write(static_cast<uint8>(cooldown.Shared ? 1 : 0));
    }
    writer.Write(static_cast<uint32>(snapshot.Skills.size()));
    for (ActiveClassSkill const& skill : snapshot.Skills)
    {
        writer.Write(skill.Id);
        writer.Write(skill.Step);
        writer.Write(skill.Rank);
        writer.Write(skill.MaxRank);
        writer.Write(skill.TemporaryBonus);
        writer.Write(skill.PermanentBonus);
        writer.Write(static_cast<uint8>(skill.Shared ? 1 : 0));
    }
    for (uint32 proficiency : snapshot.Proficiencies)
        writer.Write(proficiency);
    for (ActiveClassEquipmentItem const& item : snapshot.Equipment)
    {
        writer.Write(item.ItemId);
        writer.Write(item.Enchantments[0]);
        writer.Write(item.Enchantments[1]);
        writer.Write(item.EquippedGuid);
    }
    writer.Write(static_cast<uint32>(snapshot.ActiveClassAuras.size()));
    for (ActiveClassAura const& aura : snapshot.ActiveClassAuras)
    {
        writer.Write(aura.CreatorGuid);
        writer.Write(aura.SpellId);
        writer.Write(aura.Flags);
        writer.Write(aura.Level);
        writer.Write(aura.StackCount);
        writer.Write(aura.Duration);
        writer.Write(aura.RemainingMilliseconds);
    }
    writer.Write(snapshot.Pet.Guid);
    writer.Write(snapshot.Pet.Family);
    writer.Write(snapshot.Pet.LinkType);
    writer.Write(snapshot.Pet.ExpirationRemainingMilliseconds);
    writer.Write(snapshot.Pet.Mode);
    for (uint32 action : snapshot.Pet.Actions)
        writer.Write(action);
    writer.Write(static_cast<uint32>(snapshot.Pet.SpellActions.size()));
    for (uint32 action : snapshot.Pet.SpellActions)
        writer.Write(action);
    writer.Write(snapshot.SharedProgression.SharedDataVersion);
    writer.Write(static_cast<uint32>(snapshot.SharedProgression.SpellIds.size()));
    for (uint32 spellId : snapshot.SharedProgression.SpellIds)
        writer.Write(spellId);

    return data;
}
}
