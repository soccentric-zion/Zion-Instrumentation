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
 * @file daemons/monitord/include/instr/monitord/transport.h
 * @brief domain+state facade wired into the command table. The core owns
 *
 * the sampler, the high-res window, and the schedule; everything reachable
 * from _ctrl is reachable here, and everything here is dispatchable from
 * istate commands too (one table, two surfaces, §3.2).
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <atomic>
#include <functional>
#include <mutex>

#include "instr/common/config_sink.h"
#include "instr/monitord/domain.h"
#include "instr/platform/descriptors.h"
#include "instr/transport/transport.h"

namespace instr
{
/// @brief monitord: passive resource sampling and wear projection (§7.3).
namespace monitord
{

using namespace instr::monitord::domain;

/// @brief monitord domain+state facade: sampler, rolling window, config and emission.
class MonitorCore
{
  public:
    /// @brief Bind the core to the platform and a clock.
    /// @param plat Platform readings.
    /// @param ts Clock.
    MonitorCore(platform::IPlatform& plat, TimeSource& ts) : plat_(plat), sampler_(plat, ts), window_(60.0, 1.0), ts_(ts)
    {
    }

    /// @brief Apply a resource profile (intervals, budgets) from the board descriptor.
    /// @param profile Resource profile from the board descriptor.
    void configure(const platform::ResourceProfile& profile);

    /// @brief Configuration delivered by istate (§7.3, §9).
    ///
    /// monitord observes only — the thresholds here are reported, never turned
    /// into alarms (R5).
    struct Config
    {
        bool enabled = true;                     ///< Master switch.
        double interval = 5.0;                   ///< Default sampling interval.
        std::map<std::string, double> intervals_per_class;   ///< Per-class interval overrides.
        double thermal_warn_c = 75.0;            ///< Reported thermal warn threshold.
        double thermal_critical_c = 85.0;        ///< Reported thermal critical threshold.
        double storage_warn_pct = 90.0;          ///< Reported storage-full threshold.
        uint32_t wear_projection_threshold_days = 180;   ///< Remaining-life warning threshold.
        double highres_window_s = 60.0;          ///< Local high-res window length.
        double highres_interval_s = 1.0;         ///< Local high-res sampling interval.
    };
    /// @brief Current configuration.
    /// @return Current configuration.
    const Config& config() const
    {
        return cfg_;
    }
    /// @brief Apply an istate-delivered config document.
    /// @param doc Parsed, schema-validated document.
    /// @return ok(), or an error describing why the values were refused.
    Error apply_config(const ConfigDocument& doc);
    /// @brief Install a callback fired when a new config is accepted.
    /// @param f Config-accepted listener.
    void set_config_listener(std::function<void(const Config&)> f)
    {
        on_config_ = std::move(f);
    }
    /// @brief One sampling pass: samples → window + emit callback (spoke enqueue).
    /// @return Samples produced by this pass.
    std::vector<MonitorSample> sample_once();
    /// @brief Storage wear projections for all devices.
    /// @param age_days Device age so far in days.
    /// @return Wear projections for all devices.
    std::vector<WearProjector::Projection> wear_projections(uint64_t age_days) const;

    /// @brief High-resolution rolling window (local retention).
    /// @return The high-resolution rolling window.
    const RollingWindow& window() const
    {
        return window_;
    }
    /// @brief Sampler access.
    /// @return Sampler access.
    Sampler& sampler()
    {
        return sampler_;
    }
    /// @brief Render the current status as JSON (ctrl "status").
    /// @return The current status as JSON.
    std::string status_json() const;

    /// @brief Install the sample emission callback (spoke enqueue).
    /// @param f Sample emission callback (spoke enqueue).
    void set_emit(std::function<void(const MonitorSample&)> f)
    {
        std::lock_guard<std::mutex> lk(mu_);
        emit_ = std::move(f);
    }

  private:
    platform::IPlatform& plat_;      ///< Platform readings (not owned).
    Sampler sampler_;                ///< Per-tick sampler.
    RollingWindow window_;           ///< High-res local window.
    TimeSource& ts_;                  ///< Clock (not owned).
    std::function<void(const MonitorSample&)> emit_;   ///< Sample sink (guarded by mu_).
    std::mutex mu_;                  ///< Guards emit_.
    Config cfg_;                     ///< Current configuration.
    std::function<void(const Config&)> on_config_;   ///< Config-accepted listener.
};

} // namespace monitord
} // namespace instr