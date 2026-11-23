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

/* ScriptData
Name: ffxivdamage_commandscript
%Complete: 100
Comment: Print the read-only FFXIV combat pipeline for one action (nothing applied)
Category: commandscripts
EndScriptData */

/*
 * `.ffxivdamage <actionId> [targetLevel]`
 *
 * Read-only diagnostic: runs FFXIV::Combat's published pipeline for the selected
 * player's own stats and one action's potency, and prints every stage. It does
 * not deal damage, does not select a target, writes no Unit field and sends no
 * packet beyond the chat lines.
 *
 * Everything that is not a stat is pinned so the output is reproducible:
 *   - rolls: crit off, direct hit off, variance 100 (FfxivFixedRollSource(99,100));
 *     the crit and direct hit *chances* are printed next to it;
 *   - weapon damage: 0, because there is no FFXIV gear model yet (slice 1 leaves
 *     every equipment parameter at zero), so f(WD) is its level term alone;
 *   - party bonus: the real one, counted from the player's group by unique role.
 *
 * The action potency comes from FFXIV::Combat::FFXIVActionPotency (the sibling
 * data slice). If that API moves, only this file needs updating.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Group.h"
#include "FFXIV/Combat/FFXIVActionPotency.h"
#include "FFXIV/Combat/FFXIVCombatMath.h"
#include "FFXIV/Stats/FFXIVStatEngine.h"
#include "FFXIV/Stats/FFXIVStatTables.h"
#include "Player.h"
#include "RBAC.h"
#include <cstdio>
#include <string>

using namespace Trinity::ChatCommands;

namespace
{
char const* StageName(FFXIV::Combat::FfxivCombatStageId id)
{
    using Id = FFXIV::Combat::FfxivCombatStageId;
    switch (id)
    {
        case Id::AttackPowerPercent: return "f(ATK)";
        case Id::DeterminationPermille: return "f(DET)";
        case Id::BaseDamage: return "stage1";
        case Id::TenacityPermille: return "f(TNC)";
        case Id::WeaponDamagePercent: return "f(WD)";
        case Id::TraitPercent: return "trait";
        case Id::Stage2Damage: return "stage2";
        case Id::CriticalRoll: return "crit_roll";
        case Id::DirectHitRoll: return "dh_roll";
        case Id::CriticalMultiplierPermille: return "crit_mult";
        case Id::DirectHitMultiplierPercent: return "dh_mult";
        case Id::Stage3Damage: return "stage3";
        case Id::VariancePercent: return "variance";
        case Id::LevelCorrectionPermille: return "level_corr";
        case Id::BuffPercent: return "buff";
        case Id::SpeedPermille: return "f(SPD)";
        case Id::DotPlusOne: return "dot+1";
        case Id::HealingMagicPotencyPercent: return "f(HMP)";
        case Id::AutoAttackPotency: return "auto_potency";
        case Id::FinalDamage: return "final";
    }
    return "?";
}

std::string FormatTrace(FFXIV::Combat::FfxivCombatTrace const& trace)
{
    std::string out;
    char buffer[64];
    for (uint8 i = 0; i < trace.count; ++i)
    {
        std::snprintf(buffer, sizeof(buffer), "%s %s=%lld", out.empty() ? "" : "  ",
                      StageName(trace.stages[i].id), static_cast<long long>(trace.stages[i].value));
        out += buffer;
    }
    return out;
}

// Unique roles in the player's group, mapped through the ClassJob role table.
// A solo player counts as one role (their own), matching "unique roles present".
FFXIV::Combat::FfxivPartyComposition PartyFromGroup(Player* player)
{
    FFXIV::Combat::FfxivPartyComposition party;
    auto addRole = [&party](Player* member)
    {
        if (!member || !member->IsInWorld())
            return;
        uint8 const classJobId = FFXIV::Stats::GetFfxivClassJobId(member->GetClass());
        FFXIV::Combat::FfxivPartyRole role;
        if (FFXIV::Combat::GetFfxivPartyRole(classJobId, role))
            party = party.With(role);
    };

    addRole(player);
    if (Group* group = player->GetGroup())
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
            addRole(reference->GetSource());
    return party;
}
} // namespace

// Read-only diagnostics. Reuses an existing GM permission on purpose: a new
// RBAC_PERM_COMMAND_* id would need a rbac_permissions row, i.e. a database
// change, and this slice is schema-free.
class ffxivdamage_commandscript : public CommandScript
{
public:
    ffxivdamage_commandscript() : CommandScript("ffxivdamage_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "ffxivdamage", HandleFFXIVDamageCommand, rbac::RBAC_PERM_COMMAND_SERVER_DEBUG, Console::Yes },
        };
        return commandTable;
    }

    static bool HandleFFXIVDamageCommand(ChatHandler* handler, uint32 actionId, Optional<uint8> targetLevel)
    {
        Player* target = handler->getSelectedPlayerOrSelf();
        if (!target)
        {
            handler->SendSysMessage("No player selected and no player attached to this session.");
            return false;
        }

        if (!FFXIV::Combat::HasFfxivActionPotency(actionId))
        {
            handler->PSendSysMessage("Action {} is not in the FFXIV potency table (no potency is invented "
                                     "for it).",
                                     actionId);
            return false;
        }

        FFXIV::Stats::FfxivDerivedStats const& stats = target->GetFFXIVStats();
        FFXIV::Combat::FfxivActionPotency const potency =
            FFXIV::Combat::GetFfxivActionPotency(actionId, stats.jobId, stats.level);
        if (potency.damage == 0 && potency.cure == 0)
        {
            handler->PSendSysMessage("Action {} has no resolved potency for job {} level {} (sheet branch "
                                     "unmatched; kind {}).",
                                     actionId, uint32(stats.jobId), uint32(stats.level), uint32(potency.kind));
            return false;
        }

        uint8 const effectiveTargetLevel = targetLevel ? *targetLevel : stats.level;
        FFXIV::Combat::FfxivPartyComposition const party = PartyFromGroup(target);
        FFXIV::Combat::FfxivTraitState const traits;
        // Deterministic: 99 never crits or direct-hits, variance 100 is the mean.
        FFXIV::Combat::FfxivFixedRollSource rolls(99, 100);

        // No FFXIV gear model yet: f(WD) is its level term alone.
        int32 const weaponDamage = 0;
        int32 const weaponPercent = FFXIV::Combat::ComputeFfxivWeaponDamagePercent(stats, weaponDamage);

        handler->PSendSysMessage("FFXIV action {} for {} (job {} {}, level {}), target level {}, party "
                                 "roles {} (+{}%)",
                                 actionId, target->GetName(), uint32(stats.jobId),
                                 FFXIV::Stats::GetFfxivJobRow(stats.jobId).abbreviation,
                                 uint32(stats.level), uint32(effectiveTargetLevel),
                                 uint32(party.UniqueRoleCount()), uint32(party.UniqueRoleCount()));

        if (potency.cure > 0)
        {
            FFXIV::Combat::FfxivHealResult const result = FFXIV::Combat::ComputeFfxivHeal(
                stats, potency.cure, weaponPercent, effectiveTargetLevel, party, traits, rolls);
            handler->PSendSysMessage("  cure potency {} -> {} hp", uint32(potency.cure),
                                     static_cast<long long>(result.value));
            handler->SendSysMessage(FormatTrace(result.trace));
            handler->PSendSysMessage("  crit chance {}.{:02}% (pinned off)", result.critChanceBp / 100,
                                     result.critChanceBp % 100);
            handler->PSendSysMessage("  pinned: weapon damage {}, variance 100", weaponDamage);
            // Slice 1 leaves BaseParam 34 at zero, so f(HMP) here is its level
            // term alone -- print the why next to the number.
            if (stats.param[FFXIV::Stats::FFXIV_PARAM_HEALING_MAGIC_POTENCY] == 0)
                handler->SendSysMessage("  note: BaseParam 34 (healing magic potency) is not populated by "
                                        "slice 1, so f(HMP) has only its level term");
            return true;
        }

        FFXIV::Combat::FfxivDirectDamageResult const result = FFXIV::Combat::ComputeFfxivDirectDamage(
            stats, potency.damage, weaponPercent, effectiveTargetLevel, party, traits, rolls);
        handler->PSendSysMessage("  damage potency {} -> {}", uint32(potency.damage),
                                 static_cast<long long>(result.value));
        handler->SendSysMessage(FormatTrace(result.trace));
        handler->PSendSysMessage("  crit chance {}.{:02}%  direct hit chance {}.{:02}% (both pinned off)",
                                 result.critChanceBp / 100, result.critChanceBp % 100,
                                 result.dhChanceBp / 100, result.dhChanceBp % 100);
        handler->PSendSysMessage("  pinned: weapon damage {}, variance 100", weaponDamage);
        return true;
    }
};

void AddSC_ffxivdamage_commandscript()
{
    new ffxivdamage_commandscript();
}
