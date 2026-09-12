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
 * @file tools/hardware_ctrl/main.cpp
 * @brief full-coverage control tool for hardware (REQ §3.2)
 *
 * The daemon's command table is the single source of truth: compiled into
 * this tool for --help, per-op help and shell completion, while every
 * dispatch travels to the daemon over its raw-UDS ctrl socket (mode 0600,
 * root only). Handlers are never invoked locally — the nullptr core is a
 * dispatch-remote marker enforced by tool_main.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/hardware/daemon.h"
#include "instr/ctrl/ctrl.h"

/// @brief _ctrl tool entry: builds the daemon's command table and runs the shared ctrl main
/// (argument parsing, --help, --json, --socket, shell completion, dispatch).
int main(int argc, char** argv)
{
    auto table = instr::hardware::build_hardware_table(nullptr);
    return instr::ctrl::tool_main(argc, argv, table, INSTR_CTRL_PROG, INSTR_CTRL_SOCKET);
}
