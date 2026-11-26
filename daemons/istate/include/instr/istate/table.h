// ============================================================================
//  Arches Instrumentation
//
//  Copyright (C) 2026 SoCcentric
//  Author: Sandesh Ghimire (sandesh@soccentric)
//
//  Licensed under the MIT License; see the LICENSE file in the repository root.
//
//  NO WARRANTY:
//  This software is provided "AS IS", without warranty of any kind, either
//  express or implied, including but not limited to the warranties of
//  merchantability, fitness for a particular purpose, and non-infringement.
//  In no event shall Soccentric be liable for any claim, damages, or other
//  liability arising from the use of this software.
// ============================================================================
/**
 * @file daemons/istate/include/instr/istate/table.h
 * @brief full coverage of the hub's operations
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 SoCcentric. Licensed under the MIT License.
 */

#pragma once
#include "instr/common/command_table.h"
#include "instr/istate/core.h"

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{
/// @brief Build the istate ctrl command table (config, spokes, spool, audit, resets).
/// @param core istate core the table reads from.
/// @return The command table.
CommandTable build_istate_table(IstateCore* core);
} // namespace istate
} // namespace instr
