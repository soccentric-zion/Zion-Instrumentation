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
 * @file lib/platform/include/instr/platform/descriptors.h
 * @brief declarative descriptors (REQ §5.1)
 *
 * Board-specific content is data, never code:
 * devices.yaml        — board devices + their checks and controls
 * resource profile    — sampling intervals, retention, budgets by class
 * capability manifest — which features exist, so istate reports honestly
 * Adding a board means adding descriptors, not editing daemons.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <map>
#include <string>
#include <vector>

#include "instr/common/error.h"
#include "instr/common/json.h"

namespace instr
{
/// @brief Platform abstraction: board and hardware access behind IPlatform (REQ §5).
namespace platform
{

/// @brief One hardware self-check entry for a device.
struct DeviceCheck
{
    std::string name; ///< Check name, e.g. "loopback".
    std::string op; ///< Operation, e.g. "gpio_toggle_read".
    std::map<std::string, std::string> params; ///< Check parameters.
};

/// @brief One physical device's spec from the board descriptor.
struct DeviceSpec
{
    std::string name; ///< "status-led", "gpio-1", "can0", "usb1", "display".
    std::string kind; ///< gpio / led / can / usb / display / sensor / watchdog.
    std::vector<std::string> controls; ///< Control op names this device accepts.
    std::vector<DeviceCheck> checks; ///< Health checks for this device.
    std::map<std::string, std::string> params; ///< Device parameters, e.g. line=1, port=1-2.
};

/// @brief A board's device inventory: variant name plus every device spec.
struct DeviceDescriptor
{
    std::string board; ///< Board variant name.
    std::vector<DeviceSpec> devices; ///< Devices on this board.
};

/// @brief Per-platform-class sampling and budget profile (§6 sizing).
struct ResourceProfile
{
    std::string platform_class; ///< small / medium / large.
    double sample_interval_default_s = 5; ///< Default telemetry sample interval.
    std::map<std::string, double> sample_intervals_per_class_s; ///< cpu/memory/storage/thermal/load intervals.
    double highres_window_s = 60; ///< Fine-grained local retention window.
    double highres_interval_s = 1; ///< Fine-grained sampling interval.
    double telemetry_interval_s = 30; ///< Northbound telemetry push interval.
    uint64_t telemetry_batch_max_records = 200; ///< Max records per northbound batch.
    uint64_t spool_max_bytes = 64ull * 1024 * 1024; ///< Spool budget.
    uint64_t logs_max_bytes = 128ull * 1024 * 1024; ///< Log-retention budget.
    uint64_t ota_staging_bytes = 512ull * 1024 * 1024; ///< OTA staging budget.
    uint64_t backup_max_bytes = 256ull * 1024 * 1024; ///< Backup budget.
    uint64_t timeline_max_events = 5000; ///< Timeline retention cap.
};

/// @brief A board's capability manifest (what this variant supports).
struct CapabilityManifest
{
    std::string board; ///< Board variant name.
    std::vector<std::string> capabilities; ///< wifi, wifi_ap, can, display, gnss, ptp, watchdog, ...
    /// @brief Test membership of the manifest.
    /// @param c Capability name to test.
    /// @return True when the board declares it.
    bool has(const std::string& c) const
    {
        for (const auto& x : capabilities)
            if (x == c) return true;
        return false;
    }
};

/// @brief Load a device descriptor YAML, validated against @p schema.
/// @param yaml_text Descriptor YAML source.
/// @param schema JSON Schema to validate against.
/// @param source Origin of the text, used in errors.
/// @return The parsed descriptor, or an error.
Result<DeviceDescriptor> load_device_descriptor(const std::string& yaml_text, const JValue& schema, const std::string& source);
/// @brief Load a resource profile YAML, validated against @p schema.
/// @param yaml_text Profile YAML source.
/// @param schema JSON Schema to validate against.
/// @param source Origin of the text, used in errors.
/// @return The parsed profile, or an error.
Result<ResourceProfile> load_resource_profile(const std::string& yaml_text, const JValue& schema, const std::string& source);
/// @brief Load a capability manifest YAML, validated against @p schema.
/// @param yaml_text Manifest YAML source.
/// @param schema JSON Schema to validate against.
/// @param source Origin of the text, used in errors.
/// @return The parsed manifest, or an error.
Result<CapabilityManifest> load_capability_manifest(const std::string& yaml_text, const JValue& schema, const std::string& source);

} // namespace platform
} // namespace instr