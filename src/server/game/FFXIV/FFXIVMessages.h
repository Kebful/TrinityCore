#pragma once
// FFXIV Haven custom sub-opcodes, C++ mirror of the TS enum.
//
// Single source: release/modules/ffxivhaven/shared/Messages.ts (`FfxivSubOperations`). C++ cannot
// import TS, so this file is the one hand-mirrored copy per source tree (this core, and the client's
// WoW/Source/Duskhaven/FFXIV/FFXIVMessages.h). tools/protocol-drift.test.mts parses both mirrors and
// the TS enum and fails on a missing, renamed or renumbered entry.
//
// Append-only: a member's value is its wire id. Never reorder, remove or renumber an entry.

#include <cstdint>

namespace FFXIV::Protocol
{
enum class SubOpcode : std::uint16_t
{
    STATS_SNAPSHOT = 0x7A60,  // 0x7A60 stats snapshot (StatsSnapshotPayload), schema 2
    ACTION_POTENCY = 0x7A61,  // 0x7A61 action potency list (ActionPotencyPayload), schema 2
    JOB_GAUGE = 0x7A62,       // 0x7A62 job gauge (JobGaugePayload), schema 2
    // 0x7A63 is not used by this module (a retired combo-highlight channel); do not reuse it.
    ACTION_CHARGES = 0x7A64,  // 0x7A64 action charges (ActionChargesPayload), schema 1
};
} // namespace FFXIV::Protocol
