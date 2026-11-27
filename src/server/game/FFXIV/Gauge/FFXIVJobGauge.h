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

#ifndef TRINITYCORE_FFXIV_JOB_GAUGE_H
#define TRINITYCORE_FFXIV_JOB_GAUGE_H

#include "Define.h"
#include "FFXIV/FFXIVMessages.h"

class Player;
class SpellInfo;
class Unit;

/*
 * FFXIV job gauges: the engine rules only.
 *
 * The gauge VALUE, its per-action deltas, the display mirror and the 0x7A62 HUD packet belong to
 * the job's livescript (release/modules/ffxivhaven/livescripts/job-resources/jobgauge.ts and the ten
 * class scripts plus systems/hosted-jobs). The core keeps exactly two things a script cannot own:
 *
 * 1. Rage is not an FFXIV resource (the suppression below). For a player of an FFXIV module class,
 *    WoW's own rage sources are suppressed -- auto-attack damage dealt, damage taken, absorbed
 *    damage, out-of-combat decay, the client's predicted regen fields, and every spell energize
 *    (Unit::RewardRage, Player::Regenerate, Player::UpdatePowerRegen, Unit::EnergizeBySpell,
 *    Spell::EffectEnergize/EffectEnergizePct). The action's Spell.dbc rage cost is also not charged
 *    or refused for such a player: the gauge, owned by the script, is the only authority, and for a
 *    class whose display power is not rage it is pinned to 0. The script writes POWER_RAGE as a
 *    one-way display of slot 0 (UNIT_FIELD_MAXPOWER 1000 = 100 gauge, the client divides by 10) and
 *    sets the PLAYER_FIELD_BYTES2 ignore-prediction bit; nothing reads rage back.
 *
 * 2. Spell.OnFfxivActionResolved, the once-per-successful-cast event every gauge/charge script uses.
 *    It fires from Spell::_handle_finish_phase, after every target of the cast went through the
 *    action damage hook, with the retail Action id and whether the cast was the correct combo step
 *    (the two facts a script cannot reconstruct from Spell.OnAfterCast). It applies no data.
 *
 * Wire -- 0x7A62, schema 2 (the livescript builds and sends it; this is the shared specification)
 * -----------------------------------------------------------------------------------------------
 *   jobId u8, gaugeId u8, kind u8, slotCount u8,
 *   slotCount x { value u16, max u16, style u8 },
 *   nameLength u8, name[nameLength], schema u8 (= 2)
 * Little-endian, packed: FFXIV_JOB_GAUGE_FIXED_BYTES + FFXIV_JOB_GAUGE_SLOT_BYTES * slotCount +
 * nameLength bytes. kind 0 / slotCount 0 / gaugeId 0 / no name means "this job has no gauge" (the
 * client clears its readout). Sent on every change, at enter-world and when the job or level
 * changes. The client still decodes schema 1 (jobId, gaugeId, value u16, max u16, nameLength, name,
 * 1) as a one-slot bar; the server sends 2. Client counterpart:
 * repos/clientdecomp/WoW/Source/Duskhaven/FFXIV/Gauge/FFXIVJobGauge.h
 *
 * The gauge row itself (ffxiv_job_gauge: jobId, gaugeId, name, max, kind, slots, per-slot
 * max/style/cost) and the action rows (ffxiv_action_gauge: actionId, jobId, gaugeId, slot, minLevel,
 * op, delta, comboOnly) stay in the world database and are read by the script.
 */

