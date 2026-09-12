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
 * @file lib/platform/include/instr/platform/factory.h
 * @brief Selects the platform implementation at startup.
 *
 * REQ §5 requires a simulated implementation "complete enough to run the
 * entire suite on a build host with no hardware". That is only true if the
 * daemons themselves can be pointed at it — otherwise only unit tests can
 * reach it, and the simulated device of §12 cannot exist.
 *
 * Selection is `--platform simulated` or INSTR_PLATFORM=simulated. This is
 * not a platform conditional in daemon code (R9): the daemons still see one
 * IPlatform interface and contain no target-specific branches.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <memory>
#include <string>

#include "instr/platform/native.h"
#include "instr/platform/platform.h"
#include "instr/platform/simulated.h"

namespace instr
{
/// @brief Platform abstraction: board and hardware access behind IPlatform (REQ §5).
namespace platform
{

/// @brief Construct the platform implementation selected by @p name.
///
/// @p name is "native" (default) or "simulated". An unrecognised name falls
/// back to native and is reported via @p recognised — a typo must not
/// silently put a real device onto a simulated platform.
///
/// @param name Platform selector ("native" or "simulated").
/// @param sysroot Root used by the native implementation for file access.
/// @param recognised Optional out-flag; false when @p name was unrecognised.
/// @return The platform implementation.
inline std::unique_ptr<IPlatform> make_platform(const std::string& name, const std::string& sysroot, bool* recognised = nullptr)
{
    if (recognised) *recognised = true;
    if (name == "simulated") return std::make_unique<SimulatedPlatform>();
    if (name != "native" && recognised) *recognised = false;
    return std::make_unique<NativePlatform>(sysroot);
}

} // namespace platform
} // namespace instr
