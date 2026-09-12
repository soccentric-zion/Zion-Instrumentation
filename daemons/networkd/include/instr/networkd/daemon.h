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
 * @file daemons/networkd/include/instr/networkd/daemon.h
 * @brief networkd daemon facade: core state + command table + spoke channel
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "instr/common/command_table.h"
#include "instr/common/config_sink.h"
#include "instr/common/periodic.h"
#include "instr/networkd/domain.h"
#include "instr/platform/platform.h"
#include "instr/transport/transport.h"
#include "networkd.grpc.pb.h"

namespace instr
{
/// @brief networkd: network plan application, tunnel guard and remote access (§7.2).
namespace networkd
{
using namespace domain;
using namespace instr::transport;

/// @brief networkd domain core: plan state, config, guard, remote access and events.
class NetworkdCore
{
  public:
    /// @brief Bind the core to the platform and a clock.
    /// @param plat Platform apply hooks.
    /// @param ts Clock.
    NetworkdCore(platform::IPlatform& plat, TimeSource& ts) : plat_(plat), ts_(ts)
    {
    }

    /// @brief Runtime (non-persistent) plan state — persistent config lives in /pst via istate.
    ///
    /// _ctrl changes are runtime-only (§3.2). Applying an istate delivery
    /// rebuilds the plan — interfaces, wireless, DNS, routes, firewall,
    /// forwards — and puts it through the tunnel-loss guard, so a bad push
    /// cannot strand the device.
    struct Config
    {
        bool enabled = true;                        ///< Master switch.
        double tunnel_guard_timeout_s = 30.0;       ///< Revert grace period.
        bool remote_ssh_default_enabled = false;    ///< Remote access off by default.
        uint32_t remote_ssh_max_ttl_s = 8 * 3600;   ///< Window cap.
        double connectivity_probe_interval_s = 10.0;   ///< Probe cadence.
        double ssh_sweep_interval_s = 30.0;         ///< Window expiry sweep cadence.
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
    /// @brief Build a plan from a delivered document (also used by tests).
    /// @param doc Parsed, schema-validated document.
    /// @return The rendered plan, or an error.
    static Result<NetworkPlan> plan_from_config(const ConfigDocument& doc);

    /// @brief Periodic work (§7.2): expire SSH windows. Returns events to emit upward.
    /// @return Events to emit upward.
    std::vector<std::pair<std::string, std::string>> sweep_remote_access();
    /// @brief Periodic work (§7.2): probe reachability and re-rank interfaces.
    /// @return Events to emit upward.
    std::vector<std::pair<std::string, std::string>> probe_connectivity();

    /// @brief Load a plan as the current running state (without applying).
    /// @param plan Plan to make current without applying.
    /// @return ok(), or an error when the plan is rejected.
    Error load_plan(const NetworkPlan& plan);
    /// @brief Current running plan.
    /// @return The current running plan.
    const NetworkPlan& plan() const
    {
        return plan_;
    }
    /// @brief Mutable plan access (runtime _ctrl edits).
    /// @return Mutable plan access (runtime _ctrl edits).
    NetworkPlan& mutable_plan()
    {
        return plan_;
    }

    /// @brief Apply the current plan through the platform (recorded/dry-run on host).
    /// @return ok(), or the platform error.
    Error apply();
    /// @brief Tunnel-loss-guarded apply of a new plan (mandatory §7.2).
    /// @param next Candidate plan.
    /// @param timeout_s Revert grace period in seconds.
    /// @return What happened (applied, reverted, detail).
    TunnelGuard::Result guarded_apply(const NetworkPlan& next, double timeout_s);
    /// @brief Simulated reachability hook (tests); production uses tunnel handshake age.
    /// @param f Reachability hook (tests).
    void set_reachability_fn(std::function<bool()> f)
    {
        reachability_ = std::move(f);
    }
    /// @brief Test seam: override the guard clock (monotonic seconds by default).
    /// @param f Guard clock hook (tests).
    void set_clock_fn(std::function<double()> f)
    {
        clock_fn_ = std::move(f);
    }