namespace FFXIV::Gauge
{
constexpr uint16 FFXIV_JOB_GAUGE_OPCODE = uint16(FFXIV::Protocol::SubOpcode::JOB_GAUGE);
constexpr uint8 FFXIV_JOB_GAUGE_SCHEMA = 2;
constexpr std::size_t FFXIV_JOB_GAUGE_FIXED_BYTES = 6; // every field but the slots and the name
constexpr std::size_t FFXIV_JOB_GAUGE_SLOT_BYTES = 5;  // value u16, max u16, style u8
constexpr std::size_t FFXIV_JOB_GAUGE_MAX_SLOTS = 3;
constexpr std::size_t FFXIV_JOB_GAUGE_MAX_NAME = 32;   // the table's VARCHAR(32)

// The Beast Gauge row's `max` is the module's 100 (INFERRED from the retail HUD). WoW stores rage in
// tenths -- UNIT_FIELD_MAXPOWER 1000 and the client divides by 10 -- so the script's mirror writes
// gauge * FFXIV_RAGE_TENTHS and Spell.dbc rage costs are the gauge cost * the same factor.
constexpr uint16 FFXIV_RAGE_MAX = 100;
constexpr uint16 FFXIV_RAGE_TENTHS = 10;
constexpr uint16 FFXIV_RAGE_MAX_FIELD = FFXIV_RAGE_MAX * FFXIV_RAGE_TENTHS; // 1000

// How the slots of one gauge relate (the wire's `kind`). Same numbers on the client and the script.
enum class FfxivGaugeKind : uint8
{
    None    = 0, // no gauge: no slots
    Counter = 1, // one Number slot (a count read as a number)
    Bar     = 2, // one Bar slot (Beast Gauge 0..100)
    Stacked = 3, // one Pips slot (Cartridges 0..3)
    Multi   = 4, // 2..3 independent slots, none of them a state (Chakra + Beast Chakra + Nadi)
    State   = 5, // 1..3 slots, at least one a state (Signed or Enum) (Black Mage element, Bard song)
};

// How one slot's value is read (the wire's per-slot `style`). Every style keeps value <= max.
enum class FfxivGaugeStyle : uint8
{
    None   = 0,
    Bar    = 1, // 0..max fill                                                 max 1..10000
    Pips   = 2, // `value` of `max` stacks lit                                 max 1..10
    Number = 3, // a count shown as a number                                   max 1..10000
    Signed = 4, // centred state: stance = value - max/2, max even             max 2..20
                //   (Black Mage element: max 6, value 0..2 Umbral Ice III..I, 3 none, 4..6 Astral Fire I..III)
    Flags  = 5, // bitmask; max is every flag set (2^n - 1)                    max 1..255
    Enum   = 6, // state id, 0 = none, 1..max a named state                    max 1..15
};

// What one ffxiv_action_gauge row does to its slot -- the law the script applies.
enum class FfxivGaugeOp : uint8
{
    Add     = 0, // slot := clamp(slot + value)
    Set     = 1, // slot := value                                        (any slot)
    AtLeast = 2, // slot := max(slot, value)   ("Grants Astral Fire")     (state slots)
    AtMost  = 3, // slot := min(slot, value)   ("Grants Umbral Ice")      (state slots)
    Swap    = 4, // a Signed slot's stance := the other side's single stack (Transpose), value 0
};

// --- rage: the engine suppression -------------------------------------------------------------

// True when the unit is a player of an FFXIV module class (the class has a ClassJob row). False for
// a stock class and for every creature; the suppression below is this predicate.
bool IsFfxivRageOwner(Unit const& unit);
bool SuppressesWowRage(Unit const& unit);

// True when Spell::CheckPower / Spell::TakePower must not charge the action's WoW rage cost: the
// gauge lives in the script and rage is only its display (or pinned to 0 for a non-rage class). A
// status that makes an action free is an engine spell mod (SpellInfo::CalcPowerCost already returns
// 0), so it needs no branch here.
bool IsFfxivActionRageCostWaived(Player const& player);

// --- the hook ---------------------------------------------------------------------------------

// Spell::_handle_finish_phase, once per successful cast. Not an FFXIV action: returns. Otherwise
// fires Spell.OnFfxivActionResolved (player, spellInfo, actionId, comboCorrect); the scripts apply
// their gauge rows, charge pools and procs. No data is read here.
void OnFfxivActionResolved(Player& caster, SpellInfo const* spellInfo);
} // namespace FFXIV::Gauge

#endif // TRINITYCORE_FFXIV_JOB_GAUGE_H
