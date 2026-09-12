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
 * @file daemons/hardware/include/instr/hardware/daemon.h
 * @brief hardware daemon facade: core + command table + spoke
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <memory>
#include <mutex>

#include "instr/common/command_table.h"
#include "instr/common/config_sink.h"
#include "instr/common/periodic.h"
#include "instr/hardware/domain.h"
#include "instr/platform/platform.h"
#include "instr/transport/transport.h"
#include "hardware.grpc.pb.h"

namespace instr
{
/// @brief Hardware daemon: device controls and quick health checks (§7.4, R7).
namespace hardware
{
using namespace domain;
using namespace instr::transport;

/// @brief Hardware domain core: device manager, config and check-result routing.
class HardwareCore
{
  public:
    /// @brief Bind the core to the platform, a clock and the board descriptor.
    /// @param plat Platform primitives.
    /// @param ts Clock.
    /// @param desc Board device inventory.
    HardwareCore(platform::IPlatform& plat, TimeSource& ts, const platform::DeviceDescriptor& desc) : mgr_(plat, ts, desc)
    {
    }

    /// @brief Configuration delivered by istate (§7.4, §9).
    struct Config
    {
        bool enabled = true; ///< Master switch.
        double periodic_check_interval_s = 300.0; ///< 0 disables scheduled checks.
        double check_timeout_s = 5.0; ///< Per-check timeout.
    };
    /// @brief Current configuration (thread-safe).
    Config config() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return cfg_;
    }
    /// @brief Apply an istate-delivered config document (rejects invalid values).
    /// @param doc Parsed, schema-validated document.
    /// @return ok(), or an error describing why the values were refused.
    Error apply_config(const ConfigDocument& doc)
    {
        Config c;
        c.enabled = doc.get_bool("enabled", true);
        c.periodic_check_interval_s = doc.get_double("periodic_check_interval_s", 300.0);
        c.check_timeout_s = doc.get_double("check_timeout_s", 5.0);
        if (c.periodic_check_interval_s < 0) return Error::make(Errc::InvalidArgument, "periodic_check_interval_s must be >= 0", "hardware config");
        std::function<void(const Config&)> listener;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cfg_ = c;
            listener = on_config_;
        }
        if (listener) listener(c);
        return Error::none();
    }
    /// @brief Install a callback fired when a new config is accepted.
    /// @param f Config-accepted listener.
    void set_config_listener(std::function<void(const Config&)> f)
    {
        std::lock_guard<std::mutex> lk(mu_);
        on_config_ = std::move(f);
    }

    /// @brief Device manager access.
    /// @return Device manager access.
    DeviceManager& manager()
    {
        return mgr_;
    }
    /// @brief Install the check-result sink (spoke emission).
    /// @param f Check-result sink (spoke emission).
    void set_result_sink(std::function<void(const CheckResult&)> f)
    {
        std::lock_guard<std::mutex> lk(mu_);
        sink_ = std::move(f);
    }
    /// @brief Emit one check result to the installed sink.
    /// @param r Check result to emit.
    void emit(const CheckResult& r)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (sink_) sink_(r);
    }
    /// @brief Render the board's device inventory as JSON (ctrl "devices").
    /// @return The board's device inventory as JSON.
    std::string descriptor_summary_json() const;

  private:
    DeviceManager mgr_; ///< Device controls and checks.
    std::function<void(const CheckResult&)> sink_; ///< Check-result sink (guarded by mu_).
    mutable std::mutex mu_; ///< Guards cfg_, on_config_ and sink_.
    Config cfg_; ///< Current configuration.
    std::function<void(const Config&)> on_config_; ///< Config-accepted listener.
};

/// @brief Build the hardware ctrl command table (devices, control, checks).
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_hardware_table(HardwareCore* core);

/// @brief Spoke: transport to istate plus the schema-validated config sink.
class HardwareSpoke
{
  public:
    /// @brief Wire the channel, config sink and handlers together.
    /// @param core Domain core.
    /// @param log Logger.
    /// @param table Command table.
    /// @param hub_target istate gRPC target.
    /// @param build_version Build version to advertise.
    HardwareSpoke(HardwareCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version);
    /// @brief Start the channel.
    void start();
    /// @brief Stop the channel.
    void stop();
    /// @brief True while registered with istate.
    bool registered() const
    {
        return channel_->registered();
    }
    /// @brief Install the schema every config delivery must satisfy (R4, §9).
    /// @param schema Schema every config delivery must satisfy.
    void set_schema(JValue schema);
    /// @brief Revision of the last config applied.
    uint64_t config_revision() const
    {
        return channel_->config_revision();
    }
    /// @brief Queue a check result to istate.
    /// @param r Check result to queue.
    void send_result(const CheckResult& r);

  private:
    HardwareCore* core_; ///< Domain core (not owned).
    Logger* log_; ///< Logger (not owned).
    const CommandTable* table_; ///< Command table (not owned).
    std::unique_ptr<ConfigSink> config_sink_; ///< Validates and applies config deliveries.
    std::unique_ptr<SpokeChannel<hardware::HardwareHub, hardware::HardwareSpokeFrame, hardware::HardwareHubFrame>> channel_; ///< gRPC channel to istate.
};

} // namespace hardware
} // namespace instr