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

#ifndef TRINITYCORE_FFXIV_STAT_ENGINE_H
#define TRINITYCORE_FFXIV_STAT_ENGINE_H

#include "FFXIVStatTypes.h"
#include <string>

/*
 * Pure FFXIV derived-stat calculator (slice 1).
 *
 * ComputeFfxivStats() reads nothing but its argument and the generated tables:
 * no Player, no DB, no Unit field, no aura. Same inputs -> same snapshot, which
 * is what the Catch2 golden vectors pin.
 *
 * Formula order (I-REFRESH-20261123 §3.2, B-FORMULA-REFERENCE F1-F12):
 *   1. base[i]  = trunc(levelMain * jobPct(i) / 100) + tribeBonus(i)        F2
 *   2. param[1..6] = base[]; secondaries = the level base S when unset       U4
 *   3. F9 crit, F10 direct hit, F8 determination, F11 tenacity, F12 speed
 *   4. F3 HP, F4 fixed 10 000 MP + model MP tick
 *
 * Rounding: F9/F10/F8/F11 are published with the model's fl() (floor), F12 is
 * published with native signed truncation toward zero. Both are reproduced
 * exactly on integers (the model's floating-point epsilon guard cannot trigger
 * for integer numerators over the shipped divisors, so floor == floorDiv).
 */

namespace FFXIV::Stats
{
// Retail's standard base GCD in milliseconds. F12's B is the action's own base
// time; the snapshot reports the standard one so a value can be compared.
constexpr uint32 FFXIV_STANDARD_GCD_MS = 2500;
constexpr uint32 FFXIV_FIXED_MAX_MP = 10000;
// F12/B §F12.1: the native recast stage clamps T2 to 0x5DC (1500 ms) *after* the
// haste percentage (0x1408DCAA0). The GCD is a recast, so it inherits that floor;
// the cast path has no floor (0x1408DD990 contains neither 0x5DC nor 500).
constexpr uint32 FFXIV_MIN_GCD_MS = 1500;

// Aborts (via the table accessors) on a job/level/tribe id outside the shipped
// tables. Gameplay callers cap the level at FFXIV_MAX_LEVEL (100) before calling.
FfxivDerivedStats ComputeFfxivStats(FfxivStatInputs const& inputs);

// Fixed-width, locale-independent block used by `.ffxivstats` and the test
// fixtures. Ends with a newline.
std::string FormatFfxivStatsBlock(FfxivDerivedStats const& stats);
} // namespace FFXIV::Stats

#endif // TRINITYCORE_FFXIV_STAT_ENGINE_H
