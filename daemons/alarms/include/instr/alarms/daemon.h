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
 * @file daemons/alarms/include/instr/alarms/daemon.h
 * @brief alarms daemon facade: core + command table + spoke
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <memory>

#include "instr/alarms/domain.h"
#include "instr/common/command_table.h"
#include "instr/common/config_sink.h"
#include "instr/common/periodic.h"
#include "instr/transport/transport.h"
#include "alarms.grpc.pb.h"

namespace instr
{
/// @brief Alarms daemon: X.733 alarm state machine, escalation and catalog (§7.6).
namespace alarms
{
using namespace domain;
using namespace instr::transport;

/// @brief Alarms domain core: manager access, config and spoke event routing.
class AlarmsCore
{
  public:
    /// @brief Bind the core to storage and a clock.
    /// @param fs Storage backend.
    /// @param db_path Active-alarm database path.
    /// @param ts Clock.
    AlarmsCore(IFileSystem& fs, const std::string& db_path, TimeSource& ts) : mgr_(fs, db_path, ts), fs_(fs), ts_(ts)
    {
    }

    /// @brief Configuration delivered by istate (§7.6, §9).
    struct Config
    {
        bool enabled = true; ///< Master switch.
        uint32_t escalation_default_promote_after_s = 3600; ///< Default escalation threshold.
        double escalation_sweep_interval_s = 30.0; ///< Sweep cadence.
        bool snapshot_on_critical = true; ///< Snapshot-on-fault switch.
    };
    /// @brief Current configuration.
    /// @return Current configuration.
    const Config& config() const
    {
        return cfg_;
    }
    /// @brief Apply an istate-delivered config document (rejects invalid values loudly).
    /// @param doc Parsed, schema-validated document.
    /// @return ok(), or an error describing why the values were refused.
    Error apply_config(const ConfigDocument& doc)
    {
        Config c;
        c.enabled = doc.get_bool("enabled", true);
        c.escalation_default_promote_after_s = uint32_t(doc.get_int("escalation_default_promote_after_s", 3600));
        c.escalation_sweep_interval_s = doc.get_double("escalation_sweep_interval_s", 30.0);
        c.snapshot_on_critical = doc.get_bool("snapshot_on_critical", true);
        if (c.escalation_sweep_interval_s <= 0) return Error::make(Errc::InvalidArgument, "escalation_sweep_interval_s must be > 0", "alarms config");
        cfg_ = c;
        mgr_.set_snapshot_on_critical(c.snapshot_on_critical);
        if (on_config_) on_config_(cfg_);
        return Error::none();
    }
    /// @brief Install a callback fired when a new config is accepted.
    /// @param f Config-accepted listener.
    void set_config_listener(std::function<void(const Config&)> f)
    {
        on_config_ = std::move(f);
    }

    /// @brief Alarm manager access.
    /// @return Alarm manager access.
    AlarmManager& manager()
    {
        return mgr_;
    }
    /// @brief Current monotonic time.
    uint64_t now_mono() const
    {
        return ts_.mono_ns();
    }
    /// @brief Route manager events to the spoke.
    /// @param f Spoke event routing sink.
    void set_spoke_sink(std::function<void(const AlarmStateEvent&)> f)
    {
        spoke_sink_ = std::move(f);
        mgr_.set_event_sink([this](const AlarmStateEvent& ev) {
            if (spoke_sink_) spoke_sink_(ev);
        });
    }

  private:
    AlarmManager mgr_; ///< State machine and persistence.
    IFileSystem& fs_; ///< Storage backend.
    TimeSource& ts_; ///< Clock.
    std::function<void(const AlarmStateEvent&)> spoke_sink_; ///< Spoke event routing.
    Config cfg_; ///< Current configuration.
    std::function<void(const Config&)> on_config_; ///< Config-accepted listener.
};

/// @brief Build the alarms ctrl command table (raise/clear/ack/list/catalog).
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_alarms_table(AlarmsCore* core);

/// @brief Spoke: transport to istate plus the schema-validated config sink.
class AlarmsSpoke
{
  public:
    /// @brief Wire the channel, config sink and handlers together.
    /// @param core Domain core.
    /// @param log Logger.
    /// @param table Command table.
    /// @param hub_target istate gRPC target.
    /// @param build_version Build version to advertise.
    AlarmsSpoke(AlarmsCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version);
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
    /// @brief Queue an alarm state event to istate.
    /// @param ev Alarm state event to queue.
    void send_event(const AlarmStateEvent& ev);

  private:
    AlarmsCore* core_; ///< Domain core (not owned).
    Logger* log_; ///< Logger (not owned).
    const CommandTable* table_; ///< Command table (not owned).
    std::unique_ptr<ConfigSink> config_sink_; ///< Validates and applies config deliveries.
    std::unique_ptr<SpokeChannel<alarms::AlarmHub, alarms::AlarmSpokeFrame, alarms::AlarmHubFrame>> channel_; ///< gRPC channel to istate.
};

} // namespace alarms
} // namespace instr