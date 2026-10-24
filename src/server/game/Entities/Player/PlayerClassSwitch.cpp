/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "PlayerClassSwitch.h"

#include "ArenaTeam.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "CustomPacketDefines.h"
#include "CustomPacketWrite.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "UpdateFields.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace ClassSwitch
{
namespace
{
constexpr std::chrono::milliseconds MIN_REQUEST_INTERVAL(500);
constexpr size_t MAX_REQUESTS_PER_MINUTE = 6;
constexpr std::chrono::seconds CLIENT_RESPONSE_TIMEOUT(10);

struct RuntimeClassDefinition
{
    ClassDefinition Definition;
    uint32 SpellClassSet = 0;
    uint8 DisplayPower = 0;
};

enum class TransitionPhase : uint8
{
    None,
    AwaitingSpellCast,
    AwaitingInitialAcceptance,
    RecoveringSource,
    RecoveryFailed
};

struct EquipmentRollbackPlan
{
    std::array<uint16, EQUIPMENT_SLOTS> TargetBagDestinations = {};
    bool Prepared = false;
};

struct LanguageEntryIds
{
    std::set<uint32> Spells;
    std::set<uint32> Skills;
};

struct PendingSwitch
{
    TransitionPhase Phase = TransitionPhase::None;
    uint8 SourceClass = 0;
    uint8 TargetClass = 0;
    uint8 ExpectedClass = 0;
    uint64 BaseRevision = 0;
    uint64 TargetRevision = 0;
    uint64 RecoveryRevision = 0;
    uint64 SnapshotDigest = 0;
    uint8 RecoveryAttempts = 0;
    StoredClassProfile SourceProfile;
    StoredClassProfile TargetProfile;
    PetStable SourcePetStable;
    bool SourcePetCaptured = false;
    EquipmentRollbackPlan EquipmentRollback;
    std::vector<uint8> SnapshotPayload;
    SystemTimePoint Deadline = SystemTimePoint::clock::now();
};

struct PlayerRuntime
{
    uint64 Session = 0;
    uint64 ActiveRevision = 0;
    bool PersistSwitchTarget = false;
    StoredClassProfile PersistTargetProfile;
    PendingSwitch Pending;
    uint64 LastCompletedRevision = 0;
    uint64 LastCompletedDigest = 0;
    uint8 LastCompletedClass = 0;
    uint8 LastRequestedClass = 0;
    std::deque<SystemTimePoint> RequestTimes;
    std::vector<ActiveClassCooldown> SharedCooldowns;
};

void SendCustomPacket(Player* player, uint16 opcode, std::vector<uint8> const& payload)
{
    if (!player || payload.size() > std::numeric_limits<totalSize_t>::max())
        return;

    CustomPacketWrite writer(opcode, MAX_FRAGMENT_SIZE, 0);
    if (!payload.empty())
        writer.WriteBytes(static_cast<totalSize_t>(payload.size()), reinterpret_cast<char const*>(payload.data()));

    for (CustomPacketChunk& chunk : writer.buildMessages())
    {
        WorldPacket packet(SERVER_TO_CLIENT_OPCODE, chunk.FullSize());
        packet.append(reinterpret_cast<uint8 const*>(chunk.Data()), chunk.FullSize());
        player->SendDirectMessage(&packet);
    }
    writer.Destroy();
}

float ClampPercent(float value)
{
    if (!std::isfinite(value))
        return 0.0f;
    return std::clamp(value, 0.0f, 100.0f);
}

uint32 TalentPointsForClassLevel(uint8 classId, uint8 level)
{
    uint32 points = level < 10 ? 0 : uint32(level) - 9;
    if (classId == CLASS_DEATH_KNIGHT && level < 56)
        return 0;
    return points;
}

uint32 SkillInfoIndex(uint8 position)
{
    return PLAYER_SKILL_INFO_1_1 + uint32(position) * 3;
}

uint32 SkillValueIndex(uint8 position)
{
    return SkillInfoIndex(position) + 1;
}

uint32 SkillBonusIndex(uint8 position)
{
    return SkillInfoIndex(position) + 2;
}

bool IsLanguageSkillId(uint32 skillId)
{
    SkillLineEntry const* skill = sSkillLineStore.LookupEntry(skillId);
    return skill && skill->CategoryID == SKILL_CATEGORY_LANGUAGES;
}

bool IsLanguageSpellId(uint32 spellId)
{
    SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    for (auto itr = bounds.first; itr != bounds.second; ++itr)
        if (IsLanguageSkillId(itr->second->SkillLine))
            return true;
    return false;
}
}

class ClassSwitchManager
{
public:
    static ClassSwitchManager& Instance()
    {
        static ClassSwitchManager instance;
        return instance;
    }

    uint32 GetSwitchSpellId(std::string* error = nullptr)
    {
        if (!EnsureDefinitions(error))
            return 0;
        return _switchSpellId;
    }

    bool ValidateRegistry(std::string* error)
    {
        if (!EnsureDefinitions(error))
            return false;
        if (_definitions.empty())
            return SetError(error, "no complete class-switch definitions are installed");
        TC_LOG_INFO("server.loading",
            "Loaded class-switch registry with {} available class definitions and switch spell {}",
            _definitions.size(), _switchSpellId);
        return true;
    }

    RequestResult Request(Player* player, uint16 protocolVersion, uint8 requestType, uint8 classId,
        uint64 revision, std::string* error)
    {
        if (!player || !player->GetSession())
            return RequestResult::NotReady;
        PlayerRuntime& runtime = _players[player->GetGUID().GetCounter()];
        uint64 const session = player->GetSession()->GetActiveClassSessionTag();
        if (runtime.Session != session)
            runtime = {};
        runtime.Session = session;
        runtime.LastRequestedClass = classId;
        if (protocolVersion != REQUEST_PROTOCOL_VERSION)
            return Reject(player, runtime, RequestResult::UnsupportedProtocol,
                "unsupported class-switch protocol version", error);
        if (!EnsureDefinitions(error))
        {
            if (error && error->empty())
                *error = "class-switch definitions are unavailable";
            return Reject(player, runtime, RequestResult::StorageFailure,
                error ? *error : "class-switch definitions are unavailable", error);
        }
        if (requestType == static_cast<uint8>(RequestType::State))
        {
            if (classId != 0)
                return Reject(player, runtime, RequestResult::Malformed,
                    "state requests must not include a class ID", error);
            SendState(player, StatusStage::State, RequestResult::Ok, runtime);
            return RequestResult::Ok;
        }

        SystemTimePoint const now = SystemTimePoint::clock::now();
        while (!runtime.RequestTimes.empty() &&
            now - runtime.RequestTimes.front() >= std::chrono::minutes(1))
            runtime.RequestTimes.pop_front();
        if (!runtime.RequestTimes.empty() && now - runtime.RequestTimes.back() < MIN_REQUEST_INTERVAL)
            return Reject(player, runtime, RequestResult::RateLimited,
                "class-switch selections are too frequent", error);
        if (runtime.RequestTimes.size() >= MAX_REQUESTS_PER_MINUTE)
            return Reject(player, runtime, RequestResult::RateLimited,
                "class-switch selection limit reached", error);
        runtime.RequestTimes.push_back(now);

        if (requestType != static_cast<uint8>(RequestType::Select))
            return Reject(player, runtime, RequestResult::Malformed,
                "class-switch request type is invalid", error);
        if (!classId)
            return Reject(player, runtime, RequestResult::Malformed,
                "selection requests require a class ID", error);
        if (!IsSupportedClassId(classId))
            return Reject(player, runtime, RequestResult::UnknownClass,
                "class ID is outside the uint32 class-mask representable range", error);

        char const* readinessError = nullptr;
        if (!IsReady(player, false, &readinessError))
            return Reject(player, runtime, RequestResult::NotReady, readinessError, error);
        if (runtime.Pending.TargetClass)
            return Reject(player, runtime, RequestResult::Busy, "a class switch is already pending", error);

        RuntimeClassDefinition const* definition = FindDefinition(classId);
        if (!definition)
            return Reject(player, runtime, RequestResult::MissingDefinition,
                "class definition is missing or incomplete", error);
        if (!definition->Definition.Enabled)
            return Reject(player, runtime, RequestResult::ClassUnavailable, "configured class is disabled", error);
        if (!(ClassIdToMask(player->GetRace()) & definition->Definition.RaceMask))
            return Reject(player, runtime, RequestResult::ClassUnavailable, "class is unavailable for this race", error);
        if (!ValidateRaceClassAtLevel(player->GetRace(), definition->Definition, definition->Definition.InitialLevel, error))
            return Reject(player, runtime, RequestResult::ClassUnavailable, error ? *error : "class definition is incomplete", error);
        if (classId == player->GetClass())
            return Reject(player, runtime, RequestResult::TargetUnchanged, "selected class is already active", error);

        StoredClassProfile active;
        if (!LoadOrCreateProfile(player, player->GetClass(), &active, error))
            return Reject(player, runtime, RequestResult::StorageFailure, error ? *error : "active profile is unavailable", error);
        if (revision != active.Revision)
            return Reject(player, runtime, RequestResult::StaleRevision, "class-switch revision is stale", error);

        StoredClassProfile reservedTarget;
        if (!LoadOrCreateProfile(player, classId, &reservedTarget, active, error))
            return Reject(player, runtime, RequestResult::StorageFailure,
                error ? *error : "target profile is unavailable", error);
        MergeSharedState(active, reservedTarget);
        if (!ValidateSnapshotActionPreflight(active, reservedTarget, error))
            return Reject(player, runtime, RequestResult::MissingDefinition,
                error ? *error : "target actions cannot be represented safely", error);
        if (!ValidateTargetProfile(player, *definition, reservedTarget, error))
            return Reject(player, runtime, RequestResult::MissingDefinition,
                error ? *error : "target profile is unavailable", error);
        if (!ValidateEquipment(player, reservedTarget, error))
            return Reject(player, runtime, RequestResult::EquipmentIncompatible,
                error ? *error : "equipped items are incompatible with the target profile", error);

        uint64 reservedRevision = 0;
        if (!ReserveTransitionRevision(active.Revision, reservedTarget.Revision,
            &reservedRevision))
            return Reject(player, runtime, RequestResult::StorageFailure,
                "class-switch revision space is exhausted", error);
        runtime.ActiveRevision = active.Revision;
        runtime.Pending.Phase = TransitionPhase::AwaitingSpellCast;
        runtime.Pending.SourceClass = active.ClassId;
        runtime.Pending.TargetClass = classId;
        runtime.Pending.BaseRevision = revision;
        runtime.Pending.TargetRevision = reservedRevision;
        runtime.Pending.Deadline = SystemTimePoint::clock::now() + CLIENT_RESPONSE_TIMEOUT;
        SendState(player, StatusStage::Accepted, RequestResult::Ok, runtime,
            false, runtime.Pending.TargetRevision);
        return RequestResult::Ok;
    }

    bool HandleSpell(Player* player, uint32 spellId, std::string* error)
    {
        if (!player || !player->GetSession() || !player->GetSession()->GetActiveClassSessionTag() ||
            spellId != GetSwitchSpellId(error))
            return false;

        PlayerRuntime& runtime = _players[player->GetGUID().GetCounter()];
        uint64 const session = player->GetSession()->GetActiveClassSessionTag();
        if (runtime.Session != session)
            runtime = {};
        runtime.Session = session;
        if (!runtime.Pending.TargetClass)
        {
            SetError(error, "class switch has no selected target");
            return false;
        }
        char const* readinessError = nullptr;
        if (!IsReady(player, true, &readinessError))
            return RejectSpell(player, runtime, RequestResult::NotReady,
                readinessError ? readinessError : "player is not ready", error);

        RuntimeClassDefinition const* definition = FindDefinition(runtime.Pending.TargetClass);
        if (!definition || !definition->Definition.Enabled)
            return RejectSpell(player, runtime, RequestResult::MissingDefinition,
                "selected class definition is no longer available", error);

        StoredClassProfile source;
        if (!CaptureProfile(player, &source, error, runtime.Pending.BaseRevision))
        {
            if (error && error->empty())
                *error = "failed to capture the active class profile";
            return RejectSpell(player, runtime, RequestResult::StorageFailure,
                error ? *error : "failed to capture the active class profile", error);
        }
        if (runtime.Pending.BaseRevision != source.Revision)
            return RejectSpell(player, runtime, RequestResult::StaleRevision,
                "class switch became stale before cast completion", error);

        StoredClassProfile target;
        if (!LoadOrCreateProfile(player, runtime.Pending.TargetClass, &target, source, error))
        {
            if (error && error->empty())
                *error = "failed to load the target class profile";
            return RejectSpell(player, runtime, RequestResult::StorageFailure,
                error ? *error : "failed to load the target class profile", error);
        }
        MergeSharedState(source, target);
        if (!ValidateSnapshotActionPreflight(source, target, error))
            return RejectSpell(player, runtime, RequestResult::MissingDefinition,
                error ? *error : "target actions cannot be represented safely", error);
        if (!ValidateTargetProfile(player, *definition, target, error))
        {
            if (error && error->empty())
                *error = "target class profile is incomplete";
            return RejectSpell(player, runtime, RequestResult::MissingDefinition,
                error ? *error : "target class profile is incomplete", error);
        }
        if (!ValidateEquipment(player, target, error))
        {
            if (error && error->empty())
                *error = "equipped items are incompatible with the target class";
            return RejectSpell(player, runtime, RequestResult::EquipmentIncompatible,
                error ? *error : "equipped items are incompatible with the target class", error);
        }

        uint64 const targetRevision = runtime.Pending.TargetRevision;
        if (!targetRevision || targetRevision <= std::max(source.Revision, target.Revision))
            return RejectSpell(player, runtime, RequestResult::StaleRevision,
                "reserved class-switch revision is no longer newer than the stored profiles", error);

        target.Revision = targetRevision;
        target.NextLevelXp = sObjectMgr->GetXPForLevel(target.Level);

        CaptureSourcePet(player, runtime.Pending);
        runtime.Pending.SourceProfile = source;
        runtime.Pending.TargetProfile = target;
        runtime.Pending.ExpectedClass = target.ClassId;
        player->m_classSwitchTransitionPending.store(true, std::memory_order_release);
        SendState(player, StatusStage::CommitBegin, RequestResult::Ok, runtime,
            false, targetRevision, source.ClassId);

        auto recoverPreSnapshotFailure = [&](std::string const& message) {
            if (error)
                *error = message;
            TC_LOG_ERROR("entities.player.classswitch",
                "Player {} class-switch failed after CommitBegin; entering authoritative source recovery: {}",
                player->GetGUID().ToString(), message);
            BeginSourceRecovery(player, runtime, message);
            return false;
        };
        if (!ApplyProfile(player, target, error, true,
            &runtime.Pending.EquipmentRollback))
        {
            return recoverPreSnapshotFailure(
                error && !error->empty()
                    ? *error : "failed to apply the target class profile");
        }

        runtime.ActiveRevision = targetRevision;
        runtime.PersistSwitchTarget = true;
        runtime.PersistTargetProfile = target;

        StoredClassProfile appliedTarget;
        if (!CaptureProfile(player, &appliedTarget, error, targetRevision, false, &target))
        {
            return recoverPreSnapshotFailure(
                error && !error->empty()
                    ? *error : "failed to capture the applied class profile");
        }
        if (!ValidateTargetProfile(player, *definition, appliedTarget, error))
        {
            return recoverPreSnapshotFailure(
                error && !error->empty()
                    ? *error : "the applied class profile is not valid for the target class");
        }
        appliedTarget.Revision = targetRevision;
        runtime.PersistTargetProfile = appliedTarget;

        ActiveClassSnapshot snapshot;
        std::vector<uint8> encodedSnapshot;
        if (!BuildSnapshot(player, appliedTarget, runtime, &snapshot, error) ||
            (encodedSnapshot = EncodeActiveClassSnapshot(snapshot)).empty())
        {
            return recoverPreSnapshotFailure(
                error && !error->empty()
                    ? *error : "the target class snapshot is not representable");
        }

        runtime.PersistSwitchTarget = false;
        runtime.Pending.Phase = TransitionPhase::AwaitingInitialAcceptance;
        runtime.Pending.SourceClass = source.ClassId;
        runtime.Pending.ExpectedClass = appliedTarget.ClassId;
        runtime.Pending.TargetRevision = targetRevision;
        runtime.Pending.SourceProfile = source;
        runtime.Pending.TargetProfile = appliedTarget;
        runtime.Pending.SnapshotPayload = encodedSnapshot;
        runtime.Pending.SnapshotDigest =
            ComputeFnv1a64(encodedSnapshot.data(), encodedSnapshot.size());
        runtime.Pending.Deadline = SystemTimePoint::clock::now() + CLIENT_RESPONSE_TIMEOUT;

        player->SendProficiency(ITEM_CLASS_WEAPON, player->m_WeaponProficiency);
        player->SendProficiency(ITEM_CLASS_ARMOR, player->m_ArmorProficiency);
        player->SendInitialSpells();
        player->SendUnlearnSpells();
        player->SendInitialActionButtons();
        player->UpdateObjectVisibility();
        player->SendUpdateToPlayer(player);
        SendCustomPacket(player, SERVER_SNAPSHOT_OPCODE, encodedSnapshot);
        return true;
    }

    bool HandleClientResponse(Player* player, uint8 const* payload, size_t size,
        std::string* error)
    {
        ClientClassSwitchResponse response;
        std::string decodeError;
        if (!DecodeClientClassSwitchResponse(payload, size, &response, &decodeError))
        {
            if (error)
                *error = decodeError;
            if (player && player->GetSession())
            {
                PlayerRuntime& runtime = _players[player->GetGUID().GetCounter()];
                if (runtime.Pending.Phase == TransitionPhase::AwaitingInitialAcceptance ||
                    runtime.Pending.Phase == TransitionPhase::RecoveringSource)
                    BeginSourceRecovery(player, runtime,
                        "malformed client class-switch response: " + decodeError);
            }
            return false;
        }

        if (!player || !player->GetSession())
            return SetError(error, "client response has no active player session");
        uint64 const session = player->GetSession()->GetActiveClassSessionTag();
        if (!session || response.Session != session)
        {
            TC_LOG_WARN("entities.player.classswitch",
                "Ignoring class-switch client response with a stale session for player {}",
                player->GetGUID().ToString());
            return SetError(error, "client response session is stale");
        }

        PlayerRuntime& runtime = _players[player->GetGUID().GetCounter()];
        if (response.Disposition == ClientResponseDisposition::Accepted &&
            response.Revision == runtime.LastCompletedRevision &&
            response.ClassId == runtime.LastCompletedClass &&
            response.SnapshotDigest == runtime.LastCompletedDigest)
        {
            SendState(player, StatusStage::Confirmed, RequestResult::Ok, runtime,
                false, response.Revision, response.ClassId);
            return true;
        }

        PendingSwitch& pending = runtime.Pending;
        if (pending.Phase != TransitionPhase::AwaitingInitialAcceptance &&
            pending.Phase != TransitionPhase::RecoveringSource)
            return SetError(error, "no class-switch snapshot is awaiting a client response");

        ClientResponseMatch const match = MatchClientClassSwitchResponse(
            response, session, pending.ExpectedClass, pending.TargetRevision,
            pending.SnapshotDigest, pending.SourceClass,
            pending.SourceProfile.Revision, runtime.LastCompletedClass,
            runtime.LastCompletedRevision, runtime.LastCompletedDigest);
        if (match == ClientResponseMatch::StaleOrMismatched)
        {
            TC_LOG_WARN("entities.player.classswitch",
                "Ignoring stale class-switch response for player {}: class={} revision={} digest={} expected_class={} expected_revision={} expected_digest={}",
                player->GetGUID().ToString(), uint32(response.ClassId), response.Revision,
                response.SnapshotDigest, uint32(pending.ExpectedClass), pending.TargetRevision,
                pending.SnapshotDigest);
            return SetError(error, "client response does not match the pending class-switch snapshot");
        }

        if (match == ClientResponseMatch::DuplicateAccepted)
        {
            SendState(player, StatusStage::Confirmed, RequestResult::Ok, runtime,
                false, response.Revision, response.ClassId);
            return true;
        }
        if (match == ClientResponseMatch::ResyncRequested)
        {
            BeginSourceRecovery(player, runtime,
                response.Reason.empty() ? "client requested class state resync" : response.Reason);
            return true;
        }
        if (match == ClientResponseMatch::Accepted)
        {
            if (pending.Phase == TransitionPhase::AwaitingInitialAcceptance)
                return CommitAcceptedTarget(player, runtime, error);
            return CompleteSourceRecovery(player, runtime, error);
        }

        std::string const rejection = response.Reason.empty()
            ? "client rejected the class-switch snapshot without a reason"
            : "client rejected the class-switch snapshot: " + response.Reason;
        if (error)
            *error = rejection;
        BeginSourceRecovery(player, runtime, rejection);
        return false;
    }

    void Update(Player* player)
    {
        if (!player || !player->GetSession())
            return;
        auto runtimeItr = _players.find(player->GetGUID().GetCounter());
        if (runtimeItr == _players.end())
            return;
        PlayerRuntime& runtime = runtimeItr->second;
        PendingSwitch& pending = runtime.Pending;
        if (pending.Phase == TransitionPhase::AwaitingSpellCast &&
            SystemTimePoint::clock::now() >= pending.Deadline)
        {
            runtime.Pending = {};
            SendState(player, StatusStage::Rejected, RequestResult::NotReady, runtime);
            TC_LOG_WARN("entities.player.classswitch",
                "Player {} class-switch cast was not started before the pending deadline",
                player->GetGUID().ToString());
            return;
        }
        if ((pending.Phase != TransitionPhase::AwaitingInitialAcceptance &&
             pending.Phase != TransitionPhase::RecoveringSource) ||
            SystemTimePoint::clock::now() < pending.Deadline)
            return;

        ClientTimeoutAction const action = EvaluateClassSwitchTimeout(
            true, pending.Phase == TransitionPhase::RecoveringSource, true,
            pending.RecoveryAttempts);
        if (action == ClientTimeoutAction::RecoverSource)
            BeginSourceRecovery(player, runtime,
                "client did not acknowledge the class-switch snapshot within 10 seconds");
        else if (action == ClientTimeoutAction::Disconnect)
            player->GetSession()->KickPlayer(
                "Class-switch source recovery was not acknowledged within 10 seconds; durable source state was preserved");
    }

    bool IsPending(Player* player)
    {
        return player && player->m_classSwitchTransitionPending.load(std::memory_order_acquire);
    }

    bool CancelOnLogout(Player* player)
    {
        if (!player)
            return false;
        auto itr = _players.find(player->GetGUID().GetCounter());
        if (itr == _players.end())
        {
            player->m_classSwitchTransitionPending.store(false, std::memory_order_release);
            return true;
        }
        PlayerRuntime& runtime = itr->second;
        if (runtime.Pending.Phase == TransitionPhase::AwaitingInitialAcceptance ||
            runtime.Pending.Phase == TransitionPhase::RecoveringSource)
        {
            std::string restoreError;
            if (!ApplyProfile(player, runtime.Pending.SourceProfile, &restoreError,
                false, nullptr, runtime.Pending.EquipmentRollback.Prepared
                    ? &runtime.Pending.EquipmentRollback : nullptr))
            {
                TC_LOG_ERROR("entities.player.classswitch",
                    "Player {} could not restore source state during logout: {}",
                    player->GetGUID().ToString(), restoreError);
                runtime.Pending.Phase = TransitionPhase::RecoveryFailed;
                player->m_classSwitchTransitionPending.store(true, std::memory_order_release);
                return false;
            }
            else
            {
                RestoreSourcePet(player, runtime.Pending);
                NormalizeInMemoryAfterProfileWrite(player);
            }
        }
        player->m_classSwitchTransitionPending.store(false, std::memory_order_release);
        _players.erase(itr);
        return true;
    }

    bool ValidateSnapshotActionPreflight(StoredClassProfile const& source,
        StoredClassProfile& target, std::string* error)
    {
        (void)error;
        std::set<uint32> provenClientActions;
        for (uint32 action : source.Groups[source.ActiveSpec].Actions)
            if (action)
                provenClientActions.insert(action);

        for (uint32& action : target.Groups[target.ActiveSpec].Actions)
        {
            if (!action)
                continue;
            uint32 const actionId = action & 0x00FFFFFFu;
            uint8 const type = uint8(action >> 24);
            switch (type)
            {
                case ACTION_BUTTON_SPELL:
                    break;
                case ACTION_BUTTON_C:
                    action = 0;
                    break;
                case ACTION_BUTTON_EQSET:
                case ACTION_BUTTON_MACRO:
                case ACTION_BUTTON_CMACRO:
                    if (provenClientActions.find(action) == provenClientActions.end())
                        action = 0;
                    break;
                case ACTION_BUTTON_ITEM:
                    if (!sObjectMgr->GetItemTemplate(actionId))
                        action = 0;
                    break;
                default:
                    action = 0;
                    break;
            }
        }
        return true;
    }

    void MergeSharedState(StoredClassProfile const& source, StoredClassProfile& target)
    {
        ChrClassesEntry const* targetClass = sChrClassesStore.LookupEntry(target.ClassId);
        uint32 const targetFamily = targetClass ? targetClass->SpellClassSet : 0;
        std::set<uint32> targetSpells;
        for (SpellProfile const& spell : target.Spells)
            targetSpells.insert(spell.Id);
        for (SpellProfile const& spell : source.Spells)
        {
            if (IsLanguageSpellId(spell.Id))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.Id);
            if (!info || !IsSpellAllowedForClassProfile(
                info->SpellFamilyName, targetFamily, info->IsProfessionOrRiding()))
                continue;
            if (targetSpells.insert(spell.Id).second)
            {
                SpellProfile shared = spell;
                shared.State = PLAYERSPELL_NEW;
                target.Spells.push_back(shared);
            }
            else
            {
                auto targetSpell = std::find_if(target.Spells.begin(), target.Spells.end(),
                    [&spell](SpellProfile const& value) { return value.Id == spell.Id; });
                if (targetSpell != target.Spells.end())
                {
                    targetSpell->Active = spell.Active;
                    targetSpell->Dependent = spell.Dependent;
                    targetSpell->Disabled = spell.Disabled;
                    targetSpell->State = PLAYERSPELL_NEW;
                }
            }
        }

        for (SkillProfile const& sourceSkill : source.Skills)
        {
            if (IsLanguageSkillId(sourceSkill.Id))
                continue;
            if (!sourceSkill.Shared)
                continue;
            auto targetSkill = std::find_if(target.Skills.begin(), target.Skills.end(),
                [&sourceSkill](SkillProfile const& value) { return value.Id == sourceSkill.Id; });
            if (targetSkill == target.Skills.end())
                target.Skills.push_back(sourceSkill);
            else
                *targetSkill = sourceSkill;
        }

        TalentGroupProfile& targetActions = target.Groups[target.ActiveSpec];
        TalentGroupProfile const& sourceActions = source.Groups[source.ActiveSpec];
        for (uint8 button = 0; button < ACTION_SLOTS; ++button)
        {
            uint32 const action = sourceActions.Actions[button];
            uint8 const type = uint8(action >> 24);
            if (!action || type == ACTION_BUTTON_SPELL || type == ACTION_BUTTON_C)
                continue;
            if (!targetActions.Actions[button])
                targetActions.Actions[button] = action;
        }
    }

    void RestoreAfterLoad(Player* player)
    {
        if (!player)
            return;

        StoredClassProfile profile;
        std::string error;
        if (!EnsureDefinitions(&error))
        {
            TC_LOG_ERROR("entities.player.classswitch",
                "Player {} cannot restore class state because definitions are unavailable: {}",
                player->GetGUID().ToString(), error);
            player->GetSession()->KickPlayer("Class-switch definitions are unavailable");
            return;
        }
        ProfileLoadStatus loadStatus = LoadProfile(player, player->GetClass(), &profile, &error);
        if (loadStatus != ProfileLoadStatus::Loaded)
        {
            if (loadStatus == ProfileLoadStatus::Corrupt ||
                loadStatus == ProfileLoadStatus::DatabaseError)
            {
                TC_LOG_ERROR("entities.player.classswitch",
                    "Player {} active class profile is unreadable: {}", player->GetGUID().ToString(), error);
                player->GetSession()->KickPlayer("Unreadable active class profile");
            }
            return;
        }
        if (!profile.Revision || profile.ClassId != player->GetClass())
            return;
        RuntimeClassDefinition const* definition = FindDefinition(player->GetClass());
        if (!definition || !ValidateTargetProfile(player, *definition, profile, &error))
        {
            TC_LOG_ERROR("entities.player.classswitch", "Player {} active class profile is invalid: {}",
                player->GetGUID().ToString(), error);
            player->GetSession()->KickPlayer("Invalid active class profile");
            return;
        }

        if (!ApplyProfile(player, profile, &error))
        {
            TC_LOG_ERROR("entities.player.classswitch", "Player {} failed to restore active class profile: {}",
                player->GetGUID().ToString(), error);
            player->GetSession()->KickPlayer("Active class profile restore failed");
        }
        else
            _players[player->GetGUID().GetCounter()].ActiveRevision = profile.Revision;
    }

    bool EnsureKnown(Player* player)
    {
        if (!player)
            return false;
        uint32 spellId = GetSwitchSpellId();
        if (!spellId || !sSpellMgr->GetSpellInfo(spellId))
            return false;
        if (!player->HasSpell(spellId))
            player->AddSpell(spellId, true, false, false, false, !player->IsInWorld());
        return player->HasSpell(spellId);
    }

    bool AppendActiveProfile(CharacterDatabaseTransaction const& transaction, Player* player,
        std::string* error)
    {
        if (!player || !player->GetSession())
            return SetError(error, "active player is unavailable for class profile save");

        PlayerRuntime& runtime = _players[player->GetGUID().GetCounter()];
        uint64 session = player->GetSession()->GetActiveClassSessionTag();
        if (runtime.Session != session)
            runtime = {};
        runtime.Session = session;

        StoredClassProfile stored;
        ProfileLoadStatus loadStatus = LoadProfile(player, player->GetClass(), &stored, error);
        uint64 revision = 0;
        if (loadStatus == ProfileLoadStatus::Loaded)
            revision = stored.Revision;
        else if (loadStatus == ProfileLoadStatus::Missing)
            revision = runtime.ActiveRevision ? runtime.ActiveRevision : 1;
        else
        {
            TC_LOG_ERROR("entities.player.classswitch",
                "Player {} active class profile cannot be loaded during save: {}",
                player->GetGUID().ToString(), error ? *error : "storage failure");
            player->GetSession()->KickPlayer("Active class profile storage is unreadable");
            return false;
        }

        if (runtime.ActiveRevision)
            revision = runtime.ActiveRevision;

        StoredClassProfile current;
        bool const useSwitchTarget = runtime.PersistSwitchTarget;
        StoredClassProfile const* seed = useSwitchTarget
            ? &runtime.PersistTargetProfile : nullptr;
        if (!CaptureProfile(player, &current, error, revision,
            !useSwitchTarget, seed))
            return false;
        runtime.ActiveRevision = current.Revision;
        if (!AppendProfileBackingTables(transaction, player, current, error))
            return false;
        return AppendStoredProfileToTransaction(transaction, player, current, error);
    }

    void Clear(Player* player)
    {
        if (player)
            _players.erase(player->GetGUID().GetCounter());
    }

private:
    bool CommitAcceptedTarget(Player* player, PlayerRuntime& runtime, std::string* error)
    {
        PendingSwitch const& pending = runtime.Pending;
        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        bool assembled = AppendProfileBackingTables(
            transaction, player, pending.TargetProfile, error) &&
            AppendStoredProfileToTransaction(
                transaction, player, pending.SourceProfile, error) &&
            AppendStoredProfileToTransaction(
                transaction, player, pending.TargetProfile, error);
        if (assembled)
            AppendSourcePetDetachment(transaction, player, pending);
        if (assembled)
            assembled = player->SaveToDB(
                transaction, false, false, false, false, true);

        bool committed = false;
        if (assembled)
        {
            TransactionCallback callback = CharacterDatabase.AsyncCommitTransaction(transaction);
            committed = callback.m_future.get();
        }
        if (!assembled || !committed)
        {
            std::string persistenceError = error && !error->empty()
                ? *error : "the class-switch transaction could not be committed";
            BeginSourceRecovery(player, runtime, persistenceError);
            if (error)
                *error = persistenceError;
            return false;
        }

        FinalizeSourcePetDetachment(player, pending);
        uint64 const revision = pending.TargetRevision;
        uint8 const classId = pending.ExpectedClass;
        uint64 const digest = pending.SnapshotDigest;
        NormalizeInMemoryAfterProfileWrite(player);
        runtime.ActiveRevision = revision;
        runtime.LastCompletedRevision = revision;
        runtime.LastCompletedDigest = digest;
        runtime.LastCompletedClass = classId;
        runtime.Pending = {};
        player->m_classSwitchTransitionPending.store(false, std::memory_order_release);
        sCharacterCache->UpdateCharacterClass(player->GetGUID(), classId);
        sCharacterCache->UpdateCharacterLevel(player->GetGUID(), player->GetLevel());
        SendState(player, StatusStage::Confirmed, RequestResult::Ok, runtime,
            false, revision, classId);
        TC_LOG_INFO("entities.player.classswitch",
            "Player {} class-switch committed after client acceptance: class={} revision={} digest={}",
            player->GetGUID().ToString(), uint32(classId), revision, digest);
        return true;
    }

    void BeginSourceRecovery(Player* player, PlayerRuntime& runtime,
        std::string const& reason)
    {
        PendingSwitch& pending = runtime.Pending;
        if (pending.RecoveryAttempts >= MAX_CLASS_SWITCH_RECOVERY_ATTEMPTS)
        {
            TC_LOG_ERROR("entities.player.classswitch",
                "Player {} exhausted class-switch recovery attempts: {}",
                player->GetGUID().ToString(), reason);
            player->GetSession()->KickPlayer(
                "Class-switch source recovery failed: " + reason
                + "; durable source state was preserved");
            return;
        }
        ++pending.RecoveryAttempts;
        pending.Phase = TransitionPhase::RecoveringSource;
        runtime.PersistSwitchTarget = false;
        runtime.PersistTargetProfile = {};

        StoredClassProfile source = pending.SourceProfile;
        std::string recoveryError;
        if (!ApplyProfile(player, source, &recoveryError, false, nullptr,
            pending.EquipmentRollback.Prepared
                ? &pending.EquipmentRollback : nullptr))
        {
            TC_LOG_ERROR("entities.player.classswitch",
                "Player {} could not restore source profile after {}: {}",
                player->GetGUID().ToString(), reason, recoveryError);
            player->GetSession()->KickPlayer(
                "Class-switch source restore failed: " + recoveryError);
            return;
        }
        RestoreSourcePet(player, pending);
        NormalizeInMemoryAfterProfileWrite(player);

        uint64 recoveryRevision = 0;
        if (!ReserveTransitionRevision(
            pending.TargetRevision, source.Revision, &recoveryRevision))
        {
            player->GetSession()->KickPlayer(
                "Class-switch recovery revision space is exhausted");
            return;
        }
        source.Revision = recoveryRevision;
        runtime.ActiveRevision = recoveryRevision;

        ActiveClassSnapshot snapshot;
        std::vector<uint8> encodedSnapshot;
        if (!BuildSnapshot(player, source, runtime, &snapshot, &recoveryError) ||
            (encodedSnapshot = EncodeActiveClassSnapshot(snapshot)).empty())
        {
            player->GetSession()->KickPlayer(
                "Class-switch source recovery snapshot could not be built: " + recoveryError);
            return;
        }

        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        bool assembled = AppendProfileBackingTables(
            transaction, player, source, &recoveryError) &&
            AppendStoredProfileToTransaction(transaction, player, source, &recoveryError);
        if (assembled)
            assembled = player->SaveToDB(
                transaction, false, false, false, false, true);
        bool committed = false;
        if (assembled)
        {
            TransactionCallback callback = CharacterDatabase.AsyncCommitTransaction(transaction);
            committed = callback.m_future.get();
        }
        if (!assembled || !committed)
        {
            player->GetSession()->KickPlayer(
                "Class-switch source recovery persistence failed: " + recoveryError);
            return;
        }

        pending.SourceProfile = source;
        pending.ExpectedClass = source.ClassId;
        pending.TargetRevision = recoveryRevision;
        pending.RecoveryRevision = recoveryRevision;
        pending.SnapshotPayload = encodedSnapshot;
        pending.SnapshotDigest =
            ComputeFnv1a64(encodedSnapshot.data(), encodedSnapshot.size());
        pending.Deadline = SystemTimePoint::clock::now() + CLIENT_RESPONSE_TIMEOUT;

        TC_LOG_ERROR("entities.player.classswitch",
            "Player {} entered bounded source recovery after {}; recovery_revision={} digest={}",
            player->GetGUID().ToString(), reason, recoveryRevision, pending.SnapshotDigest);
        SendState(player, StatusStage::CommitBegin, RequestResult::Ok, runtime,
            false, recoveryRevision, source.ClassId);
        player->SendProficiency(ITEM_CLASS_WEAPON, player->m_WeaponProficiency);
        player->SendProficiency(ITEM_CLASS_ARMOR, player->m_ArmorProficiency);
        player->SendInitialSpells();
        player->SendUnlearnSpells();
        player->SendInitialActionButtons();
        player->UpdateObjectVisibility();
        player->SendUpdateToPlayer(player);
        SendCustomPacket(player, SERVER_SNAPSHOT_OPCODE, encodedSnapshot);
    }

    bool CompleteSourceRecovery(Player* player, PlayerRuntime& runtime, std::string* error)
    {
        PendingSwitch const& pending = runtime.Pending;
        if (pending.Phase != TransitionPhase::RecoveringSource)
            return SetError(error, "class-switch response is not for source recovery");

        uint64 const revision = pending.RecoveryRevision;
        uint8 const classId = pending.ExpectedClass;
        runtime.ActiveRevision = revision;
        runtime.LastCompletedRevision = revision;
        runtime.LastCompletedDigest = pending.SnapshotDigest;
        runtime.LastCompletedClass = classId;
        runtime.Pending = {};
        player->m_classSwitchTransitionPending.store(false, std::memory_order_release);
        sCharacterCache->UpdateCharacterClass(player->GetGUID(), classId);
        sCharacterCache->UpdateCharacterLevel(player->GetGUID(), player->GetLevel());
        SendState(player, StatusStage::Confirmed, RequestResult::Ok, runtime,
            false, revision, classId);
        TC_LOG_INFO("entities.player.classswitch",
            "Player {} source recovery confirmed by client: class={} revision={}",
            player->GetGUID().ToString(), uint32(classId), revision);
        return true;
    }

    bool AppendProfileBackingTables(CharacterDatabaseTransaction const& transaction, Player* player,
        StoredClassProfile const& profile, std::string* error)
    {
        if (!transaction)
            return SetError(error, "class profile transaction is missing");
        LanguageEntryIds const languageIds = CollectLanguageEntryIds(player, &profile);
        std::vector<uint32> languageSpells(
            languageIds.Spells.begin(), languageIds.Spells.end());
        std::vector<uint32> languageSkills(
            languageIds.Skills.begin(), languageIds.Skills.end());
        for (std::string const& statement : BuildProfileBackingTableStatements(
            player->GetGUID().GetCounter(), profile, languageSpells, languageSkills))
            transaction->Append(statement.c_str());
        return true;
    }

    RequestResult SetError(std::string* output, RequestResult result, char const* error)
    {
        if (output)
            *output = error;
        return result;
    }

    bool SetError(std::string* output, char const* error)
    {
        if (output)
            *output = error;
        return false;
    }

    RequestResult Reject(Player* player, PlayerRuntime& runtime, RequestResult result, std::string const& error,
        std::string* output)
    {
        SetError(output, result, error.c_str());
        TC_LOG_WARN("entities.player.classswitch",
            "Player {} class-switch selection rejected: requested_class={} result={} error={}",
            player ? player->GetGUID().ToString() : std::string("unknown"),
            uint32(runtime.LastRequestedClass),
            uint32(result), error);
        SendState(player, StatusStage::Rejected, result, runtime);
        return result;
    }

    bool RejectSpell(Player* player, PlayerRuntime& runtime, RequestResult result,
        std::string const& message, std::string* output)
    {
        runtime.Pending = {};
        if (player)
            player->m_classSwitchTransitionPending.store(false, std::memory_order_release);
        SendState(player, StatusStage::Rejected, result, runtime);
        TC_LOG_ERROR("entities.player.classswitch",
            "Player {} class-switch spell rejected: result={} error={}",
            player ? player->GetGUID().ToString() : std::string("unknown"),
            uint32(result), message);
        if (output)
            *output = message;
        return false;
    }

    RuntimeClassDefinition const* FindDefinition(uint8 classId) const
    {
        auto itr = _definitions.find(classId);
        return itr == _definitions.end() ? nullptr : &itr->second;
    }

    LanguageEntryIds CollectLanguageEntryIds(Player* player,
        StoredClassProfile const* profile = nullptr) const
    {
        LanguageEntryIds ids;
        GetLanguageDescByID(LANG_UNIVERSAL);
        for (LanguageDesc const& language : lang_description)
        {
            if (language.spell_id)
                ids.Spells.insert(language.spell_id);
            if (language.skill_id)
                ids.Skills.insert(language.skill_id);
        }
        if (player)
        {
            for (auto const& [spellId, spell] : player->m_spells)
                if (spell.state != PLAYERSPELL_REMOVED && IsLanguageSpellId(spellId))
                    ids.Spells.insert(spellId);
            for (auto const& [skillId, status] : player->mSkillStatus)
                if (status.uState != SKILL_DELETED && IsLanguageSkillId(skillId))
                    ids.Skills.insert(skillId);
        }
        if (profile)
        {
            for (SpellProfile const& spell : profile->Spells)
                if (IsLanguageSpellId(spell.Id))
                    ids.Spells.insert(spell.Id);
            for (SkillProfile const& skill : profile->Skills)
                if (IsLanguageSkillId(skill.Id))
                    ids.Skills.insert(skill.Id);
        }
        return ids;
    }

    CharacterLanguageState CaptureCharacterLanguages(Player* player) const
    {
        CharacterLanguageState captured;
        LanguageEntryIds const languageIds = CollectLanguageEntryIds(player);
        for (auto const& [spellId, spell] : player->m_spells)
        {
            if (spell.state == PLAYERSPELL_REMOVED ||
                languageIds.Spells.find(spellId) == languageIds.Spells.end())
                continue;
            captured.Spells.push_back({ spellId, spell.state, spell.active,
                spell.dependent, spell.disabled });
        }
        for (auto const& [skillId, status] : player->mSkillStatus)
        {
            if (status.uState == SKILL_DELETED ||
                languageIds.Skills.find(skillId) == languageIds.Skills.end())
                continue;
            uint32 const valueData = player->GetUInt32Value(SkillValueIndex(status.pos));
            uint32 const bonusData = player->GetUInt32Value(SkillBonusIndex(status.pos));
            captured.Skills.push_back({
                skillId,
                uint16(PAIR32_HIPART(player->GetUInt32Value(SkillInfoIndex(status.pos)))),
                uint16(PAIR32_LOPART(valueData)),
                uint16(PAIR32_HIPART(valueData)),
                int16(PAIR32_LOPART(bonusData)),
                int16(PAIR32_HIPART(bonusData)),
                true
            });
        }
        return captured;
    }

    bool EnsureDefinitions(std::string* error = nullptr)
    {
        if (_definitionsLoaded)
            return _switchSpellId != 0;
        if (_definitionsLoading)
            return SetError(error, "class-switch definitions are already loading");
        _definitionsLoading = true;

        QueryResult config = WorldDatabase.Query(
            "SELECT switch_spell_id FROM ffxiv_class_switch_config WHERE id = 1");
        if (!config)
        {
            _definitionsLoading = false;
            return SetError(error, "ffxiv_class_switch_config has no switch spell definition");
        }
        _switchSpellId = config->Fetch()[0].GetUInt32();
        SpellInfo const* switchSpell = sSpellMgr->GetSpellInfo(_switchSpellId);
        if (!_switchSpellId || !switchSpell)
        {
            _definitionsLoading = false;
            return SetError(error, "configured class-switch spell is missing");
        }
        if (switchSpell->IsChanneled() || switchSpell->CalcCastTime() != 2000)
        {
            _definitionsLoading = false;
            return SetError(error, "configured class-switch spell must be a 2000 ms non-channeled cast");
        }

        QueryResult result = WorldDatabase.Query(
            "SELECT class_id, initial_level, race_mask, enabled, display_name "
            "FROM ffxiv_class_switch_classes ORDER BY class_id");
        if (!result)
        {
            _definitionsLoading = false;
            return SetError(error, "ffxiv_class_switch_classes has no configured classes");
        }

        do
        {
            Field* fields = result->Fetch();
            ClassDefinition definition;
            definition.ClassId = fields[0].GetUInt8();
            definition.InitialLevel = fields[1].GetUInt8();
            definition.RaceMask = fields[2].GetUInt32();
            definition.Enabled = fields[3].GetBool();
            definition.Name = fields[4].GetString();

            RuntimeClassDefinition runtime;
            runtime.Definition = definition;
            std::string validationError;
            if (!ValidateDefinition(runtime, &validationError))
            {
                TC_LOG_ERROR("sql.sql", "Class-switch class {} ignored: {}", uint32(definition.ClassId), validationError);
                continue;
            }
            _definitions.emplace(definition.ClassId, std::move(runtime));
        }
        while (result->NextRow());

        _definitionsLoaded = true;
        _definitionsLoading = false;
        if (_definitions.empty())
            return SetError(error, "no configured class has a complete playable definition");
        return true;
    }

    bool ValidateDefinition(RuntimeClassDefinition& definition, std::string* error)
    {
        definition.Definition.InitialLevel = ResolveClassStartLevel(
            definition.Definition.ClassId, definition.Definition.InitialLevel,
            uint8(sWorld->getIntConfig(CONFIG_START_PLAYER_LEVEL)),
            uint8(sWorld->getIntConfig(CONFIG_START_DEATH_KNIGHT_PLAYER_LEVEL)));
        ClassDefinition const& def = definition.Definition;
        if (!IsSupportedClassId(def.ClassId))
            return SetError(error, "class ID must be between 1 and 32");
        if (!def.Enabled)
            return true;
        if (!def.InitialLevel ||
            def.InitialLevel > std::min<uint32>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL), 80))
            return SetError(error, "initial class level is invalid");
        if (def.Name.empty())
            return SetError(error, "display name is empty");
        if (!def.RaceMask)
            return SetError(error, "class race mask is empty");

        ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(def.ClassId);
        if (!classEntry || !classEntry->SpellClassSet || !classEntry->Name[0] || !*classEntry->Name[0])
            return SetError(error, "ChrClasses record is missing or incomplete");
        if (classEntry->DisplayPower >= POWER_TYPES)
            return SetError(error, "class display power is outside the client snapshot range");
        definition.SpellClassSet = classEntry->SpellClassSet;
        definition.DisplayPower = uint8(classEntry->DisplayPower);
        std::vector<uint32> classActionSpells;
        for (uint32 spellId : sObjectMgr->GetClassCreateActionSpells(def.ClassId))
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                info && info->SpellFamilyName == definition.SpellClassSet)
                classActionSpells.push_back(spellId);

        PlayerClassLevelInfo classLevel;
        sObjectMgr->GetPlayerClassLevelInfo(def.ClassId, def.InitialLevel, &classLevel);
        if (!classLevel.basehealth)
            return SetError(error, "player_classlevelstats coverage is missing for the initial level");

        SpellAutoLearns const& autolearn = sObjectMgr->GetSpellAutolearns();
        for (uint8 race = 1; race <= MAX_RACES; ++race)
        {
            if (!(def.RaceMask & ClassIdToMask(race)))
                continue;
            PlayerInfo const* playerInfo = sObjectMgr->GetPlayerInfo(race, def.ClassId);
            if (!sChrRacesStore.LookupEntry(race) || !playerInfo)
                return SetError(error, "race/class create-info coverage is incomplete");
            for (PlayerCreateInfoSkill const& skill : playerInfo->skills)
                if (!GetSkillRaceClassInfo(skill.SkillId, race, def.ClassId))
                    return SetError(error, "race/class skill-line coverage is incomplete");
            PlayerLevelInfo levelInfo;
            sObjectMgr->GetPlayerLevelInfo(race, def.ClassId, def.InitialLevel, &levelInfo);
            if (!levelInfo.stats[STAT_STRENGTH] && !levelInfo.stats[STAT_AGILITY] &&
                !levelInfo.stats[STAT_STAMINA] && !levelInfo.stats[STAT_INTELLECT] &&
                !levelInfo.stats[STAT_SPIRIT])
                return SetError(error, "race/class level-stat coverage is incomplete");

            bool hasStarterSpell = false;
            if (!classActionSpells.empty())
                hasStarterSpell = true;
            for (uint32 level = 0; level <= def.InitialLevel && level < autolearn.size() && !hasStarterSpell; ++level)
            {
                for (SpellAutoLearn const& spell : autolearn[level])
                {
                    SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.spell);
                    if ((spell.classmask & ClassIdToMask(def.ClassId)) &&
                        (!spell.racemask || (spell.racemask & ClassIdToMask(race))) &&
                        info)
                    {
                        hasStarterSpell = true;
                        break;
                    }
                }
            }
            for (uint32 spellId : playerInfo->customSpells)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                if (info && std::max(info->BaseLevel, info->SpellLevel) <= def.InitialLevel)
                {
                    hasStarterSpell = true;
                    break;
                }
            }
            for (PlayerCreateInfoAction const& action : playerInfo->action)
            {
                if (action.type != ACTION_BUTTON_SPELL)
                    continue;
                if (sSpellMgr->GetSpellInfo(action.action))
                {
                    hasStarterSpell = true;
                    break;
                }
            }
            if (!hasStarterSpell)
            {
                std::string firstCustom = "none";
                if (!playerInfo->customSpells.empty())
                {
                    uint32 firstId = playerInfo->customSpells.front();
                    SpellInfo const* firstInfo = sSpellMgr->GetSpellInfo(firstId);
                    firstCustom = std::to_string(firstId);
                    firstCustom += firstInfo
                        ? " base=" + std::to_string(firstInfo->BaseLevel)
                            + " spell=" + std::to_string(firstInfo->SpellLevel)
                        : " missing";
                }
                std::string message = "installed race " + std::to_string(race)
                    + " class " + std::to_string(def.ClassId)
                    + " has no class-specific starter spell (custom="
                    + std::to_string(playerInfo->customSpells.size())
                    + ", actions=" + std::to_string(playerInfo->action.size())
                    + ", autolearn=" + std::to_string(autolearn.size())
                    + ", firstCustom=" + firstCustom + ")";
                return SetError(error, message.c_str());
            }
        }

        if (!ValidateClassCapabilityTables(def.ClassId, def.InitialLevel, error))
            return false;
        return true;
    }

    bool ValidateRaceClassAtLevel(uint8 race, ClassDefinition const& definition, uint8 level, std::string* error)
    {
        if (!(ClassIdToMask(race) & definition.RaceMask))
            return SetError(error, "race is not configured for the class");
        PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, definition.ClassId);
        if (!info)
            return SetError(error, "race/class create-info coverage is missing");
        PlayerLevelInfo levelInfo;
        sObjectMgr->GetPlayerLevelInfo(race, definition.ClassId, level, &levelInfo);
        if (!levelInfo.stats[STAT_STRENGTH] && !levelInfo.stats[STAT_AGILITY] &&
            !levelInfo.stats[STAT_STAMINA] && !levelInfo.stats[STAT_INTELLECT] &&
            !levelInfo.stats[STAT_SPIRIT])
            return SetError(error, "race/class level-stat coverage is missing");
        PlayerClassLevelInfo classLevel;
        sObjectMgr->GetPlayerClassLevelInfo(definition.ClassId, level, &classLevel);
        if (!classLevel.basehealth)
            return SetError(error, "class level-stat coverage is missing");
        if (!ValidateClassCapabilityTables(definition.ClassId, level, error))
            return false;
        return true;
    }

    bool ValidateClassCapabilityTables(uint8 classId, uint8 level, std::string* error)
    {
        if (!IsSupportedClassId(classId) || !level || level > 80)
            return SetError(error, "class capability level is invalid");

        uint32 const levelOffset = uint32(classId - 1) * GT_MAX_LEVEL + uint32(level - 1);
        if (!sGtChanceToMeleeCritBaseStore.LookupEntry(classId - 1) ||
            !sGtChanceToMeleeCritStore.LookupEntry(levelOffset) ||
            !sGtChanceToSpellCritBaseStore.LookupEntry(classId - 1) ||
            !sGtChanceToSpellCritStore.LookupEntry(levelOffset) ||
            !sGtOCTRegenHPStore.LookupEntry(levelOffset) ||
            !sGtRegenHPPerSptStore.LookupEntry(levelOffset))
            return SetError(error, "required class combat-rating or regeneration tables are incomplete");

        for (uint8 rating = 0; rating < MAX_COMBAT_RATING; ++rating)
        {
            if (!sGtCombatRatingsStore.LookupEntry(uint32(rating) * GT_MAX_LEVEL + level - 1) ||
                !sGtOCTClassCombatRatingScalarStore.LookupEntry(
                    uint32(classId - 1) * GT_MAX_RATING + rating + 1))
                return SetError(error, "required class combat-rating tables are incomplete");
        }
        return true;
    }

    bool IsReady(Player* player, bool afterSpellCompletion, char const** error) const
    {
        if (!player->IsInWorld())
        {
            *error = "class switching is unavailable before the player enters the world";
            return false;
        }
        if (player->IsInCombat())
        {
            *error = "class switching is unavailable in combat";
            return false;
        }
        if (!player->IsAlive())
        {
            *error = "class switching is unavailable while dead";
            return false;
        }
        if (player->GetVehicle() || player->GetVehicleBase())
        {
            *error = "class switching is unavailable in a vehicle";
            return false;
        }
        if (player->GetCharmed())
        {
            *error = "class switching is unavailable while controlling another unit";
            return false;
        }
        if (player->IsBeingTeleported())
        {
            *error = "class switching is unavailable during teleport";
            return false;
        }
        if (player->GetShapeshiftForm() != FORM_NONE)
        {
            *error = "leave the current form before changing class";
            return false;
        }
        if (!afterSpellCompletion && player->IsNonMeleeSpellCast(false, false, true))
        {
            *error = "finish casting before selecting a class";
            return false;
        }
        return true;
    }

    ProfileLoadStatus LoadProfile(Player* player, uint8 classId, StoredClassProfile* profile, std::string* error)
    {
        if (!profile)
        {
            SetError(error, "profile output is null");
            return ProfileLoadStatus::DatabaseError;
        }
        QueryResult result = CharacterDatabase.PQuery(
            "SELECT profile FROM ffxiv_class_switch_profiles WHERE guid = {} AND class_id = {}",
            player->GetGUID().GetCounter(), classId);
        if (!result)
        {
            QueryResult count = CharacterDatabase.PQuery(
                "SELECT COUNT(*) FROM ffxiv_class_switch_profiles WHERE guid = {} AND class_id = {}",
                player->GetGUID().GetCounter(), classId);
            if (!count)
            {
                SetError(error, "class profile database query failed");
                return ProfileLoadStatus::DatabaseError;
            }
            if (count->Fetch()[0].GetUInt64() == 0)
                return ProfileLoadStatus::Missing;
            SetError(error, "class profile row exists but could not be read");
            return ProfileLoadStatus::DatabaseError;
        }
        std::vector<uint8> binary = result->Fetch()[0].GetBinary();
        if (!DecodeStoredProfile(binary.data(), binary.size(), profile, error))
            return ProfileLoadStatus::Corrupt;
        if (profile->ClassId != classId)
        {
            SetError(error, "class profile row contains a different class ID");
            return ProfileLoadStatus::Corrupt;
        }
        LanguageEntryIds const languageIds = CollectLanguageEntryIds(player, profile);
        uint32 removedLanguageSpells = 0;
        uint32 removedLanguageSkills = 0;
        StripClassOwnedLanguages(profile, languageIds.Spells, languageIds.Skills,
            &removedLanguageSpells, &removedLanguageSkills);
        if (removedLanguageSpells || removedLanguageSkills)
        {
            TC_LOG_WARN("entities.player.classswitch",
                "Player {} ignored {} language spells and {} language skills stored in class {} profile; character language state remains authoritative",
                player->GetGUID().ToString(), removedLanguageSpells,
                removedLanguageSkills, uint32(classId));
        }
        if (profile->EquipmentMigrated && classId == player->GetClass())
        {
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                {
                    StoredEquipmentItem& equipment = profile->Equipment[slot];
                    equipment.ItemId = item->GetEntry();
                    equipment.Enchantments[0] = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
                    equipment.Enchantments[1] = item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
                    equipment.EquippedGuid = item->GetGUID().GetRawValue();
                }
            }
            profile->EquipmentMigrated = false;
        }
        uint32 repaired = 0;
        if (!RepairActionSpells(player, profile, &repaired, error) ||
            !ValidateStoredProfile(*profile, error))
            return ProfileLoadStatus::Corrupt;
        if (repaired)
            TC_LOG_WARN("entities.player.classswitch",
                "Player {} normalized {} class-action entries in stored class {} profile",
                player->GetGUID().ToString(), repaired, uint32(classId));
        return ProfileLoadStatus::Loaded;
    }

    bool LoadOrCreateProfile(Player* player, uint8 classId, StoredClassProfile* profile,
        StoredClassProfile const& sharedSource, std::string* error)
    {
        ProfileLoadStatus loadStatus = LoadProfile(player, classId, profile, error);
        if (loadStatus == ProfileLoadStatus::Loaded)
        {
            if (profile->EquipmentMigrated)
            {
                // Legacy profiles did not store a loadout. Treat the visible
                // source gear as the migration candidate so selecting an old
                // class does not silently strip gear. Target compatibility is
                // validated immediately after loading.
                profile->Equipment = sharedSource.Equipment;
                profile->EquipmentMigrated = false;
            }
            return true;
        }
        if (loadStatus != ProfileLoadStatus::Missing)
            return false;
        if (classId != player->GetClass())
            return CreateStarterProfile(player, classId, sharedSource, profile, error);

        StoredClassProfile captured;
        if (!CaptureProfile(player, &captured, error, 1))
            return false;
        *profile = std::move(captured);
        return true;
    }

    bool LoadOrCreateProfile(Player* player, uint8 classId, StoredClassProfile* profile, std::string* error)
    {
        StoredClassProfile emptySource;
        return LoadOrCreateProfile(player, classId, profile, emptySource, error);
    }

    bool CreateStarterProfile(Player* player, uint8 classId, StoredClassProfile const& source,
        StoredClassProfile* profile, std::string* error)
    {
        RuntimeClassDefinition const* definition = FindDefinition(classId);
        if (!definition)
            return SetError(error, "target class definition is missing");

        StoredClassProfile created;
        created.Revision = 1;
        created.ClassId = classId;
        created.Level = definition->Definition.InitialLevel;
        created.NextLevelXp = sObjectMgr->GetXPForLevel(created.Level);
        created.ActiveSpec = 0;
        created.SpecCount = 1;
        created.FreeTalentPoints = TalentPointsForClassLevel(classId, created.Level);
        created.HealthPercent = 100.0f;
        created.PowerPercent.fill(100.0f);

        std::set<uint32> spellIds;
        SpellAutoLearns const& autolearn = sObjectMgr->GetSpellAutolearns();
        for (uint32 level = 0; level <= created.Level && level < autolearn.size(); ++level)
        {
            for (SpellAutoLearn const& spell : autolearn[level])
            {
                if ((spell.classmask && !(spell.classmask & ClassIdToMask(classId))) ||
                    (spell.racemask && !(spell.racemask & ClassIdToMask(player->GetRace()))))
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.spell);
                if (!info || (info->SpellFamilyName && info->SpellFamilyName != definition->SpellClassSet))
                    continue;
                spellIds.insert(spell.spell);
            }
        }
        PlayerInfo const* createInfo = sObjectMgr->GetPlayerInfo(player->GetRace(), classId);
        if (!createInfo)
            return SetError(error, "race/class create-info coverage is missing");
        for (uint32 spellId : createInfo->customSpells)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || std::max(info->BaseLevel, info->SpellLevel) > created.Level)
                continue;
            spellIds.insert(spellId);
        }
        for (uint32 spellId : sObjectMgr->GetClassCreateActionSpells(classId))
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                info && info->SpellFamilyName == definition->SpellClassSet)
                spellIds.insert(spellId);
        for (PlayerCreateInfoAction const& action : createInfo->action)
            if (action.type == ACTION_BUTTON_SPELL && sSpellMgr->GetSpellInfo(action.action))
                spellIds.insert(action.action);

        // Shared classless spells, professions, riding, and known mounts remain available.
        for (SpellProfile const& spell : source.Spells)
        {
            if (IsLanguageSpellId(spell.Id))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.Id);
            if (info && (!info->SpellFamilyName || info->IsProfessionOrRiding()))
                spellIds.insert(spell.Id);
        }
        spellIds.insert(_switchSpellId);

        for (uint32 spellId : spellIds)
        {
            SpellProfile& spell = created.Spells.emplace_back();
            spell.Id = spellId;
            spell.State = PLAYERSPELL_NEW;
            spell.Active = true;
        }

        std::set<uint32> skillIds;
        for (PlayerCreateInfoSkill const& createSkill : createInfo->skills)
        {
            SkillProfile skill;
            skill.Id = createSkill.SkillId;
            skill.Step = createSkill.Rank;
            CalculateStarterSkillValue(player->GetRace(), classId, created.Level, createSkill.SkillId,
                createSkill.Rank, &skill);
            created.Skills.push_back(skill);
            skillIds.insert(skill.Id);
        }
        for (SkillProfile const& skill : source.Skills)
        {
            if (IsLanguageSkillId(skill.Id))
                continue;
            if (!skill.Shared)
                continue;
            auto itr = std::find_if(created.Skills.begin(), created.Skills.end(),
                [&skill](SkillProfile const& value) { return value.Id == skill.Id; });
            if (itr == created.Skills.end())
                created.Skills.push_back(skill);
        }

        for (PlayerCreateInfoAction const& action : createInfo->action)
        {
            if (action.button >= ACTION_SLOTS)
                continue;
            created.Groups[0].Actions[action.button] = action.action | (uint32(action.type) << 24);
        }
        for (PlayerCreateInfoAction const& action : sObjectMgr->GetClassCreateActions(classId))
        {
            if (action.button >= ACTION_SLOTS || created.Groups[0].Actions[action.button])
                continue;
            if (action.type == ACTION_BUTTON_SPELL)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(action.action);
                if (!info || info->SpellFamilyName != definition->SpellClassSet)
                    continue;
            }
            created.Groups[0].Actions[action.button] = action.action | (uint32(action.type) << 24);
        }
        for (uint32 slot = 0; slot < ACTION_SLOTS; ++slot)
        {
            uint32 const sourceAction = source.Groups[source.ActiveSpec].Actions[slot];
            uint8 const type = uint8(sourceAction >> 24);
            if (sourceAction && type != ACTION_BUTTON_SPELL && type != ACTION_BUTTON_C)
                if (!created.Groups[0].Actions[slot])
                    created.Groups[0].Actions[slot] = sourceAction;
        }

        uint32 repaired = 0;
        if (!RepairActionSpells(player, &created, &repaired, error))
            return false;
        if (!ValidateStoredProfile(created, error))
        {
            if (error)
                *error = "starter profile for class " + std::to_string(classId)
                    + " is invalid: " + *error;
            return false;
        }
        *profile = std::move(created);
        return true;
    }

    void CalculateStarterSkillValue(uint8 race, uint8 classId, uint8 level, uint32 skillId,
        uint16 rank, SkillProfile* profile)
    {
        profile->Max = uint16(std::min<uint32>(level * 5, sWorld->GetConfigMaxSkillValue()));
        profile->Value = 1;
        SkillRaceClassInfoEntry const* rcInfo = GetSkillRaceClassInfo(skillId, race, classId);
        if (!rcInfo)
        {
            profile->Value = profile->Max = 1;
            profile->Shared = IsProfessionOrRidingSkill(skillId);
            return;
        }
        if (rcInfo->Flags & SKILL_FLAG_ALWAYS_MAX_VALUE)
            profile->Value = profile->Max;
        else if (classId == CLASS_DEATH_KNIGHT)
            profile->Value = uint16(std::min<uint32>(std::max<uint32>(1, (level - 1) * 5), profile->Max));
        profile->Shared = IsProfessionOrRidingSkill(skillId)
            || GetSkillRangeType(rcInfo) == SKILL_RANGE_LANGUAGE;
        if (GetSkillRangeType(rcInfo) == SKILL_RANGE_MONO)
            profile->Value = profile->Max = 1;
        else if (GetSkillRangeType(rcInfo) == SKILL_RANGE_RANK)
        {
            SkillTiersEntry const* tier = sSkillTiersStore.LookupEntry(rcInfo->SkillTierID);
            if (tier)
                profile->Max = tier->Value[std::min<uint32>(
                    std::max<int32>(int32(rank) - 1, 0),
                    MAX_SKILL_STEP - 1)];
        }
    }

    bool IsExplicitClassStarterSpell(Player* player, uint8 classId, uint32 spellId)
    {
        PlayerInfo const* createInfo = sObjectMgr->GetPlayerInfo(player->GetRace(), classId);
        if (!createInfo)
            return false;
        if (std::find(createInfo->customSpells.begin(), createInfo->customSpells.end(), spellId)
            != createInfo->customSpells.end())
            return true;
        for (PlayerCreateInfoAction const& action : createInfo->action)
            if (action.type == ACTION_BUTTON_SPELL && action.action == spellId)
                return true;
        std::vector<uint32> classActions = sObjectMgr->GetClassCreateActionSpells(classId);
        return std::find(classActions.begin(), classActions.end(), spellId) != classActions.end();
    }

    bool RepairActionSpells(Player* player, StoredClassProfile* profile,
        uint32* repaired, std::string* error)
    {
        if (repaired)
            *repaired = 0;
        ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(profile->ClassId);
        if (!classEntry)
            return SetError(error, "stored profile class entry is missing");

        std::set<uint32> activeActionRanks;
        for (SpellProfile const& known : profile->Spells)
            if (known.Active && !known.Disabled)
                activeActionRanks.insert(known.Id);

        for (uint8 group = 0; group < profile->SpecCount; ++group)
        {
            for (uint32& packed : profile->Groups[group].Actions)
            {
                uint32 const spellId = packed & 0x00FFFFFFu;
                uint8 const type = uint8(packed >> 24);
                if (!spellId || type != ACTION_BUTTON_SPELL)
                    continue;

                SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
                if (!spell)
                {
                    packed = 0;
                    if (repaired)
                        ++*repaired;
                    continue;
                }
                if (spell->SpellFamilyName &&
                    spell->SpellFamilyName != classEntry->SpellClassSet)
                {
                    packed = 0;
                    if (repaired)
                        ++*repaired;
                    continue;
                }

                std::vector<uint32> spellChain;
                uint32 const firstRank = sSpellMgr->GetFirstSpellInChain(spellId);
                for (uint32 candidate = firstRank; candidate; candidate =
                    sSpellMgr->GetNextSpellInChain(candidate))
                    spellChain.push_back(candidate);
                uint32 const activeRank =
                    NormalizeActionSpellRank(spellId, spellChain, activeActionRanks);
                if (!activeRank)
                {
                    packed = 0;
                    if (repaired)
                        ++*repaired;
                    continue;
                }
                if (activeRank != spellId)
                {
                    packed = (packed & 0xFF000000u) | activeRank;
                    if (repaired)
                        ++*repaired;
                }
                spell = sSpellMgr->GetSpellInfo(activeRank);
                if (!spell)
                {
                    packed = 0;
                    continue;
                }
                if (spell->SpellFamilyName)
                {
                    SkillLineAbilityMapBounds bounds =
                        sSpellMgr->GetSkillLineAbilityMapBounds(activeRank);
                    bool covered = false;
                    for (auto itr = bounds.first; itr != bounds.second; ++itr)
                    {
                        if (GetSkillRaceClassInfo(
                            itr->second->SkillLine, player->GetRace(), profile->ClassId))
                        {
                            covered = true;
                            break;
                        }
                    }
                    if (!covered &&
                        !IsExplicitClassStarterSpell(player, profile->ClassId, activeRank))
                    {
                        packed = 0;
                        if (repaired)
                            ++*repaired;
                    }
                }
            }
        }
        return true;
    }

    bool CaptureProfile(Player* player, StoredClassProfile* profile, std::string* error,
        uint64 revision = 1, bool loadInactiveActions = true,
        StoredClassProfile const* profileSeed = nullptr)
    {
        StoredClassProfile captured;
        LanguageEntryIds const languageIds = CollectLanguageEntryIds(player);
        captured.Revision = revision ? revision : 1;
        captured.ClassId = player->GetClass();
        captured.Level = player->GetLevel();
        captured.Xp = player->GetXP();
        captured.NextLevelXp = player->GetXPForNextLevel();
        captured.ActiveSpec = player->m_activeSpec;
        captured.SpecCount = std::max<uint8>(player->m_specsCount, 1);
        captured.FreeTalentPoints = player->GetFreeTalentPoints();
        captured.HealthPercent = player->GetMaxHealth()
            ? ClampPercent(100.0f * float(player->GetHealth()) / float(player->GetMaxHealth()))
            : 100.0f;
        for (uint8 power = 0; power < POWER_TYPES; ++power)
            captured.PowerPercent[power] = player->GetMaxPower(Powers(power))
                ? ClampPercent(100.0f * float(player->GetPower(Powers(power))) /
                    float(player->GetMaxPower(Powers(power))))
                : 0.0f;
        if (profileSeed && profileSeed->ClassId == player->GetClass())
            captured.Groups = profileSeed->Groups;
        if (player->m_runes && player->HasRunes())
        {
            captured.Runes.Count = MAX_RUNES;
            for (uint8 rune = 0; rune < captured.Runes.Count; ++rune)
            {
                captured.Runes.Types[rune] = int32(player->m_runes->runes[rune].CurrentRune);
                captured.Runes.BaseTypes[rune] = int32(player->m_runes->runes[rune].BaseRune);
                captured.Runes.Cooldowns[rune] = player->m_runes->runes[rune].Cooldown;
                captured.Runes.LastUsed[rune] = uint32(player->m_runes->lastUsedRune);
            }
        }

        for (auto const& [spellId, spell] : player->m_spells)
        {
            if (spell.state == PLAYERSPELL_REMOVED ||
                languageIds.Spells.find(spellId) != languageIds.Spells.end())
                continue;
            SpellProfile& saved = captured.Spells.emplace_back();
            saved.Id = spellId;
            saved.State = spell.state;
            saved.Active = spell.active;
            saved.Dependent = spell.dependent;
            saved.Disabled = spell.disabled;
        }

        for (auto const& [skillId, status] : player->mSkillStatus)
        {
            if (status.uState == SKILL_DELETED ||
                languageIds.Skills.find(skillId) != languageIds.Skills.end())
                continue;
            uint32 const valueData = player->GetUInt32Value(SkillValueIndex(status.pos));
            uint32 const bonusData = player->GetUInt32Value(SkillBonusIndex(status.pos));
            SkillProfile skill;
            skill.Id = skillId;
            skill.Step = uint16(PAIR32_HIPART(player->GetUInt32Value(SkillInfoIndex(status.pos))));
            skill.Value = uint16(PAIR32_LOPART(valueData));
            skill.Max = uint16(PAIR32_HIPART(valueData));
            skill.TemporaryBonus = int16(PAIR32_LOPART(bonusData));
            skill.PermanentBonus = int16(PAIR32_HIPART(bonusData));
            SkillRaceClassInfoEntry const* skillInfo =
                GetSkillRaceClassInfo(skillId, player->GetRace(), player->GetClass());
            skill.Shared = IsProfessionOrRidingSkill(skillId)
                || (skillInfo && GetSkillRangeType(skillInfo) == SKILL_RANGE_LANGUAGE);
            captured.Skills.push_back(skill);
        }

        for (uint8 spec = 0; spec < captured.SpecCount; ++spec)
        {
            for (auto const& [spellId, talent] : *player->m_talents[spec])
            {
                if (talent->state == PLAYERSPELL_REMOVED)
                    continue;
                captured.Groups[spec].Talents.push_back({ spellId, spec });
            }
            for (uint8 glyph = 0; glyph < GLYPH_SLOTS; ++glyph)
                captured.Groups[spec].Glyphs[glyph] = player->m_Glyphs[spec][glyph];
        }
        for (auto const& [button, action] : player->m_actionButtons)
            captured.Groups[captured.ActiveSpec].Actions[button] = action.packedData;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                StoredEquipmentItem& equipment = captured.Equipment[slot];
                equipment.ItemId = item->GetEntry();
                equipment.Enchantments[0] = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
                equipment.Enchantments[1] = item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
                equipment.EquippedGuid = item->GetGUID().GetRawValue();
            }
        }

        if (loadInactiveActions)
        {
            QueryResult actions = CharacterDatabase.PQuery(
                "SELECT spec, button, action, type FROM character_action WHERE guid = {}",
                player->GetGUID().GetCounter());
            if (!actions)
            {
                QueryResult count = CharacterDatabase.PQuery(
                    "SELECT COUNT(*) FROM character_action WHERE guid = {}",
                    player->GetGUID().GetCounter());
                if (!count)
                    return SetError(error, "character action query failed");
                if (count->Fetch()[0].GetUInt64() != 0)
                    return SetError(error, "character action rows exist but could not be read");
            }
            if (actions)
            {
                do
                {
                    Field* fields = actions->Fetch();
                    uint8 spec = fields[0].GetUInt8();
                    uint8 button = fields[1].GetUInt8();
                    if (spec < captured.SpecCount && button < ACTION_SLOTS && spec != captured.ActiveSpec)
                        captured.Groups[spec].Actions[button] =
                            fields[2].GetUInt32() | (uint32(fields[3].GetUInt8()) << 24);
                }
                while (actions->NextRow());
            }
        }

        uint32 repaired = 0;
        if (!RepairActionSpells(player, &captured, &repaired, error))
            return false;
        if (repaired)
            TC_LOG_WARN("entities.player.classswitch",
                "Player {} normalized {} class-action entries",
                player->GetGUID().ToString(), repaired);
        if (!ValidateStoredProfile(captured, error))
            return false;
        *profile = std::move(captured);
        return true;
    }

    bool ValidateTargetProfile(Player* player, RuntimeClassDefinition const& definition,
        StoredClassProfile const& profile, std::string* error)
    {
        if (!ValidateStoredProfile(profile, error))
            return false;
        if (profile.ClassId != definition.Definition.ClassId)
            return SetError(error, "target profile belongs to a different class");
        if (!(definition.Definition.RaceMask & ClassIdToMask(player->GetRace())))
            return SetError(error, "target profile is unavailable for the persistent race");
        if (!ValidateRaceClassAtLevel(player->GetRace(), definition.Definition, profile.Level, error))
            return false;
        bool const hasRunes = sObjectMgr->HasRunesForRaceClass(
            player->GetRace(), definition.Definition.ClassId);
        if ((!hasRunes && profile.Runes.Count) ||
            (hasRunes && profile.Runes.Count && profile.Runes.Count != MAX_RUNES))
            return SetError(error, "target profile rune state is incompatible with the class");

        bool hasRuneCapabilitySpell = false;
        for (SpellProfile const& spell : profile.Spells)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.Id);
            if (!info || !IsSpellAllowedForClassProfile(
                info->SpellFamilyName, definition.SpellClassSet, info->IsProfessionOrRiding()))
                return SetError(error, "target profile contains a missing or foreign-class spell");
            if (spell.Active && !spell.Disabled &&
                (info->PowerType == POWER_RUNE || info->RuneCostID))
                hasRuneCapabilitySpell = true;
            if (info->SpellFamilyName)
            {
                SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spell.Id);
                bool covered = false;
                for (auto itr = bounds.first; itr != bounds.second; ++itr)
                    if (GetSkillRaceClassInfo(itr->second->SkillLine, player->GetRace(), profile.ClassId))
                    {
                        covered = true;
                        break;
                    }
                if (!covered &&
                    !IsExplicitClassStarterSpell(player, definition.Definition.ClassId, spell.Id))
                    return SetError(error, "target spell lacks class-aware skill-line coverage");
            }
        }
        if (hasRunes != hasRuneCapabilitySpell)
            return SetError(error, "target rune support and known spell capabilities disagree");
        for (SkillProfile const& skill : profile.Skills)
            if (!sSkillLineStore.LookupEntry(skill.Id) ||
                !GetSkillRaceClassInfo(skill.Id, player->GetRace(), profile.ClassId))
                return SetError(error, "target profile contains an unknown skill");
        for (uint8 group = 0; group < profile.SpecCount; ++group)
        {
            for (TalentProfile const& talent : profile.Groups[group].Talents)
            {
                bool found = false;
                for (uint32 talentId = 0; talentId < sTalentStore.GetNumRows() && !found; ++talentId)
                {
                    TalentEntry const* talentEntry = sTalentStore.LookupEntry(talentId);
                    if (!talentEntry)
                        continue;
                    TalentTabEntry const* tab = sTalentTabStore.LookupEntry(talentEntry->TabID);
                    if (!tab || !(tab->ClassMask & ClassIdToMask(profile.ClassId)))
                        continue;
                    for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
                        if (talentEntry->SpellRank[rank] == talent.SpellId)
                        {
                            found = true;
                            break;
                        }
                }
                if (!found)
                    return SetError(error, "target profile contains an unavailable talent");
            }
            for (uint32 glyphId : profile.Groups[group].Glyphs)
            {
                if (!glyphId)
                    continue;
                GlyphPropertiesEntry const* glyph = sGlyphPropertiesStore.LookupEntry(glyphId);
                SpellInfo const* glyphSpell = glyph ? sSpellMgr->GetSpellInfo(glyph->SpellID) : nullptr;
                if (!glyphSpell ||
                    (glyphSpell->SpellFamilyName && glyphSpell->SpellFamilyName != definition.SpellClassSet))
                    return SetError(error, "target profile contains an unavailable glyph");
            }
        }
        return true;
    }

    bool ValidateEquipment(Player* player, StoredClassProfile const& profile, std::string* error)
    {
        uint32 classMask = ClassIdToMask(profile.ClassId);
        uint32 raceMask = ClassIdToMask(player->GetRace());
        uint32 weaponProficiency = 0;
        uint32 armorProficiency = 0;
        bool hasDualWield = false;
        bool hasTitanGrip = false;
        std::set<uint32> knownSpells;
        for (SpellProfile const& known : profile.Spells)
        {
            if (known.Active && !known.Disabled)
                knownSpells.insert(known.Id);
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(known.Id);
            if (!spell)
                continue;
            if (!known.Active || known.Disabled)
                continue;
            for (SpellEffectInfo const& effect : spell->GetEffects())
            {
                hasDualWield = hasDualWield || effect.Effect == SPELL_EFFECT_DUAL_WIELD;
                hasTitanGrip = hasTitanGrip || effect.Effect == SPELL_EFFECT_TITAN_GRIP;
            }
            if (spell->EquippedItemSubClassMask <= 0)
                continue;
            if (spell->EquippedItemClass == ITEM_CLASS_WEAPON)
                weaponProficiency |= uint32(spell->EquippedItemSubClassMask);
            else if (spell->EquippedItemClass == ITEM_CLASS_ARMOR)
                armorProficiency |= uint32(spell->EquippedItemSubClassMask);
        }

        Item* mainhand = nullptr;
        Item* offhand = nullptr;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            StoredEquipmentItem const& stored = profile.Equipment[slot];
            if (!stored.EquippedGuid)
                continue;
            Item* item = player->GetItemByGuid(ObjectGuid(stored.EquippedGuid));
            if (!item || item->GetEntry() != stored.ItemId ||
                item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) != stored.Enchantments[0] ||
                item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) != stored.Enchantments[1])
                return SetError(error, "a class-owned equipment item is missing or no longer matches its profile");
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto)
                return SetError(error, "an equipped item has no template");
            if (player->FindEquipSlot(proto, slot, false) != slot)
                return SetError(error, "a class-owned equipment item does not fit its stored slot");
            if (proto->AllowableClass && !(proto->AllowableClass & classMask))
                return SetError(error, "an equipped item is restricted to another class");
            if (proto->AllowableRace && !(proto->AllowableRace & raceMask))
                return SetError(error, "an equipped item is restricted to another race");
            if (proto->RequiredLevel > profile.Level)
                return SetError(error, "an equipped item exceeds the target class level");
            if (proto->RequiredSpell && knownSpells.find(proto->RequiredSpell) == knownSpells.end())
                return SetError(error, "an equipped item requires a spell absent from the target class");
            if (proto->RequiredSkill)
            {
                auto skill = std::find_if(profile.Skills.begin(), profile.Skills.end(),
                    [proto](SkillProfile const& value) { return value.Id == proto->RequiredSkill; });
                if (skill == profile.Skills.end() || skill->Value < proto->RequiredSkillRank)
                    return SetError(error, "an equipped item requires a skill absent from the target class");
            }
            if (proto->SubClass < 32)
            {
                if (proto->Class == ITEM_CLASS_WEAPON && !(weaponProficiency & (1u << proto->SubClass)))
                    return SetError(error, "an equipped weapon is incompatible with target class proficiencies");
                if (proto->Class == ITEM_CLASS_ARMOR && !(armorProficiency & (1u << proto->SubClass)))
                    return SetError(error, "an equipped armor item is incompatible with target class proficiencies");
            }
            if (slot == EQUIPMENT_SLOT_MAINHAND)
                mainhand = item;
            else if (slot == EQUIPMENT_SLOT_OFFHAND)
                offhand = item;
        }
        if (offhand && offhand->GetTemplate())
        {
            uint32 const offhandType = offhand->GetTemplate()->InventoryType;
            if ((offhandType == INVTYPE_WEAPON || offhandType == INVTYPE_WEAPONOFFHAND) &&
                !hasDualWield)
                return SetError(error, "the target class cannot dual wield the equipped offhand weapon");
            if (offhandType == INVTYPE_2HWEAPON && (!hasDualWield || !hasTitanGrip))
                return SetError(error, "the target class cannot wield the equipped two-handed offhand item");
            if (mainhand && mainhand->GetTemplate() &&
                mainhand->GetTemplate()->InventoryType == INVTYPE_2HWEAPON && !hasTitanGrip)
                return SetError(error, "the target class cannot use the equipped two-handed mainhand with an offhand item");
        }
        return true;
    }

    bool ApplyEquipmentProfile(Player* player, StoredClassProfile const& target,
        std::string* error, EquipmentRollbackPlan* rollbackPlan = nullptr)
    {
        struct StoredItemMove
        {
            uint8 Slot = 0;
            Item* Item = nullptr;
            uint16 Destination = 0;
        };
        std::vector<StoredItemMove> outgoing;
        std::map<uint8, uint16> outgoingDestinations;
        if (rollbackPlan)
            *rollbackPlan = {};

        std::array<uint64, EQUIPMENT_SLOTS> currentEquipment{};
        std::array<uint64, EQUIPMENT_SLOTS> targetEquipment{};
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                currentEquipment[slot] = item->GetGUID().GetRawValue();
            targetEquipment[slot] = target.Equipment[slot].EquippedGuid;
        }
        for (uint8 slot : BuildChangedEquipmentSlots(currentEquipment, targetEquipment))
        {
            Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!current)
                continue;

            outgoing.push_back({ slot, current, 0 });
        }

        std::vector<EquipmentBagCandidate> candidates;
        std::set<uint16> candidatePositions;
        auto addCandidate = [&](uint16 position, bool freedByTargetEquip) {
            if (position && candidatePositions.insert(position).second)
                candidates.push_back({ position, freedByTargetEquip });
        };

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                addCandidate(uint16(INVENTORY_SLOT_BAG_0) << 8 | slot, false);
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            if (Bag* bag = player->GetBagByPos(bagSlot))
            {
                for (uint8 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (!player->GetItemByPos(bagSlot, slot))
                        addCandidate(uint16(bagSlot) << 8 | slot, false);
            }
        }

        // Bag slots occupied by target gear become valid destinations once that
        // gear is detached for equipping. Include them in the complete matching.
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            StoredEquipmentItem const& desired = target.Equipment[slot];
            if (!desired.EquippedGuid)
                continue;
            Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (current && current->GetGUID().GetRawValue() == desired.EquippedGuid)
                continue;
            Item* item = player->GetItemByGuid(ObjectGuid(desired.EquippedGuid));
            if (!item || item->GetEntry() != desired.ItemId)
                return SetError(error, "a class-owned equipment item is missing or no longer matches its profile");
            if (player->FindEquipSlot(item->GetTemplate(), slot, false) != slot)
                return SetError(error, "a class-owned equipment item does not fit its stored slot");
            if (!item->IsEquipped())
                addCandidate(item->GetPos(), true);
        }

        std::vector<EquipmentBagDemand> demands;
        demands.reserve(outgoing.size());
        for (StoredItemMove const& move : outgoing)
        {
            EquipmentBagDemand demand;
            for (EquipmentBagCandidate const& candidate : candidates)
            {
                uint8 const bag = uint8(candidate.Position >> 8);
                uint8 const slot = uint8(candidate.Position & 0xFF);
                ItemPosCountVec destination;
                InventoryResult const result = player->CanStoreItem(
                    bag, slot, destination, move.Item, candidate.FreedByTargetEquip);
                if (result == EQUIP_ERR_OK && destination.size() == 1 &&
                    destination.front().count == 1)
                    demand.AllowedPositions.push_back(candidate.Position);
            }
            demands.push_back(std::move(demand));
        }

        std::vector<uint16> reservedDestinations;
        if (!PlanEquipmentBagReservations(candidates, demands, &reservedDestinations))
            return SetError(error,
                "the target class loadout needs a complete free bag destination for each unequipped item");
        for (size_t index = 0; index < outgoing.size(); ++index)
        {
            outgoing[index].Destination = reservedDestinations[index];
            outgoingDestinations[outgoing[index].Slot] = reservedDestinations[index];
        }

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            StoredEquipmentItem const& desired = target.Equipment[slot];
            if (!desired.EquippedGuid)
                continue;
            Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (current && current->GetGUID().GetRawValue() == desired.EquippedGuid)
                continue;
            Item* item = player->GetItemByGuid(ObjectGuid(desired.EquippedGuid));
            if (rollbackPlan)
            {
                if (item->IsEquipped())
                {
                    auto destination = outgoingDestinations.find(item->GetSlot());
                    if (destination == outgoingDestinations.end())
                        return SetError(error, "the equipment rollback plan is incomplete");
                    rollbackPlan->TargetBagDestinations[slot] = destination->second;
                }
                else
                    rollbackPlan->TargetBagDestinations[slot] = item->GetPos();
            }
        }
        if (rollbackPlan)
            rollbackPlan->Prepared = true;

        std::set<uint16> usedDestinations(
            reservedDestinations.begin(), reservedDestinations.end());
        std::map<uint64, Item*> detachedTargets;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            StoredEquipmentItem const& desired = target.Equipment[slot];
            if (!desired.EquippedGuid)
                continue;
            Item* item = player->GetItemByGuid(ObjectGuid(desired.EquippedGuid));
            if (!item || item->IsEquipped() || !usedDestinations.count(item->GetPos()))
                continue;
            uint64 const guid = item->GetGUID().GetRawValue();
            player->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
            detachedTargets[guid] = item;
        }

        for (StoredItemMove const& move : outgoing)
        {
            player->RemoveItem(INVENTORY_SLOT_BAG_0, move.Slot, false);
            ItemPosCountVec destination{ ItemPosCount(move.Destination, 1) };
            if (player->StoreItem(destination, move.Item, true) != move.Item)
                return SetError(error, "an unequipped class item could not be stored in its reserved bag slot");
        }

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            StoredEquipmentItem const& desired = target.Equipment[slot];
            if (!desired.EquippedGuid)
                continue;
            Item* item = player->GetItemByGuid(ObjectGuid(desired.EquippedGuid));
            auto detached = detachedTargets.find(desired.EquippedGuid);
            if (detached != detachedTargets.end())
                item = detached->second;
            if (!item)
                return SetError(error, "a class-owned equipment item disappeared during application");
            if (item->GetBagSlot() != NULL_BAG || item->GetSlot() != NULL_SLOT)
                player->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
            player->EquipItem(uint16(INVENTORY_SLOT_BAG_0) << 8 | slot, item, true);
        }

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            uint64 const actual = current ? current->GetGUID().GetRawValue() : 0;
            if (actual != target.Equipment[slot].EquippedGuid)
                return SetError(error, "the applied equipment loadout does not match its class profile");
        }
        return true;
    }

    bool RestoreEquipmentFromRollback(Player* player, StoredClassProfile const& source,
        EquipmentRollbackPlan const& rollback, std::string* error)
    {
        if (!rollback.Prepared)
            return SetError(error, "the class-switch equipment rollback plan is unavailable");

        struct RollbackMove
        {
            uint8 Slot = 0;
            Item* Item = nullptr;
            uint16 Destination = 0;
        };
        std::vector<RollbackMove> moves;
        std::vector<EquipmentBagCandidate> rollbackCandidates;
        std::vector<EquipmentBagDemand> rollbackDemands;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            uint64 const sourceGuid = source.Equipment[slot].EquippedGuid;
            if (!current || current->GetGUID().GetRawValue() == sourceGuid)
                continue;
            uint16 const destination = rollback.TargetBagDestinations[slot];
            if (!destination)
                return SetError(error, "the class-switch equipment rollback plan is incomplete");
            moves.push_back({ slot, current, destination });
            rollbackCandidates.push_back({ destination, false });
            rollbackDemands.push_back({ { destination } });
        }
        std::vector<uint16> rollbackReservations;
        if (!PlanEquipmentBagReservations(
            rollbackCandidates, rollbackDemands, &rollbackReservations))
            return SetError(error, "the class-switch rollback bag destinations are not unique");

        std::map<uint64, Item*> detachedSources;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            StoredEquipmentItem const& desired = source.Equipment[slot];
            if (!desired.EquippedGuid)
                continue;
            Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (current && current->GetGUID().GetRawValue() == desired.EquippedGuid)
                continue;
            Item* item = player->GetItemByGuid(ObjectGuid(desired.EquippedGuid));
            if (!item)
                return SetError(error, "a source equipment item is missing during rollback");
            if (!item->IsEquipped())
            {
                uint64 const guid = item->GetGUID().GetRawValue();
                player->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
                detachedSources[guid] = item;
            }
        }

        for (RollbackMove const& move : moves)
        {
            if (player->GetItemByPos(
                uint8(move.Destination >> 8), uint8(move.Destination & 0xFF)))
                return SetError(error, "the class-switch equipment rollback destination is unavailable");
            player->RemoveItem(INVENTORY_SLOT_BAG_0, move.Slot, false);
            ItemPosCountVec stored{ ItemPosCount(move.Destination, 1) };
            if (player->StoreItem(stored, move.Item, true) != move.Item)
                return SetError(error, "the target equipment could not be staged for rollback");
        }

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            StoredEquipmentItem const& desired = source.Equipment[slot];
            if (!desired.EquippedGuid)
                continue;
            Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (current && current->GetGUID().GetRawValue() == desired.EquippedGuid)
                continue;
            Item* item = player->GetItemByGuid(ObjectGuid(desired.EquippedGuid));
            auto detached = detachedSources.find(desired.EquippedGuid);
            if (detached != detachedSources.end())
                item = detached->second;
            if (!item)
                return SetError(error, "a source equipment item is missing during rollback");
            if (item->GetBagSlot() != NULL_BAG || item->GetSlot() != NULL_SLOT)
                player->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
            player->EquipItem(uint16(INVENTORY_SLOT_BAG_0) << 8 | slot, item, true);
        }

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            uint64 const actual = current ? current->GetGUID().GetRawValue() : 0;
            if (actual != source.Equipment[slot].EquippedGuid)
                return SetError(error, "the restored equipment loadout does not match the source profile");
        }
        return true;
    }

    void CaptureSourcePet(Player* player, PendingSwitch& pending)
    {
        pending.SourcePetCaptured = false;
        pending.SourcePetStable = {};
        if (!player->m_petStable || !player->m_petStable->CurrentPet)
            return;

        if (Pet* pet = player->GetPet())
            pet->FillPetInfo(&player->m_petStable->CurrentPet.value());
        pending.SourcePetStable = *player->m_petStable;
        pending.SourcePetCaptured = true;
    }

    void DetachPetForStaging(Player* player)
    {
        if (Pet* pet = player->GetPet())
        {
            pet->CombatStop();
            player->SetMinion(pet, false);
            pet->AddObjectToRemoveList();
            pet->m_removed = true;
            if (pet->isControlled())
            {
                WorldPacket packet(SMSG_PET_SPELLS, 8);
                packet << uint64(0);
                player->SendDirectMessage(&packet);
            }
        }
        if (player->m_petStable)
            player->m_petStable->CurrentPet.reset();
    }

    void RestoreSourcePet(Player* player, PendingSwitch const& pending)
    {
        if (!pending.SourcePetCaptured || !player->m_petStable)
            return;
        *player->m_petStable = pending.SourcePetStable;
        if (player->m_petStable->CurrentPet && !player->GetPet())
            player->LoadPet();
    }

    void AppendSourcePetDetachment(CharacterDatabaseTransaction const& transaction,
        Player* player, PendingSwitch const& pending)
    {
        if (!transaction || !pending.SourcePetCaptured ||
            !pending.SourcePetStable.CurrentPet)
            return;

        PetStable::PetInfo const& pet = pending.SourcePetStable.CurrentPet.value();
        std::ostringstream sql;
        if (pet.Type == HUNTER_PET)
        {
            sql << "DELETE FROM character_pet WHERE owner = "
                << player->GetGUID().GetCounter() << " AND slot = "
                << uint32(PET_SAVE_AS_CURRENT) << " AND id <> " << pet.PetNumber;
            transaction->Append(sql.str().c_str());
            sql.str("");
            sql.clear();
            sql << "UPDATE character_pet SET slot = " << uint32(PET_SAVE_NOT_IN_SLOT)
                << ", savetime = UNIX_TIMESTAMP() WHERE owner = "
                << player->GetGUID().GetCounter() << " AND id = " << pet.PetNumber;
            transaction->Append(sql.str().c_str());
        }
        else
        {
            sql << "DELETE FROM character_pet WHERE id = " << pet.PetNumber;
            transaction->Append(sql.str().c_str());
            sql.str("");
            sql.clear();
            sql << "DELETE FROM character_pet_declinedname WHERE id = " << pet.PetNumber;
            transaction->Append(sql.str().c_str());
            sql.str("");
            sql.clear();
            sql << "DELETE FROM pet_aura WHERE guid = " << pet.PetNumber;
            transaction->Append(sql.str().c_str());
            sql.str("");
            sql.clear();
            sql << "DELETE FROM pet_spell WHERE guid = " << pet.PetNumber;
            transaction->Append(sql.str().c_str());
            sql.str("");
            sql.clear();
            sql << "DELETE FROM pet_spell_cooldown WHERE guid = " << pet.PetNumber;
            transaction->Append(sql.str().c_str());
        }
    }

    void FinalizeSourcePetDetachment(Player* player, PendingSwitch const& pending)
    {
        if (!pending.SourcePetCaptured || !player->m_petStable ||
            !pending.SourcePetStable.CurrentPet)
            return;

        PetStable::PetInfo pet = pending.SourcePetStable.CurrentPet.value();
        player->m_petStable->CurrentPet.reset();
        if (pet.Type == HUNTER_PET)
            player->m_petStable->UnslottedPets.push_back(std::move(pet));
    }

    bool ApplyProfileData(Player* player, StoredClassProfile const& target, std::string* error,
        bool detachPet, EquipmentRollbackPlan* rollbackPlan,
        EquipmentRollbackPlan const* restorePlan)
    {
        CharacterLanguageState const characterLanguages = CaptureCharacterLanguages(player);
        LanguageEntryIds const languageIds = CollectLanguageEntryIds(player, &target);
        StoredClassProfile runtimeProfile = target;
        MergeCharacterLanguages(
            &runtimeProfile, characterLanguages, languageIds.Spells, languageIds.Skills);

        ChrClassesEntry const* oldClass = sChrClassesStore.LookupEntry(player->GetClass());
        uint32 const oldFamily = oldClass ? oldClass->SpellClassSet : 0;
        ObjectGuid const playerGuid = player->GetGUID();
        player->RemoveAppliedAuras([oldFamily, playerGuid](AuraApplication const* application)
        {
            SpellInfo const* info = application && application->GetBase()
                ? application->GetBase()->GetSpellInfo() : nullptr;
            return info && IsSwitchOwnedAura(playerGuid.GetRawValue(),
                application->GetBase()->GetCasterGUID().GetRawValue(),
                oldFamily, info->SpellFamilyName,
                info->HasAura(SPELL_AURA_MOD_SHAPESHIFT));
        });
        if (detachPet)
            DetachPetForStaging(player);
        player->RemoveAllControlled();
        player->RemoveAllDynObjects();
        player->ClearComboPoints();
        delete player->m_runes;
        player->m_runes = nullptr;

        ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(runtimeProfile.ClassId);
        if (!classEntry)
            return SetError(error, "target class DBC entry disappeared");
        player->SetClass(runtimeProfile.ClassId);
        player->SetPowerType(Powers(classEntry->DisplayPower), false);
        player->SetLevel(runtimeProfile.Level, false);
        player->SetXP(runtimeProfile.Xp);
        player->InitRunes();
        player->InitStatsForLevel();
        player->InitGlyphsForLevel();
        if (runtimeProfile.Runes.Count)
        {
            if (!player->m_runes || !player->HasRunes())
                return SetError(error, "target class rune capability disappeared");
            player->m_runes->runeState = 0;
            for (uint8 rune = 0; rune < runtimeProfile.Runes.Count; ++rune)
            {
                player->m_runes->runes[rune].CurrentRune =
                    RuneType(runtimeProfile.Runes.Types[rune]);
                player->m_runes->runes[rune].BaseRune =
                    RuneType(runtimeProfile.Runes.BaseTypes[rune]);
                player->m_runes->runes[rune].Cooldown = runtimeProfile.Runes.Cooldowns[rune];
                player->m_runes->SetRuneState(rune,
                    runtimeProfile.Runes.Cooldowns[rune] == 0);
            }
            player->m_runes->lastUsedRune =
                RuneType(runtimeProfile.Runes.LastUsed[0]);
            player->ResyncRunes();
        }

        std::set<uint32> retainedSharedSkills;
        for (SkillProfile const& skill : runtimeProfile.Skills)
            if (skill.Shared && player->mSkillStatus.find(skill.Id) != player->mSkillStatus.end())
                retainedSharedSkills.insert(skill.Id);

        std::set<uint32> persistedSkillIds;
        for (auto& [skillId, status] : player->mSkillStatus)
        {
            if (retainedSharedSkills.count(skillId))
            {
                persistedSkillIds.insert(skillId);
                continue;
            }
            if (status.uState != SKILL_NEW)
                persistedSkillIds.insert(skillId);
            player->SetUInt32Value(SkillInfoIndex(status.pos), 0);
            player->SetUInt32Value(SkillValueIndex(status.pos), 0);
            player->SetUInt32Value(SkillBonusIndex(status.pos), 0);
            status.uState = SKILL_DELETED;
        }
        for (SkillProfile const& skill : runtimeProfile.Skills)
        {
            if (retainedSharedSkills.count(skill.Id))
                continue;
            uint8 position = PLAYER_MAX_SKILLS;
            for (uint8 slot = 0; slot < PLAYER_MAX_SKILLS; ++slot)
            {
                if (!player->GetUInt32Value(SkillInfoIndex(slot)))
                {
                    position = slot;
                    break;
                }
            }
            if (position == PLAYER_MAX_SKILLS)
                return SetError(error, "target profile has no free player skill slots");

            player->SetUInt32Value(SkillInfoIndex(position), MAKE_PAIR32(skill.Id, skill.Step));
            player->SetUInt32Value(SkillValueIndex(position), MAKE_PAIR32(skill.Value, skill.Max));
            player->SetUInt32Value(SkillBonusIndex(position),
                MAKE_PAIR32(uint16(skill.TemporaryBonus), uint16(skill.PermanentBonus)));

            SkillUpdateState const state = persistedSkillIds.count(skill.Id)
                ? SKILL_CHANGED : SKILL_NEW;
            auto status = player->mSkillStatus.find(skill.Id);
            if (status == player->mSkillStatus.end())
                player->mSkillStatus.emplace(skill.Id, SkillStatusData(position, state));
            else
            {
                status->second.pos = position;
                status->second.uState = state;
            }
        }

        PlayerSpellMap replacement;
        for (SpellProfile const& profile : runtimeProfile.Spells)
        {
            PlayerSpell spell;
            spell.state = PLAYERSPELL_NEW;
            spell.active = profile.Active;
            spell.dependent = profile.Dependent;
            spell.disabled = profile.Disabled;
            auto existing = player->m_spells.find(profile.Id);
            if (existing != player->m_spells.end())
            {
                spell.state = PLAYERSPELL_CHANGED;
                existing->second.state = PLAYERSPELL_REMOVED;
            }
            replacement[profile.Id] = spell;
        }
        for (auto& [spellId, spell] : player->m_spells)
        {
            auto replacementItr = replacement.find(spellId);
            if (replacementItr == replacement.end())
            {
                spell.state = PLAYERSPELL_REMOVED;
                replacement[spellId] = spell;
            }
            else
                replacementItr->second.state = spell.state == PLAYERSPELL_NEW
                    ? PLAYERSPELL_NEW : PLAYERSPELL_CHANGED;
        }
        player->m_spells.swap(replacement);
        if (_switchSpellId && player->m_spells.find(_switchSpellId) == player->m_spells.end())
        {
            PlayerSpell switchSpell;
            switchSpell.state = PLAYERSPELL_NEW;
            switchSpell.active = true;
            switchSpell.dependent = false;
            switchSpell.disabled = false;
            player->m_spells[_switchSpellId] = switchSpell;
        }

        for (SpellProfile const& learned : runtimeProfile.Spells)
        {
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(learned.Id);
            if (!spell || !learned.Active || learned.Disabled || !spell->IsPassive() || player->HasAura(learned.Id))
                continue;
            if (player->HandlePassiveSpellLearn(spell))
                player->AddAura(learned.Id, player);
        }

        player->m_activeSpec = runtimeProfile.ActiveSpec;
        player->m_specsCount = runtimeProfile.SpecCount;
        player->SetFreeTalentPoints(runtimeProfile.FreeTalentPoints);
        for (uint8 spec = 0; spec < TALENT_GROUPS; ++spec)
        {
            for (auto& [spellId, talent] : *player->m_talents[spec])
            {
                (void)spellId;
                delete talent;
            }
            player->m_talents[spec]->clear();
            for (uint8 glyph = 0; glyph < GLYPH_SLOTS; ++glyph)
                player->m_Glyphs[spec][glyph] = 0;
        }
        uint32 spentTalents = 0;
        for (uint8 spec = 0; spec < runtimeProfile.SpecCount; ++spec)
        {
            for (TalentProfile const& talent : runtimeProfile.Groups[spec].Talents)
            {
                PlayerTalent* stored = new PlayerTalent();
                stored->state = PLAYERSPELL_NEW;
                stored->spec = spec;
                (*player->m_talents[spec])[talent.SpellId] = stored;
                if (spec == runtimeProfile.ActiveSpec)
                {
                    for (uint32 talentId = 0; talentId < sTalentStore.GetNumRows(); ++talentId)
                    {
                        TalentEntry const* entry = sTalentStore.LookupEntry(talentId);
                        if (!entry)
                            continue;
                        bool matched = false;
                        for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
                            if (entry->SpellRank[rank] == talent.SpellId)
                            {
                                spentTalents += uint32(rank + 1);
                                matched = true;
                                break;
                            }
                        if (matched)
                            break;
                    }
                }
            }
            for (uint8 glyph = 0; glyph < GLYPH_SLOTS; ++glyph)
                player->m_Glyphs[spec][glyph] = runtimeProfile.Groups[spec].Glyphs[glyph];
        }
        player->m_usedTalentCount = spentTalents;
        for (uint8 glyph = 0; glyph < GLYPH_SLOTS; ++glyph)
            player->SetGlyph(glyph, runtimeProfile.Groups[runtimeProfile.ActiveSpec].Glyphs[glyph]);
        for (uint32 glyphId : runtimeProfile.Groups[runtimeProfile.ActiveSpec].Glyphs)
        {
            GlyphPropertiesEntry const* glyph = glyphId ? sGlyphPropertiesStore.LookupEntry(glyphId) : nullptr;
            if (glyph && glyph->SpellID && !player->HasAura(glyph->SpellID))
                player->AddAura(glyph->SpellID, player);
        }

        for (auto& [button, action] : player->m_actionButtons)
        {
            (void)button;
            action.uState = ACTIONBUTTON_DELETED;
        }
        for (uint8 button = 0; button < ACTION_SLOTS; ++button)
        {
            uint32 const packed =
                runtimeProfile.Groups[runtimeProfile.ActiveSpec].Actions[button];
            if (!packed)
                continue;
            ActionButton& action = player->m_actionButtons[button];
            uint8 const type = uint8(packed >> 24);
            action.SetActionAndType(packed & 0x00FFFFFFu, ActionButtonType(type));
        }

        player->m_WeaponProficiency = 0;
        player->m_ArmorProficiency = 0;
        for (SpellProfile const& known : runtimeProfile.Spells)
        {
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(known.Id);
            if (!spell || spell->EquippedItemSubClassMask <= 0)
                continue;
            if (spell->EquippedItemClass == ITEM_CLASS_WEAPON)
                player->m_WeaponProficiency |= uint32(spell->EquippedItemSubClassMask);
            else if (spell->EquippedItemClass == ITEM_CLASS_ARMOR)
                player->m_ArmorProficiency |= uint32(spell->EquippedItemSubClassMask);
        }
        if (restorePlan)
        {
            if (!RestoreEquipmentFromRollback(player, runtimeProfile, *restorePlan, error))
                return false;
        }
        else if (!ApplyEquipmentProfile(player, runtimeProfile, error, rollbackPlan))
            return false;
        player->UpdateAllStats();
        player->UpdateAllCritPercentages();
        player->SetHealth(std::max<uint32>(1,
            uint32(player->GetMaxHealth() * runtimeProfile.HealthPercent / 100.0f)));
        for (uint8 power = 0; power < POWER_TYPES; ++power)
            player->SetPower(Powers(power),
                uint32(player->GetMaxPower(Powers(power)) *
                    ClampPercent(runtimeProfile.PowerPercent[power]) / 100.0f));
        return ValidateStoredProfile(runtimeProfile, error);
    }

    bool ApplyProfile(Player* player, StoredClassProfile const& target, std::string* error,
        bool detachPet = false, EquipmentRollbackPlan* rollbackPlan = nullptr,
        EquipmentRollbackPlan const* restorePlan = nullptr)
    {
        ++player->m_classSwitchNotificationSuppression;
        bool result = false;
        try
        {
            result = ApplyProfileData(
                player, target, error, detachPet, rollbackPlan, restorePlan);
        }
        catch (...)
        {
            --player->m_classSwitchNotificationSuppression;
            throw;
        }
        --player->m_classSwitchNotificationSuppression;
        return result;
    }

    void NormalizeInMemoryAfterProfileWrite(Player* player)
    {
        for (auto itr = player->m_spells.begin(); itr != player->m_spells.end();)
        {
            if (itr->second.state == PLAYERSPELL_REMOVED)
                itr = player->m_spells.erase(itr);
            else
            {
                itr->second.state = PLAYERSPELL_UNCHANGED;
                ++itr;
            }
        }

        for (auto itr = player->mSkillStatus.begin(); itr != player->mSkillStatus.end();)
        {
            if (itr->second.uState == SKILL_DELETED)
                itr = player->mSkillStatus.erase(itr);
            else
            {
                itr->second.uState = SKILL_UNCHANGED;
                ++itr;
            }
        }

        for (auto itr = player->m_actionButtons.begin(); itr != player->m_actionButtons.end();)
            if (itr->second.uState == ACTIONBUTTON_DELETED)
                itr = player->m_actionButtons.erase(itr);
            else
            {
                itr->second.uState = ACTIONBUTTON_UNCHANGED;
                ++itr;
            }
        for (uint8 spec = 0; spec < TALENT_GROUPS; ++spec)
            for (auto& [spellId, talent] : *player->m_talents[spec])
            {
                (void)spellId;
                talent->state = PLAYERSPELL_UNCHANGED;
            }
    }

    uint32 ToClientAction(uint32 packed)
    {
        uint8 const type = uint8(packed >> 24);
        uint32 const action = packed & 0x00FFFFFFu;
        switch (type)
        {
            case ACTION_BUTTON_SPELL:
                return action;
            case ACTION_BUTTON_C:
                return 0x10000000u | action;
            case ACTION_BUTTON_EQSET:
                return 0x20000000u | action;
            case ACTION_BUTTON_MACRO:
            case ACTION_BUTTON_CMACRO:
                return 0x40000000u | action;
            case ACTION_BUTTON_ITEM:
                return 0x80000000u | action;
            default:
                return 0;
        }
    }

    bool BuildSnapshot(Player* player, StoredClassProfile const& profile, PlayerRuntime& runtime,
        ActiveClassSnapshot* snapshot, std::string* error)
    {
        ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(player->GetClass());
        if (!classEntry)
            return SetError(error, "active class DBC entry is missing");

        ActiveClassSnapshot built;
        built.Session = player->GetSession()->GetActiveClassSessionTag();
        built.Revision = profile.Revision;
        built.Guid = player->GetGUID().GetRawValue();
        built.Race = player->GetRace();
        built.ClassId = player->GetClass();
        built.Level = player->GetLevel();
        built.DisplayPower = uint8(classEntry->DisplayPower);
        built.ShapeshiftForm = uint8(player->GetShapeshiftForm());
        built.Xp = player->GetXP();
        built.NextLevelXp = player->GetXPForNextLevel();
        built.Resources.PowerType = uint8(player->GetPowerType());
        built.ComboTargetGuid = player->GetComboTargetGUID().GetRawValue();
        built.ComboPoints = player->GetComboPoints();
        built.Health = int32(player->GetHealth());
        built.MaxHealth = int32(player->GetMaxHealth());
        for (uint8 power = 0; power < POWER_TYPES; ++power)
        {
            built.Resources.Powers[power] = int32(player->GetPower(Powers(power)));
            built.Resources.MaxPowers[power] = int32(player->GetMaxPower(Powers(power)));
            built.Resources.PredictedPowers[power] = 0;
            built.Resources.RegenerationFlat[power] =
                player->GetFloatValue(UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER + power);
            built.Resources.RegenerationInterruptedFlat[power] =
                player->GetFloatValue(UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER + power);
        }
        for (uint8 rune = 0; rune < 4; ++rune)
            built.Resources.RuneRegeneration[rune] = player->GetFloatValue(PLAYER_RUNE_REGEN_1 + rune);
        if (player->m_runes)
        {
            built.Resources.Runes.Count = player->HasRunes() ? MAX_RUNES : 0;
            built.Resources.Runes.PredictedMask =
                built.Resources.Runes.Count ? 0xFFFFFFFFu : 0;
            for (uint8 rune = 0; rune < built.Resources.Runes.Count; ++rune)
            {
                built.Resources.Runes.Types[rune] = int32(player->m_runes->runes[rune].CurrentRune);
                built.Resources.Runes.BaseTypes[rune] = int32(player->m_runes->runes[rune].BaseRune);
                built.Resources.Runes.Cooldowns[rune] = player->m_runes->runes[rune].Cooldown;
                built.Resources.Runes.LastUsed[rune] = uint32(player->m_runes->lastUsedRune);
            }
        }

        ActiveClassExternalStats& stats = built.ExternalStats;
        for (uint8 i = 0; i < 5; ++i)
        {
            stats.Stats[i] = player->GetInt32Value(UNIT_FIELD_STAT0 + i);
            stats.PositiveStats[i] = int32(player->GetFloatValue(UNIT_FIELD_POSSTAT0 + i));
            stats.NegativeStats[i] = int32(player->GetFloatValue(UNIT_FIELD_NEGSTAT0 + i));
        }
        for (uint8 i = 0; i < 7; ++i)
        {
            stats.Resistances[i] = player->GetInt32Value(UNIT_FIELD_RESISTANCES + i);
            stats.PositiveResistances[i] = player->GetInt32Value(UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE + i);
            stats.NegativeResistances[i] = player->GetInt32Value(UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE + i);
            stats.PowerCostModifiers[i] = player->GetInt32Value(UNIT_FIELD_POWER_COST_MODIFIER + i);
            stats.PowerCostMultipliers[i] = player->GetFloatValue(UNIT_FIELD_POWER_COST_MULTIPLIER + i);
        }
        stats.AttackPower = player->GetInt32Value(UNIT_FIELD_ATTACK_POWER);
        uint32 const attackPowerMods = player->GetUInt32Value(UNIT_FIELD_ATTACK_POWER_MODS);
        stats.AttackPowerModifiers[0] = int16(PAIR32_LOPART(attackPowerMods));
        stats.AttackPowerModifiers[1] = int16(PAIR32_HIPART(attackPowerMods));
        stats.AttackPowerMultiplier = player->GetFloatValue(UNIT_FIELD_ATTACK_POWER_MULTIPLIER);
        stats.RangedAttackPower = player->GetInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER);
        uint32 const rangedAttackPowerMods = player->GetUInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER_MODS);
        stats.RangedAttackPowerModifiers[0] = int16(PAIR32_LOPART(rangedAttackPowerMods));
        stats.RangedAttackPowerModifiers[1] = int16(PAIR32_HIPART(rangedAttackPowerMods));
        stats.RangedAttackPowerMultiplier = player->GetFloatValue(UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER);
        stats.BlockPercentage = player->GetFloatValue(PLAYER_BLOCK_PERCENTAGE);
        stats.DodgePercentage = player->GetFloatValue(PLAYER_DODGE_PERCENTAGE);
        stats.ParryPercentage = player->GetFloatValue(PLAYER_PARRY_PERCENTAGE);
        stats.Expertise = player->GetUInt32Value(PLAYER_EXPERTISE);
        stats.OffhandExpertise = player->GetUInt32Value(PLAYER_OFFHAND_EXPERTISE);
        stats.CritPercentage = player->GetFloatValue(PLAYER_CRIT_PERCENTAGE);
        stats.RangedCritPercentage = player->GetFloatValue(PLAYER_RANGED_CRIT_PERCENTAGE);
        stats.OffhandCritPercentage = player->GetFloatValue(PLAYER_OFFHAND_CRIT_PERCENTAGE);
        for (uint8 i = 0; i < 7; ++i)
            stats.SpellCritPercentages[i] = player->GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + i);
        stats.ShieldBlock = player->GetUInt32Value(PLAYER_SHIELD_BLOCK);
        stats.ShieldBlockCritPercentage = player->GetFloatValue(PLAYER_SHIELD_BLOCK_CRIT_PERCENTAGE);
        for (uint8 i = 0; i < 25; ++i)
            stats.CombatRatings[i] = player->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + i);

        int32 flatModifiers[96][MAX_SPELLMOD] = {};
        int32 percentModifiers[96][MAX_SPELLMOD] = {};
        for (uint8 operation = 0; operation < MAX_SPELLMOD; ++operation)
            for (SpellModifier* modifier : player->m_spellMods[operation])
            {
                if (!modifier)
                    continue;
                for (uint8 group = 0; group < 96; ++group)
                {
                    flag96 const groupMask(
                        group < 32 ? (1u << group) : 0,
                        group >= 32 && group < 64 ? (1u << (group - 32)) : 0,
                        group >= 64 ? (1u << (group - 64)) : 0);
                    if (!(modifier->mask & groupMask))
                        continue;
                    if (modifier->type == SPELLMOD_PCT)
                        percentModifiers[group][operation] += modifier->value;
                    else
                        flatModifiers[group][operation] += modifier->value;
                }
            }
        for (uint8 group = 0; group < 96; ++group)
            for (uint8 operation = 0; operation < MAX_SPELLMOD; ++operation)
            {
                if (flatModifiers[group][operation])
                    built.Modifiers.push_back({
                        group, operation, flatModifiers[group][operation], false
                    });
                if (percentModifiers[group][operation])
                    built.Modifiers.push_back({
                        group, operation, percentModifiers[group][operation], true
                    });
            }

        std::set<int32> known;
        for (auto const& [spellId, spell] : player->m_spells)
        {
            if (spell.state == PLAYERSPELL_REMOVED || !spell.active || spell.disabled)
                continue;
            built.Spells.push_back({ int32(spellId), 0 });
            known.insert(int32(spellId));
        }
        for (uint8 button = 0; button < ACTION_SLOTS; ++button)
        {
            uint32 action = profile.Groups[profile.ActiveSpec].Actions[button];
            uint8 const type = uint8(action >> 24);
            if (type == ACTION_BUTTON_SPELL)
            {
                uint32 spellId = action & 0x00FFFFFFu;
                for (uint32 next = sSpellMgr->GetNextSpellInChain(spellId);
                    next && player->HasActiveSpell(next);
                    next = sSpellMgr->GetNextSpellInChain(next))
                    spellId = next;
                action = (action & 0xFF000000u) | spellId;
            }
            built.Actions[button] = type == ACTION_BUTTON_C ? 0 : ToClientAction(action);
        }

        for (auto const& [spellId, application] : player->GetAppliedAuras())
        {
            if (!application || !application->GetBase() ||
                application->GetBase()->GetCasterGUID() != player->GetGUID())
                continue;

            Aura* aura = application->GetBase();
            SpellInfo const* spell = aura->GetSpellInfo();
            if (!spell || spell->Id != spellId || !spell->SpellFamilyName ||
                spell->SpellFamilyName != classEntry->SpellClassSet ||
                spell->HasAura(SPELL_AURA_MOD_SHAPESHIFT))
                continue;

            ActiveClassAura saved;
            saved.CreatorGuid = aura->GetCasterGUID().GetRawValue();
            saved.SpellId = int32(spell->Id);
            saved.Flags = application->GetFlags();
            saved.Level = aura->GetCasterLevel() ? aura->GetCasterLevel() : player->GetLevel();
            saved.StackCount = std::max<uint8>(aura->GetStackAmount(), 1);
            if (!aura->IsPermanent() && aura->GetMaxDuration() > 0)
            {
                uint32 const durationMs = uint32(std::max<int32>(
                    aura->GetMaxDuration(), aura->GetDuration()));
                saved.Duration = int32((durationMs + 999u) / 1000u);
                saved.RemainingMilliseconds = uint32(std::max<int32>(aura->GetDuration(), 0));
            }
            built.ActiveClassAuras.push_back(saved);
        }

        built.ActiveTalentGroup = profile.ActiveSpec;
        built.TalentGroupCount = profile.SpecCount;
        built.CharacterPoints[0] = player->GetUInt32Value(PLAYER_CHARACTER_POINTS1);
        built.CharacterPoints[1] = player->GetUInt32Value(PLAYER_CHARACTER_POINTS2);
        for (uint8 group = 0; group < profile.SpecCount; ++group)
        {
            ActiveClassTalentGroup& out = built.TalentGroups[group];
            out.UnspentPoints = profile.FreeTalentPoints;
            out.Glyphs = profile.Groups[group].Glyphs;
            for (TalentProfile const& stored : profile.Groups[group].Talents)
            {
                for (uint32 talentId = 0; talentId < sTalentStore.GetNumRows(); ++talentId)
                {
                    TalentEntry const* talent = sTalentStore.LookupEntry(talentId);
                    if (!talent)
                        continue;
                    for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
                    {
                        if (talent->SpellRank[rank] == stored.SpellId)
                        {
                            out.Talents.push_back({ int32(talentId), rank });
                            break;
                        }
                    }
                }
            }
        }

        SystemTimePoint const now = SystemTimePoint::clock::now();
        for (auto const& [spellId, cooldown] : player->GetSpellHistory()->GetCooldowns())
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || (info->SpellFamilyName && info->SpellFamilyName != classEntry->SpellClassSet))
                continue;
            ActiveClassCooldown saved;
            saved.SpellId = int32(spellId);
            saved.ItemId = cooldown.ItemId;
            saved.Category = cooldown.CategoryId;
            saved.OnHold = cooldown.OnHold;
            saved.Shared = !info->SpellFamilyName || info->IsProfessionOrRiding();
            if (cooldown.CooldownEnd > now)
                saved.CooldownLeft = uint32(std::chrono::duration_cast<std::chrono::milliseconds>(
                    cooldown.CooldownEnd - now).count());
            if (cooldown.CategoryEnd > now)
                saved.CategoryCooldownLeft = uint32(std::chrono::duration_cast<std::chrono::milliseconds>(
                    cooldown.CategoryEnd - now).count());
            built.Cooldowns.push_back(saved);
        }
        std::set<std::tuple<int32, uint32, uint8>> sharedCooldownKeys;
        for (ActiveClassCooldown const& cooldown : built.Cooldowns)
            if (cooldown.Shared)
                sharedCooldownKeys.emplace(cooldown.SpellId, cooldown.ItemId, cooldown.Pet);
        for (ActiveClassCooldown const& earlier : runtime.SharedCooldowns)
        {
            if (sharedCooldownKeys.find({ earlier.SpellId, earlier.ItemId, earlier.Pet }) != sharedCooldownKeys.end())
                continue;
            ActiveClassCooldown retained = earlier;
            retained.CooldownLeft = 0;
            retained.CategoryCooldownLeft = 0;
            retained.OnHold = false;
            built.Cooldowns.push_back(retained);
            sharedCooldownKeys.emplace(retained.SpellId, retained.ItemId, retained.Pet);
        }
        runtime.SharedCooldowns.clear();
        for (ActiveClassCooldown const& cooldown : built.Cooldowns)
            if (cooldown.Shared)
                runtime.SharedCooldowns.push_back(cooldown);

        for (SkillProfile const& skill : profile.Skills)
            built.Skills.push_back({
                int32(skill.Id), skill.Step, skill.Value, skill.Max,
                uint16(std::max<int16>(skill.TemporaryBonus, 0)),
                uint16(std::max<int16>(skill.PermanentBonus, 0)), skill.Shared
            });
        CharacterLanguageState const characterLanguages = CaptureCharacterLanguages(player);
        for (SkillProfile const& language : characterLanguages.Skills)
        {
            bool const present = std::any_of(built.Skills.begin(), built.Skills.end(),
                [&language](ActiveClassSkill const& skill) {
                    return skill.Id == int32(language.Id);
                });
            if (present)
                continue;
            built.Skills.push_back({
                int32(language.Id), language.Step, language.Value, language.Max,
                uint16(std::max<int16>(language.TemporaryBonus, 0)),
                uint16(std::max<int16>(language.PermanentBonus, 0)), true
            });
        }
        built.Proficiencies[ITEM_CLASS_WEAPON] = player->m_WeaponProficiency;
        built.Proficiencies[ITEM_CLASS_ARMOR] = player->m_ArmorProficiency;

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                ActiveClassEquipmentItem& equipment = built.Equipment[slot];
                equipment.ItemId = int32(item->GetEntry());
                equipment.Enchantments[0] = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
                equipment.Enchantments[1] = item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
                equipment.EquippedGuid = item->GetGUID().GetRawValue();
            }
        }

        for (uint32 spellId : known)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (info && (!info->SpellFamilyName || info->IsProfessionOrRiding()))
                built.SharedProgression.SpellIds.push_back(spellId);
        }
        std::sort(built.SharedProgression.SpellIds.begin(), built.SharedProgression.SpellIds.end());

        if (!ValidateActiveClassSnapshot(built, error))
            return false;
        *snapshot = std::move(built);
        return true;
    }

    void SendState(Player* player, StatusStage stage, RequestResult result, PlayerRuntime const& runtime,
        bool forceInvalid = false, uint64 revisionOverride = 0, uint8 activeClassOverride = 0)
    {
        uint64 revision = revisionOverride ? revisionOverride : 1;
        std::map<uint8, std::pair<uint8, uint64>> levels;
        bool invalidActiveProfile = false;
        if (QueryResult profileRows = CharacterDatabase.PQuery(
            "SELECT class_id, level, revision FROM ffxiv_class_switch_profiles WHERE guid = {}",
            player->GetGUID().GetCounter()))
        {
            do
            {
                Field* fields = profileRows->Fetch();
                uint8 classId = fields[0].GetUInt8();
                uint8 level = fields[1].GetUInt8();
                uint64 rowRevision = fields[2].GetUInt64();
                if (!IsSupportedClassId(classId) || !level || level > 80 || !rowRevision)
                {
                    if (classId == player->GetClass())
                        invalidActiveProfile = true;
                    continue;
                }
                levels[classId] = { level, rowRevision };
            }
            while (profileRows->NextRow());
        }
        auto activeRow = levels.find(player->GetClass());
        if (!revisionOverride && activeRow != levels.end())
            revision = activeRow->second.second;
        if (invalidActiveProfile && !forceInvalid)
        {
            SendState(player, StatusStage::Rejected, RequestResult::StorageFailure, runtime,
                true, revisionOverride, activeClassOverride);
            return;
        }
        uint8 const activeClassId = activeClassOverride ? activeClassOverride : player->GetClass();
        uint8 const pendingClassId = runtime.Pending.Phase == TransitionPhase::RecoveringSource
            ? runtime.Pending.ExpectedClass : runtime.Pending.TargetClass;
        std::vector<ActiveClassStatusDescriptor> available;
        available.reserve(_definitions.size());
        for (auto const& [classId, definition] : _definitions)
        {
            uint8 flags = 0;
            if (classId == activeClassId)
                flags |= CLASS_FLAG_ACTIVE;
            if (classId == pendingClassId)
                flags |= CLASS_FLAG_PENDING;
            if (definition.Definition.Enabled && (definition.Definition.RaceMask & ClassIdToMask(player->GetRace())))
                flags |= CLASS_FLAG_AVAILABLE;
            if (!(flags & (CLASS_FLAG_AVAILABLE | CLASS_FLAG_ACTIVE | CLASS_FLAG_PENDING)))
                continue;

            uint16 level = ResolveClassStatusLevel(
                classId, false, 0, definition.Definition.InitialLevel,
                uint8(sWorld->getIntConfig(CONFIG_START_PLAYER_LEVEL)),
                uint8(sWorld->getIntConfig(CONFIG_START_DEATH_KNIGHT_PLAYER_LEVEL)));
            auto profileLevel = levels.find(classId);
            if (profileLevel != levels.end() && profileLevel->second.first)
                level = profileLevel->second.first;
            else if (classId == player->GetClass())
                level = player->GetLevel();
            available.push_back({ classId, uint8(level), flags, definition.Definition.Name });
        }

        std::vector<uint8> payload = EncodeActiveClassStatus(stage, result, activeClassId,
            pendingClassId, revision,
            player->GetSession()->GetActiveClassSessionTag(), available);
        if (payload.empty())
        {
            TC_LOG_ERROR("entities.player.classswitch",
                "Failed to encode class-switch status for player {}", player->GetGUID().ToString());
            return;
        }
        SendCustomPacket(player, SERVER_STATUS_OPCODE, payload);
    }

    bool _definitionsLoaded = false;
    bool _definitionsLoading = false;
    uint32 _switchSpellId = 0;
    std::map<uint8, RuntimeClassDefinition> _definitions;
    std::unordered_map<ObjectGuid::LowType, PlayerRuntime> _players;
};

