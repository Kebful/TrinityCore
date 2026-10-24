/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _PLAYERCLASSSWITCH_H
#define _PLAYERCLASSSWITCH_H

#include "Define.h"
#include "DatabaseEnvFwd.h"

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

class Player;

namespace ClassSwitch
{
constexpr uint16 REQUEST_PROTOCOL_VERSION = 1;
constexpr uint16 CLIENT_REQUEST_OPCODE = 0x7A30;
constexpr uint16 SERVER_STATUS_OPCODE = 0x7A31;
constexpr uint16 SERVER_SNAPSHOT_OPCODE = 0x7A32;
constexpr uint16 CLIENT_RESPONSE_OPCODE = 0x7A33;
constexpr uint16 RESPONSE_PROTOCOL_VERSION = 1;
constexpr uint16 MAX_RESPONSE_REASON_BYTES = 512;
constexpr uint8 MAX_CLASS_SWITCH_RECOVERY_ATTEMPTS = 1;

constexpr uint32 SNAPSHOT_MAGIC = 0x32435341; // "ASC2"
constexpr uint16 SNAPSHOT_VERSION = 2;
constexpr uint32 SNAPSHOT_SECTION_REQUIRED = 0x3FF;
constexpr uint32 PROFILE_MAGIC = 0x31505343; // "CSP1"
constexpr uint16 PROFILE_VERSION = 2;

constexpr uint32 MAX_SPELLS = 4096;
constexpr uint32 MAX_MODIFIERS = 5952;
constexpr uint32 MAX_TALENTS = 256;
constexpr uint32 MAX_COOLDOWNS = 1024;
constexpr uint32 MAX_SKILLS = 128;
constexpr uint32 ACTIVE_CLASS_MAX_AURAS = 128;
constexpr uint32 MAX_PET_SPELLS = 32;
constexpr uint32 ACTION_SLOTS = 144;
constexpr uint32 EQUIPMENT_SLOTS = 19;
constexpr uint32 TALENT_GROUPS = 2;
constexpr uint32 GLYPH_SLOTS = 6;
constexpr uint32 PET_ACTION_SLOTS = 10;
constexpr uint32 POWER_TYPES = 7;
constexpr uint32 RUNE_SLOTS = 8;
constexpr uint32 PROFICIENCY_CLASSES = 17;

enum class RequestType : uint8
{
    State = 1,
    Select = 2
};

enum class RequestResult : uint8
{
    Ok = 0,
    UnsupportedProtocol,
    Malformed,
    NotReady,
    StaleRevision,
    UnknownClass,
    ClassUnavailable,
    Busy,
    RateLimited,
    TargetUnchanged,
    EquipmentIncompatible,
    MissingDefinition,
    StorageFailure
};

enum class StatusStage : uint8
{
    State = 1,
    Accepted = 2,
    Rejected = 3,
    CommitBegin = 4,
    Confirmed = 5
};

enum class ClientResponseDisposition : uint8
{
    Accepted = 1,
    Rejected = 2,
    ResyncRequested = 3
};

struct ClientClassSwitchResponse
{
    uint16 Version = RESPONSE_PROTOCOL_VERSION;
    ClientResponseDisposition Disposition = ClientResponseDisposition::Rejected;
    uint8 ClassId = 0;
    uint64 Session = 0;
    uint64 Revision = 0;
    uint64 SnapshotDigest = 0;
    std::string Reason;
};

struct EquipmentBagCandidate
{
    uint16 Position = 0;
    bool FreedByTargetEquip = false;
};

struct EquipmentBagDemand
{
    std::vector<uint16> AllowedPositions;
};

struct ClassSwitchLogoutPlan
{
    bool PersistPlayer = true;
    bool RemovePet = true;
    bool ClearRuntime = true;
};

enum class ClientResponseMatch : uint8
{
    Accepted,
    Rejected,
    ResyncRequested,
    DuplicateAccepted,
    StaleOrMismatched
};

enum class ClientTimeoutAction : uint8
{
    None,
    RecoverSource,
    Disconnect
};

enum class ProfileLoadStatus : uint8
{
    Loaded,
    Missing,
    Corrupt,
    DatabaseError
};

enum ClassAvailabilityFlags : uint8
{
    CLASS_FLAG_ACTIVE = 0x01,
    CLASS_FLAG_PENDING = 0x02,
    CLASS_FLAG_AVAILABLE = 0x04
};

struct ClassDefinition
{
    uint8 ClassId = 0;
    uint8 InitialLevel = 0;
    uint32 RaceMask = 0;
    std::string Name;
    bool Enabled = false;
};

struct SpellProfile
{
    uint32 Id = 0;
    uint8 State = 0;
    bool Active = false;
    bool Dependent = false;
    bool Disabled = false;
};

struct TalentProfile
{
    uint32 SpellId = 0;
    uint8 Spec = 0;
};

struct SkillProfile
{
    uint32 Id = 0;
    uint16 Step = 0;
    uint16 Value = 0;
    uint16 Max = 0;
    int16 TemporaryBonus = 0;
    int16 PermanentBonus = 0;
    bool Shared = false;
};

struct TalentGroupProfile
{
    std::vector<TalentProfile> Talents;
    std::array<uint32, GLYPH_SLOTS> Glyphs = {};
    std::array<uint32, ACTION_SLOTS> Actions = {};
};

struct StoredRuneProfile
{
    uint8 Count = 0;
    std::array<int32, RUNE_SLOTS> Types = {};
    std::array<int32, RUNE_SLOTS> BaseTypes = {};
    std::array<uint32, RUNE_SLOTS> Cooldowns = {};
    std::array<uint32, RUNE_SLOTS> LastUsed = {};
};

struct StoredEquipmentItem
{
    uint32 ItemId = 0;
    std::array<uint16, 2> Enchantments = {};
    uint64 EquippedGuid = 0;
};

struct StoredClassProfile
{
    uint16 Version = PROFILE_VERSION;
    uint64 Revision = 0;
    uint8 ClassId = 0;
    uint16 Level = 0;
    uint32 Xp = 0;
    uint32 NextLevelXp = 0;
    uint8 ActiveSpec = 0;
    uint8 SpecCount = 0;
    uint32 FreeTalentPoints = 0;
    float HealthPercent = 100.0f;
    std::array<float, POWER_TYPES> PowerPercent = {};
    StoredRuneProfile Runes;
    std::vector<SpellProfile> Spells;
    std::vector<SkillProfile> Skills;
    std::array<TalentGroupProfile, TALENT_GROUPS> Groups;
    std::array<StoredEquipmentItem, EQUIPMENT_SLOTS> Equipment;
    bool EquipmentMigrated = false;
};

struct CharacterLanguageState
{
    std::vector<SpellProfile> Spells;
    std::vector<SkillProfile> Skills;
};

struct SelectionPolicy
{
    bool Pending = false;
    bool InCombat = false;
    bool Casting = false;
    bool SameClass = false;
    uint64 ActiveRevision = 0;
    uint64 ClientRevision = 0;
};

struct ActiveClassStatusDescriptor
{
    uint8 ClassId = 0;
    uint8 Level = 0;
    uint8 Flags = 0;
    std::string Name;
};

struct ActiveClassSpell
{
    int32 Id = 0;
    int32 Slot = 0;
};

struct ActiveClassSpellModifier
{
    uint8 Group = 0;
    uint8 Operation = 0;
    int32 Value = 0;
    bool Percent = false;
};

struct ActiveClassTalent
{
    int32 Id = 0;
    uint8 Rank = 0;
};

struct ActiveClassTalentGroup
{
    uint32 UnspentPoints = 0;
    std::vector<ActiveClassTalent> Talents;
    std::array<uint32, GLYPH_SLOTS> Glyphs = {};
};

struct ActiveClassCooldown
{
    int32 SpellId = 0;
    uint32 ItemId = 0;
    uint32 Category = 0;
    uint32 CooldownLeft = 0;
    uint32 CategoryCooldownLeft = 0;
    bool OnHold = false;
    uint8 Pet = 0;
    bool Shared = false;
};

struct ActiveClassSkill
{
    int32 Id = 0;
    uint16 Step = 0;
    uint16 Rank = 0;
    uint16 MaxRank = 0;
    uint16 TemporaryBonus = 0;
    uint16 PermanentBonus = 0;
    bool Shared = false;
};

struct ActiveClassRuneState
{
    uint32 Count = 0;
    uint32 PredictedMask = 0;
    std::array<int32, RUNE_SLOTS> Types = {};
    std::array<int32, RUNE_SLOTS> BaseTypes = {};
    std::array<uint32, RUNE_SLOTS> Cooldowns = {};
    std::array<uint32, RUNE_SLOTS> LastUsed = {};
};

struct ActiveClassResourceState
{
    uint8 PowerType = 0;
    std::array<int32, POWER_TYPES> Powers = {};
    std::array<int32, POWER_TYPES> MaxPowers = {};
    std::array<int32, POWER_TYPES> PredictedPowers = {};
    std::array<float, POWER_TYPES> RegenerationFlat = {};
    std::array<float, POWER_TYPES> RegenerationInterruptedFlat = {};
    std::array<float, 4> RuneRegeneration = {};
    ActiveClassRuneState Runes;
};

struct ActiveClassExternalStats
{
    std::array<int32, 5> Stats = {};
    std::array<int32, 5> PositiveStats = {};
    std::array<int32, 5> NegativeStats = {};
    std::array<int32, 7> Resistances = {};
    std::array<int32, 7> PositiveResistances = {};
    std::array<int32, 7> NegativeResistances = {};
    int32 AttackPower = 0;
    std::array<int16, 2> AttackPowerModifiers = {};
    float AttackPowerMultiplier = 0.0f;
    int32 RangedAttackPower = 0;
    std::array<int16, 2> RangedAttackPowerModifiers = {};
    float RangedAttackPowerMultiplier = 0.0f;
    std::array<int32, 7> PowerCostModifiers = {};
    std::array<float, 7> PowerCostMultipliers = {};
    float BlockPercentage = 0.0f;
    float DodgePercentage = 0.0f;
    float ParryPercentage = 0.0f;
    uint32 Expertise = 0;
    uint32 OffhandExpertise = 0;
    float CritPercentage = 0.0f;
    float RangedCritPercentage = 0.0f;
    float OffhandCritPercentage = 0.0f;
    std::array<float, 7> SpellCritPercentages = {};
    uint32 ShieldBlock = 0;
    float ShieldBlockCritPercentage = 0.0f;
    std::array<uint32, 25> CombatRatings = {};
};

struct ActiveClassEquipmentItem
{
    int32 ItemId = 0;
    std::array<uint16, 2> Enchantments = {};
    uint64 EquippedGuid = 0;
};

struct ActiveClassAura
{
    uint64 CreatorGuid = 0;
    int32 SpellId = 0;
    uint8 Flags = 0;
    uint8 Level = 0;
    uint8 StackCount = 0;
    int32 Duration = 0;
    uint32 RemainingMilliseconds = 0;
};

struct ActiveClassPetState
{
    uint64 Guid = 0;
    uint32 Family = 0;
    uint8 LinkType = 0;
    uint32 ExpirationRemainingMilliseconds = 0;
    uint32 Mode = 0;
    std::array<uint32, PET_ACTION_SLOTS> Actions = {};
    std::vector<uint32> SpellActions;
};

struct SharedProgressionState
{
    uint32 SharedDataVersion = 1;
    std::vector<uint32> SpellIds;
};

struct ActiveClassSnapshot
{
    uint16 Version = SNAPSHOT_VERSION;
    uint32 SectionMask = SNAPSHOT_SECTION_REQUIRED;
    uint64 Session = 0;
    uint64 Revision = 0;
    uint64 Guid = 0;
    uint8 Race = 0;
    uint8 ClassId = 0;
    uint8 Level = 0;
    uint8 DisplayPower = 0;
    uint8 ShapeshiftForm = 0;
    uint32 Xp = 0;
    uint32 NextLevelXp = 0;
    ActiveClassResourceState Resources;
    uint64 ComboTargetGuid = 0;
    uint8 ComboPoints = 0;
    int32 Health = 0;
    int32 MaxHealth = 0;
    ActiveClassExternalStats ExternalStats;
    std::vector<ActiveClassSpellModifier> Modifiers;
    std::vector<ActiveClassSpell> Spells;
    std::array<uint32, ACTION_SLOTS> Actions = {};
    uint32 ActiveTalentGroup = 0;
    uint32 TalentGroupCount = 0;
    std::array<uint32, 2> CharacterPoints = {};
    std::array<ActiveClassTalentGroup, TALENT_GROUPS> TalentGroups;
    std::vector<ActiveClassCooldown> Cooldowns;
    std::vector<ActiveClassSkill> Skills;
    std::array<uint32, PROFICIENCY_CLASSES> Proficiencies = {};
    std::array<ActiveClassEquipmentItem, EQUIPMENT_SLOTS> Equipment;
    std::vector<ActiveClassAura> ActiveClassAuras;
    ActiveClassPetState Pet;
    SharedProgressionState SharedProgression;
};

uint32 ClassIdToMask(uint8 classId);
bool IsSupportedClassId(uint8 classId);
uint64 ComputeSessionTag(uint8 const* bytes, size_t size);
uint64 ComputeFnv1a64(uint8 const* bytes, size_t size);
uint8 ResolveClassStartLevel(uint8 classId, uint8 configuredClassInitialLevel,
    uint8 configuredPlayerStartLevel, uint8 configuredDeathKnightStartLevel);
uint16 ResolveClassStatusLevel(uint8 classId, bool hasPersistedProfile, uint16 persistedLevel,
    uint8 configuredClassInitialLevel, uint8 configuredPlayerStartLevel,
    uint8 configuredDeathKnightStartLevel);
bool PlanEquipmentBagReservations(std::vector<EquipmentBagCandidate> const& candidates,
    std::vector<EquipmentBagDemand> const& demands, std::vector<uint16>* destinations);
std::vector<uint8> BuildChangedEquipmentSlots(
    std::array<uint64, EQUIPMENT_SLOTS> const& current,
    std::array<uint64, EQUIPMENT_SLOTS> const& target);
ClassSwitchLogoutPlan PlanClassSwitchLogout(bool hadPendingTransition,
    bool sourceRestoreSucceeded);
bool IsClassSwitchGameplayBlocked(bool pendingTransition, bool controlPacket);
bool DecodeClientClassSwitchResponse(uint8 const* data, size_t size,
    ClientClassSwitchResponse* response, std::string* error);
ClientResponseMatch MatchClientClassSwitchResponse(ClientClassSwitchResponse const& response,
    uint64 expectedSession, uint8 expectedClass, uint64 expectedRevision, uint64 expectedDigest,
    uint8 sourceClass, uint64 sourceRevision, uint8 completedClass, uint64 completedRevision,
    uint64 completedDigest);
ClientTimeoutAction EvaluateClassSwitchTimeout(bool pending, bool recoveringSource,
    bool deadlineReached, uint8 recoveryAttempts);
bool ReserveTransitionRevision(uint64 baseRevision, uint64 targetRevision, uint64* reserved);
bool IsSwitchOwnedAura(uint64 playerGuid, uint64 creatorGuid, uint32 classFamily,
    uint32 spellFamily, bool shapeshift);
RequestResult EvaluateSelectionPolicy(SelectionPolicy const& policy);
bool IsSpellAllowedForClassProfile(uint32 spellFamily, uint32 classFamily, bool professionOrRiding);
uint32 NormalizeActionSpellRank(uint32 requestedRank, std::vector<uint32> const& spellChain,
    std::set<uint32> const& activeRanks);
void StripClassOwnedLanguages(StoredClassProfile* profile,
    std::set<uint32> const& languageSpellIds, std::set<uint32> const& languageSkillIds,
    uint32* removedSpells = nullptr, uint32* removedSkills = nullptr);
void MergeCharacterLanguages(StoredClassProfile* profile,
    CharacterLanguageState const& languages,
    std::set<uint32> const& languageSpellIds, std::set<uint32> const& languageSkillIds);
bool ValidateStoredProfile(StoredClassProfile const& profile, std::string* error,
    bool validateActionSpells = true);
std::vector<uint8> EncodeStoredProfile(StoredClassProfile const& profile);
bool DecodeStoredProfile(uint8 const* data, size_t size, StoredClassProfile* profile, std::string* error);
std::vector<std::string> BuildProfileBackingTableStatements(uint64 guid,
    StoredClassProfile const& profile, std::vector<uint32> const& languageSpellIds = {},
    std::vector<uint32> const& languageSkillIds = {});
bool AppendStoredProfileToTransaction(CharacterDatabaseTransaction const& transaction, Player* player,
    StoredClassProfile const& profile, std::string* error);
bool AppendActiveProfileToTransaction(CharacterDatabaseTransaction const& transaction, Player* player,
    std::string* error);
std::vector<uint8> EncodeActiveClassStatus(StatusStage stage, RequestResult result,
    uint8 activeClassId, uint8 pendingClassId, uint64 revision, uint64 session,
    std::vector<ActiveClassStatusDescriptor> const& classes);
bool ValidateActiveClassSnapshot(ActiveClassSnapshot const& snapshot, std::string* error);
std::vector<uint8> EncodeActiveClassSnapshot(ActiveClassSnapshot const& snapshot);

RequestResult RequestSwitch(Player* player, uint16 protocolVersion, uint8 requestType, uint8 classId,
    uint64 revision, std::string* error);
bool ProcessClientResponse(Player* player, uint8 const* payload, size_t size, std::string* error);
bool HandleSwitchSpellCast(Player* player, uint32 spellId, std::string* error);
void UpdatePlayer(Player* player);
bool IsTransitionPending(Player* player);
bool CancelPendingTransitionOnLogout(Player* player);
void RestoreActiveProfileAfterLoad(Player* player);
void ClearPlayerRuntime(Player* player);
bool ValidateClassSwitchRegistry(std::string* error);
uint32 GetSwitchSpellId(Player* player);
}

#endif
