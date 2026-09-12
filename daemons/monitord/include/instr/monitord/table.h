// ============================================================================
//  Arches Instrumentation
//
//  Copyright (C) 2026 Soccentric. All rights reserved.
//  Author: Sandesh Ghimire (sandesh@soccentric)
//
//  This file is part of a proprietary product of Soccentric.
//
//  NO WARRANTY:
//  This software is provided "AS IS", without warranty of any kind, either
//  express or implied, including but not limited to the warranties of
//  merchantability, fitness for a particular purpose, and non-infringement.
//  In no event shall Soccentric be liable for any claim, damages, or other
//  liability arising from the use of this software.
//
//  This software may not be used, copied, modified, distributed, or
//  disclosed, in whole or in part, without the express prior written
//  consent of Soccentric.
// ============================================================================
/**
 * @file daemons/monitord/include/instr/monitord/table.h
 * @brief single source of truth for the gRPC surface and
 *
 * the _ctrl tool (REQ §3.2).
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include "instr/common/command_table.h"

namespace instr
{
/// @brief monitord: passive resource sampling and wear projection (§7.3).
namespace monitord
{

class MonitorCore; ///< domain+state facade, defined in transport.h

/// @brief Build the monitord ctrl command table (status, samples, wear).
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_monitor_table(MonitorCore* core);

} // namespace monitord
} // namespace instr