ClassSwitchManager& Manager()
{
    return ClassSwitchManager::Instance();
}

RequestResult RequestSwitch(Player* player, uint16 protocolVersion, uint8 requestType, uint8 classId,
    uint64 revision, std::string* error)
{
    return Manager().Request(player, protocolVersion, requestType, classId, revision, error);
}

bool ProcessClientResponse(Player* player, uint8 const* payload, size_t size, std::string* error)
{
    return Manager().HandleClientResponse(player, payload, size, error);
}

bool HandleSwitchSpellCast(Player* player, uint32 spellId, std::string* error)
{
    return Manager().HandleSpell(player, spellId, error);
}

void UpdatePlayer(Player* player)
{
    Manager().Update(player);
}

bool IsTransitionPending(Player* player)
{
    return Manager().IsPending(player);
}

bool CancelPendingTransitionOnLogout(Player* player)
{
    return Manager().CancelOnLogout(player);
}

void RestoreActiveProfileAfterLoad(Player* player)
{
    Manager().RestoreAfterLoad(player);
}

bool AppendStoredProfileToTransaction(CharacterDatabaseTransaction const& transaction, Player* player,
    StoredClassProfile const& profile, std::string* error)
{
    if (!transaction || !player)
    {
        if (error)
            *error = "class profile transaction or player is missing";
        return false;
    }

    std::vector<uint8> encoded = EncodeStoredProfile(profile);
    if (encoded.empty())
    {
        if (!ValidateStoredProfile(profile, error) && error)
            *error = "stored class profile could not be encoded: " + *error;
        else if (error)
            *error = "stored class profile could not be encoded";
        return false;
    }

    std::string hex = ByteArrayToHexStr(encoded);
    std::ostringstream query;
    query <<
        "REPLACE INTO ffxiv_class_switch_profiles "
        "(guid, class_id, revision, level, xp, profile, updated_at) VALUES ("
        << player->GetGUID().GetCounter() << ", "
        << uint32(profile.ClassId) << ", "
        << profile.Revision << ", "
        << uint32(profile.Level) << ", "
        << profile.Xp << ", UNHEX('" << hex << "'), NOW())";
    transaction->Append(query.str().c_str());
    return true;
}

