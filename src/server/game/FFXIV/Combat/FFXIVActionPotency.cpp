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

#include "FFXIVActionPotency.h"

#include <algorithm>
#include <iterator>

/*
 * The table is generated data, not code: FFXIVActionPotency.inc holds the If nodes the client
 * evaluates and one row per action id, and this file is the evaluator. The generator
 * (release/modules/ffxivhaven/tools/generate-ffxiv-action-potency.py --cpp-out) writes it from the
 * shipped sheet and asserts that a preorder walk of the nodes reproduces the module table's branch
 * rows one for one, so the two never drift.
 */

namespace FFXIV::Combat
{
namespace
{
// A row's `damage`/`cure` are 0 when the sheet states no plain value; 0 is safe because the smallest
// potency in the table is 20.
constexpr uint16 FFXIV_POTENCY_NO_ROOT = 0xFFFF;

enum FfxivPotencyParam : uint8
{
    FFXIV_POTENCY_PARAM_JOB   = 0, // gnum68
    FFXIV_POTENCY_PARAM_LEVEL = 1, // gnum72
};

enum FfxivPotencyOp : uint8
{
    FFXIV_POTENCY_OP_EQ = 0,
    FFXIV_POTENCY_OP_NE = 1,
    FFXIV_POTENCY_OP_GE = 2,
    FFXIV_POTENCY_OP_GT = 3,
    FFXIV_POTENCY_OP_LE = 4,
    FFXIV_POTENCY_OP_LT = 5,
};

enum FfxivPotencyArmKind : uint8
{
    FFXIV_POTENCY_ARM_NONE  = 0, // this branch of the sheet states no number
    FFXIV_POTENCY_ARM_VALUE = 1, // the number in `value`
    FFXIV_POTENCY_ARM_NODE  = 2, // node `value` is the branch
};

struct FfxivPotencyArm
{
    uint8 kind;
    uint16 value;
};

struct FfxivPotencyNode
{
    uint8 param;
    uint8 op;
    uint16 value;
    FfxivPotencyArm thenArm;
    FfxivPotencyArm elseArm;
};

struct FfxivPotencyRow
{
    uint32 actionId;
    uint16 damage;
    uint16 cure;
    uint8 kind;
    uint16 ownRoot;
    uint16 cureRoot;
};

#include "FFXIVActionPotency.inc"

// One test on one parameter, evaluated exactly as written: the sheet's own comparison, applied once.
// A test the sheet repeats is a second node, so nothing here short-circuits it.
bool TestHolds(FfxivPotencyNode const& node, uint8 classJobId, uint8 level)
{
    uint16 actual;
    switch (node.param)
    {
        case FFXIV_POTENCY_PARAM_JOB:
            actual = classJobId;
            break;
        case FFXIV_POTENCY_PARAM_LEVEL:
            actual = level;
            break;
        default:
            return false;
    }

    switch (node.op)
    {
        case FFXIV_POTENCY_OP_EQ: return actual == node.value;
        case FFXIV_POTENCY_OP_NE: return actual != node.value;
        case FFXIV_POTENCY_OP_GE: return actual >= node.value;
        case FFXIV_POTENCY_OP_GT: return actual > node.value;
        case FFXIV_POTENCY_OP_LE: return actual <= node.value;
        case FFXIV_POTENCY_OP_LT: return actual < node.value;
        default: return false;
    }
}

uint16 EvaluateArm(FfxivPotencyArm const& arm, uint8 classJobId, uint8 level);

uint16 EvaluateNode(uint16 node, uint8 classJobId, uint8 level)
{
    FfxivPotencyNode const& current = FFXIV_ACTION_POTENCY_NODES[node];
    FfxivPotencyArm const& arm = TestHolds(current, classJobId, level)
        ? current.thenArm
        : current.elseArm;
    return EvaluateArm(arm, classJobId, level);
}

uint16 EvaluateArm(FfxivPotencyArm const& arm, uint8 classJobId, uint8 level)
{
    switch (arm.kind)
    {
        case FFXIV_POTENCY_ARM_VALUE:
            return arm.value;
        case FFXIV_POTENCY_ARM_NODE:
            return EvaluateNode(arm.value, classJobId, level);
        default:
            return 0;
    }
}

uint16 ResolveRoot(uint16 root, uint8 classJobId, uint8 level)
{
    return root == FFXIV_POTENCY_NO_ROOT ? 0 : EvaluateNode(root, classJobId, level);
}

FfxivPotencyRow const* FindRow(uint32 actionId)
{
    FfxivPotencyRow const* first = std::begin(FFXIV_ACTION_POTENCY_ROWS);
    FfxivPotencyRow const* last = std::end(FFXIV_ACTION_POTENCY_ROWS);
    FfxivPotencyRow const* row = std::lower_bound(first, last, actionId,
        [](FfxivPotencyRow const& candidate, uint32 id) { return candidate.actionId < id; });
    return (row != last && row->actionId == actionId) ? row : nullptr;
}
} // namespace

bool HasFfxivActionPotency(uint32 actionId)
{
    return FindRow(actionId) != nullptr;
}

FfxivActionPotency GetFfxivActionPotency(uint32 actionId, uint8 classJobId, uint8 level)
{
    FfxivActionPotency result{ 0, 0, FFXIV_POTENCY_KIND_NONE };

    FfxivPotencyRow const* row = FindRow(actionId);
    if (!row)
        return result;

    result.kind = row->kind;
    result.damage = row->damage != 0 ? row->damage
                                     : ResolveRoot(row->ownRoot, classJobId, level);
    result.cure = row->cure != 0 ? row->cure
                                 : ResolveRoot(row->cureRoot, classJobId, level);
    return result;
}
} // namespace FFXIV::Combat
