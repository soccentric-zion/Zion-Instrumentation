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
 * @file daemons/istate/include/instr/istate/orchestrator.h
 * @brief orchestration, not supervision (REQ §7.1). Declares readiness
 *
 * order and dependency edges, requests start/stop through systemd, tracks
 * each spoke's operational state, exposes it northbound. systemd owns
 * restart policy; if istate restarts, spokes keep running and re-attach.
 * Liveness-checks each spoke so a hung (not crashed) daemon is detected.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "instr/common/backoff.h"
#include "instr/common/timestamps.h"
#include "instr/platform/platform.h"

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

/// @brief Lifecycle state of a managed spoke daemon.
enum class SpokeState
{
    Expected,    ///< Declared but not started.
    Starting,    ///< Start requested.
    Ready,       ///< Registered and responsive.
    Degraded,    ///< Registered but failing liveness.
    Stopped,     ///< Stopped or exited.
    Hung         ///< Running but not answering pings.
};

/// @brief Canonical name of a spoke state.
/// @param s State to name.
/// @return Canonical lowercase name.
const char* spoke_state_name(SpokeState s);

/// @brief Bookkeeping for one managed spoke.
struct SpokeInfo
{
    std::string name;               ///< Daemon name.
    std::string unit;               ///< systemd unit.
    SpokeState state = SpokeState::Expected;   ///< Lifecycle state.
    bool live = false;              ///< Liveness probe result.
    std::string build_version;      ///< Reported build.
    uint32_t proto_version = 0;     ///< Reported proto schema version.
    std::vector<std::string> capabilities;   ///< Reported capabilities.
    uint64_t last_seen_mono = 0;    ///< Last traffic of any kind.
    // Liveness ping/pong (§3.1, §7.1). last_seen_mono tracks any traffic, which
    // an idle-but-healthy spoke (backup, ota) does not produce — only the pong
    // distinguishes "quiet" from "hung".
    uint64_t last_ping_mono = 0;    ///< Last ping sent.
    uint64_t last_pong_mono = 0;    ///< Last pong received.
    uint64_t ping_nonce = 0;        ///< Outstanding ping nonce.
    uint64_t pongs_received = 0;    ///< Lifetime pong count.
    uint64_t pings_sent = 0;        ///< Lifetime ping count.
};

/// @brief Startup order (§7.1): istate → Faults, monitord, Hardware → Alarms,
/// LogManager, Telemetry → networkd → OTA, Backup → tunnel and HQ session.
///
/// The HQ session is last and never a precondition for local operation.
/// Observation daemons start early so boot-window anomalies are captured.
/// @return The declared startup order.
const std::vector<std::string>& startup_order();

/// @brief Dependency edges: daemon → set it must wait for.
/// @return The dependency edges.
const std::map<std::string, std::vector<std::string>>& dependency_edges();

/// @brief Spoke lifecycle orchestrator: ordering, registration, liveness, watchdog.
class Orchestrator
{
  public:
    /// @brief Bind the orchestrator to the platform and a clock.
    /// @param plat systemd operations.
    /// @param ts Clock.
    Orchestrator(platform::IPlatform& plat, TimeSource& ts) : plat_(plat), ts_(ts)
    {
    }

    /// @brief Register all spokes in startup order.
    /// @param names Spoke names to declare.
    void declare_spokes(const std::vector<std::string>& names);

    /// @brief Start sequence: requests start through systemd in dependency order.
    /// @param names Spoke names to declare.
    ///
    /// Returns false when a required unit fails to start (the spoke
    /// re-registers on its own retry; istate keeps operating).
    /// @return True when every required unit started.
    bool start_all();
    /// @brief Stop in reverse order.
    void stop_all();

    /// @brief Spoke registration handshake outcome (proto mismatch refuses + alarms).
    enum class Reg
    {
        Accepted,        ///< Registered.
        SchemaMismatch,  ///< Proto version refused.
        NameMismatch     ///< Wrong daemon name.
    };
    /// @brief Process a registration handshake.
    /// @param refusal_reason Optional out-text for refusals.
    /// @param name Spoke registering.
    /// @param build_version Reported build version.
    /// @param proto_version Reported proto schema version.
    /// @param capabilities Reported capabilities.
    /// @return Process a registration handshake.
    Reg register_spoke(const std::string& name, const std::string& build_version, uint32_t proto_version, const std::vector<std::string>& capabilities, std::string* refusal_reason = nullptr);
    /// @brief Mark a spoke ready (channel established).
    /// @param name Spoke that became ready.
    void mark_ready(const std::string& name);
    /// @brief Mark a spoke stopped.
    /// @param name Spoke registering.
    /// @param build_version Reported build version.
    /// @param proto_version Reported proto schema version.
    /// @param capabilities Reported capabilities.
    /// @param refusal_reason Receives the refusal reason on refusal.
    /// @return The handshake outcome.
    void mark_stopped(const std::string& name);
    /// @brief A spoke sent data: it is attached and responsive.
    /// @param name Spoke that became ready.
    void note_spoke_activity(const std::string& name);