bool AppendActiveProfileToTransaction(CharacterDatabaseTransaction const& transaction, Player* player,
    std::string* error)
{
    return Manager().AppendActiveProfile(transaction, player, error);
}

void ClearPlayerRuntime(Player* player)
{
    Manager().Clear(player);
}

uint32 GetSwitchSpellId(Player*)
{
    return Manager().GetSwitchSpellId();
}

bool ValidateClassSwitchRegistry(std::string* error)
{
    return Manager().ValidateRegistry(error);
}
}

uint8 Player::RequestClassSwitch(uint16 protocolVersion, uint8 requestType, uint8 classId,
    uint64 revision, std::string* error)
{
    if (!GetSession() || !GetSession()->GetActiveClassSessionTag())
    {
        if (error)
            *error = "authenticated session is unavailable";
        return uint8(ClassSwitch::RequestResult::NotReady);
    }
    return uint8(ClassSwitch::RequestSwitch(this, protocolVersion, requestType, classId, revision, error));
}

bool Player::HandleClassSwitchSpellCast(uint32 spellId, std::string* error)
{
    std::string localError;
    bool const result = ClassSwitch::HandleSwitchSpellCast(
        this, spellId, error ? error : &localError);
    if (!result && !error && !localError.empty())
        TC_LOG_ERROR("entities.player.classswitch",
            "Player {} class-switch spell failed: {}",
            GetGUID().ToString(), localError);
    return result;
}

bool Player::ProcessClassSwitchClientResponse(uint8 const* payload, size_t size,
    std::string* error)
{
    return ClassSwitch::ProcessClientResponse(this, payload, size, error);
}

bool Player::IsClassSwitchTransitionPending() const
{
    return m_classSwitchTransitionPending.load(std::memory_order_acquire);
}

bool Player::IsClassSwitchSpell(uint32 spellId) const
{
    return spellId && spellId == ClassSwitch::GetSwitchSpellId(const_cast<Player*>(this));
}

bool Player::EnsureClassSwitchSpellKnown()
{
    return ClassSwitch::ClassSwitchManager::Instance().EnsureKnown(this);
}

uint32 Player::GetClassSwitchSpellId() const
{
    return ClassSwitch::GetSwitchSpellId(const_cast<Player*>(this));
}

void Player::RestoreActiveClassProfileAfterLoad()
{
    ClassSwitch::RestoreActiveProfileAfterLoad(this);
}