    /// @brief Remote-access manager access.
    /// @return Remote-access manager access.
    RemoteAccessManager& ssh()
    {
        return ssh_;
    }
    /// @brief Script tunnel state (tests).
    /// @param up Tunnel state to script.
    void set_tunnel_up(bool up)
    {
        tunnel_up_ = up;
    }
    /// @brief Current tunnel state.
    bool tunnel_up() const
    {
        return tunnel_up_;
    }

    /// @brief Apply the time policy passed down from istate; renders daemon configs.
    /// @param policy Time policy from istate.
    /// @return ok(), or the render/apply error.
    Error apply_time_policy(const TimePolicy& policy);
    /// @brief Rendered time-daemon configuration.
    /// @return The rendered time-daemon configuration.
    const TimeDaemonConfig& time_daemons() const
    {
        return time_daemons_;
    }

    /// @brief Render effective state as JSON (ctrl "status").
    /// @return The effective state as JSON.
    std::string status_json() const;

    /// @brief Wall seconds (best-effort when the clock is unlocked: monotonic fallback).
    int64_t wall_now_s() const
    {
        int64_t w = ts_.wall_ns() / 1'000'000'000ll;
        return w > 1'500'000'000ll ? w : int64_t(1'700'000'000ll);
    }

    /// @brief Events emitted upward to istate (kind, detail).
    /// @param f Event sink (kind, detail).
    void set_event_sink(std::function<void(const std::string& kind, const std::string& detail)> f)
    {
        event_sink_ = std::move(f);
    }

    /// @brief Platform access.
    /// @return Platform access.
    platform::IPlatform& platform()
    {
        return plat_;
    }

  private:
    platform::IPlatform& plat_;        ///< Platform apply hooks (not owned).
    TimeSource& ts_;                    ///< Clock (not owned).
    Config cfg_;                        ///< Current configuration.
    std::function<void(const Config&)> on_config_;   ///< Config-accepted listener.
    NetworkPlan plan_;                  ///< Current running plan.
    RemoteAccessManager ssh_;           ///< Remote-access windows.
    TimeDaemonConfig time_daemons_;     ///< Rendered time-daemon configs.
    bool tunnel_up_ = false;            ///< Tracked tunnel state.
    std::function<bool()> reachability_;      ///< Reachability hook (tests).
    std::function<double()> clock_fn_;        ///< Guard clock hook (tests).
    std::function<void(const std::string&, const std::string&)> event_sink_;   ///< Event sink.
    std::vector<std::string> last_rank_;       ///< Last computed interface ranking.
};

/// @brief Build the networkd ctrl command table (plan, wifi, ssh, status).
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_networkd_table(NetworkdCore* core);

/// @brief Spoke runner (same shape for all spokes; typed to networkd frames).
class NetworkdSpoke
{
  public:
    /// @brief Wire the channel, config sink and handlers together.
    /// @param core Domain core.
    /// @param log Logger.
    /// @param table Command table.
    /// @param hub_target istate gRPC target.
    /// @param build_version Build version to advertise.
    NetworkdSpoke(NetworkdCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version);
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
    /// @brief Queue a network event to istate.
    /// @param ev Network event to queue.
    void send_event(const networkd::NetworkEvent& ev);

  private:
    NetworkdCore* core_;                      ///< Domain core (not owned).
    Logger* log_;                             ///< Logger (not owned).
    const CommandTable* table_;               ///< Command table (not owned).
    std::unique_ptr<ConfigSink> config_sink_;      ///< Validates and applies config deliveries.
    std::unique_ptr<SpokeChannel<networkd::NetworkHub, networkd::NetworkSpokeFrame, networkd::NetworkHubFrame>> channel_;   ///< gRPC channel to istate.
};

} // namespace networkd
} // namespace instr