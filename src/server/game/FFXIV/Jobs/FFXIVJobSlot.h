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

#ifndef TRINITYCORE_FFXIV_JOB_SLOT_H
#define TRINITYCORE_FFXIV_JOB_SLOT_H

#include "Define.h"

class Player;

namespace FFXIV::Jobs
{
// The ffxiv_job_slot table records each later job's class and class-restricted soul crystal.
void LoadFfxivJobSlotTable();
uint8 GetFfxivJobStoneClassId(uint32 itemEntry);

inline bool IsFfxivJobStoneMappingValid(uint8 jobId, uint8 classJobId,
    uint64 classMask, uint64 itemClassMask)
{
    return jobId != 0 && jobId == classJobId && classMask != 0 && classMask == itemClassMask;
}

// The class determines the job and its gauge for the entire equipment profile.
uint8 GetFfxivPlayerJobId(Player const& player);
}

#endif
