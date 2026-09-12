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
 * @file lib/transport/src/transport.cpp
 * @brief translation unit for the header-based shared plumbing
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/transport/transport.h"

namespace instr
{
/// @brief gRPC transport between the istate hub and daemon spokes (§3.1).
namespace transport
{

// Suite-wide proto schema version lives here (single point of truth for the
// registration handshake contract, REQ §3.1).
// kProtoSchemaVersion is defined in the header as constexpr.

} // namespace transport
} // namespace instr