    /// @brief Install the ping sender (§3.1, §7.1 liveness).
    ///
    /// @param name Spoke that sent data.
    /// @param f Ping sender (daemon, nonce) → bool.
    /// istate probes each registered spoke; a process that is running but not
    /// responding must not look healthy. Hung → request restart. The probe is
    /// a ping/pong over the spoke's own channel: a liveness-ping command
    /// carrying a nonce, answered by the spoke's transport. A daemon wedged
    /// inside its own event loop stops answering while its process is still
    /// very much alive — which is the case this exists to catch.
    void set_ping_fn(std::function<bool(const std::string& daemon, uint64_t nonce)> f)
    {
        send_ping_ = std::move(f);
    }
    /// @brief A spoke answered a ping.
    /// @param daemon Spoke that answered.
    /// @param nonce Ping nonce answered.
    void note_pong(const std::string& daemon, uint64_t nonce);
    /// @brief Optional synchronous probe override (tests, and any spoke that offers a cheaper liveness signal).
    /// @param name Spoke to override.
    /// @param f Synchronous probe (true = alive).
    void set_probe_fn(const std::string& name, std::function<bool()> f)
    {
        probes_[name] = std::move(f);
    }
    /// @brief One liveness event.
    struct LivenessEvent
    {
        std::string daemon;    ///< Affected daemon.
        std::string event;     ///< hung / restarted / degraded.
    };
    /// @brief Sweep all spokes for hangs; returns events to emit.
    /// @param now_s Current monotonic seconds.
    /// @param hang_timeout_s Silence threshold in seconds.
    /// @return Liveness events to emit.
    std::vector<LivenessEvent> liveness_sweep(double now_s, double hang_timeout_s);

    /// @brief Watchdog policy: pet only while every must-live daemon is live.
    /// @param names Daemons that must be live for a pet.
    void set_watchdog_required(const std::vector<std::string>& names)
    {
        must_live_ = names;
    }
    /// @brief Pet the watchdog when safe; false when a must-live daemon is down.
    /// @param now_s Current monotonic seconds.
    /// @param hang_timeout_s Silence threshold in seconds.
    /// @return Liveness events to emit.
    bool pet_watchdog();

    /// @brief All managed spokes.
    /// @return All managed spokes.
    const std::map<std::string, SpokeInfo>& spokes() const
    {
        return spokes_;
    }
    /// @brief Look up one spoke.
    /// @param f Event sink (kind, detail).
    /// @param name Spoke to look up.
    /// @return Pointer to the info, or NotFound.
    Result<const SpokeInfo*> spoke(const std::string& name) const;
    /// @brief True when every declared spoke is Ready.
    /// @return True when every declared spoke is Ready.
    bool all_ready() const;
    /// @brief True when any spoke is Degraded or Hung.
    /// @return All managed spokes.
    bool degraded() const;
    /// @brief Render the fleet status as JSON (ctrl "status").
    /// @param name Spoke to look up.
    /// @return Pointer to the info, or NotFound.
    std::string status_json() const;

    /// @brief Install the event sink (liveness and lifecycle events).
    /// @param f Event sink (kind, detail).
    void set_event_sink(std::function<void(const std::string& kind, const std::string& detail)> f)
    {
        event_sink_ = std::move(f);
    }

  private:
    platform::IPlatform& plat_;       ///< systemd operations (not owned).
    TimeSource& ts_;                   ///< Clock (not owned).
    std::map<std::string, SpokeInfo> spokes_;                 ///< Managed spokes.
    std::map<std::string, std::function<bool()>> probes_;      ///< Per-spoke probe overrides.
    std::function<bool(const std::string&, uint64_t)> send_ping_;   ///< Ping sender.
    uint64_t next_nonce_ = 1;         ///< Ping nonce generator.
    std::vector<std::string> must_live_;    ///< Watchdog-gating daemons.
    std::function<void(const std::string&, const std::string&)> event_sink_;   ///< Event sink.
};

} // namespace istate
} // namespace instr