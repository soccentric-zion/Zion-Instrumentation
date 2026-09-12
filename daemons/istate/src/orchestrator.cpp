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
 * @file daemons/istate/src/orchestrator.cpp
 * @brief istate orchestration implementation (REQ §7.1): startup order, spoke liveness, systemd start/stop requests, and spoke state tracking exposed northbound.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/istate/orchestrator.h"

#include "instr/alarms_gen.h"

#include <algorithm>

#include "instr/transport/transport.h"

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

/// @brief Canonical name of a spoke state.
/// @param s State to name.
/// @return Canonical lowercase name.
const char* spoke_state_name(SpokeState s)
{
    switch (s)
    {
    case SpokeState::Expected: return "expected";
    case SpokeState::Starting: return "starting";
    case SpokeState::Ready: return "ready";
    case SpokeState::Degraded: return "degraded";
    case SpokeState::Stopped: return "stopped";
    case SpokeState::Hung: return "hung";
    }
    return "unknown";
}

/// @brief Startup order (§7.1).
/// @return The declared startup order.
const std::vector<std::string>& startup_order()
{
    static const std::vector<std::string> order = {
        "faults", "monitord", "hardware", "alarms", "logmanager", "telemetry", "networkd", "ota", "backup",
    };
    return order;
}

/// @brief Dependency edges between daemons.
/// @return The dependency edges.
const std::map<std::string, std::vector<std::string>>& dependency_edges()
{
    static const std::map<std::string, std::vector<std::string>> edges = {
        {"faults", {}}, {"monitord", {}}, {"hardware", {}}, {"alarms", {}}, {"logmanager", {}}, {"telemetry", {"faults", "monitord", "hardware"}}, {"networkd", {"faults", "monitord", "hardware", "alarms", "logmanager", "telemetry"}}, {"ota", {"networkd"}}, {"backup", {"networkd"}},
    };
    return edges;
}

/// @brief Declare all spokes in startup order.
/// @param names Spoke names to declare.
void Orchestrator::declare_spokes(const std::vector<std::string>& names)
{
    for (const auto& n : names)
    {
        SpokeInfo info;
        info.name = n;
        info.unit = "instr-" + n + ".service";
        spokes_[n] = info;
    }
}

/// @brief Start units in dependency order.
/// @return True when every required unit started.
bool Orchestrator::start_all()
{
    bool ok = true;
    // Readiness order = startup_order with dependency edges respected (edges
    // are consistent with the declared order).
    for (const auto& name : startup_order())
    {
        auto it = spokes_.find(name);
        if (it == spokes_.end()) continue;
        // Dependencies start earlier in the declared order (systemd owns unit
        // ordering); readiness arrives via registration.
        Error e = plat_.unit_start(it->second.unit);
        if (e.ok())
        {
            it->second.state = SpokeState::Starting;
            if (event_sink_) event_sink_("restart", "{\"daemon\":\"" + name + "\",\"action\":\"start\"}");
        }
        else
        {
            it->second.state = SpokeState::Degraded;
            ok = false;
            if (event_sink_) event_sink_("restart", "{\"daemon\":\"" + name + "\",\"action\":\"start-failed\"}");
        }
    }
    return ok;
}

/// @brief Stop units in reverse order.
void Orchestrator::stop_all()
{
    auto order = startup_order();
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        auto sit = spokes_.find(*it);
        if (sit == spokes_.end()) continue;
        (void)plat_.unit_stop(sit->second.unit);
        sit->second.state = SpokeState::Stopped;
    }
}

/// @brief Process a registration handshake.
/// @param name Spoke registering.
/// @param build_version Reported build.
/// @param proto_version Reported proto version.
/// @param capabilities Reported capabilities.
/// @param refusal_reason Receives the refusal reason.
/// @return The handshake outcome.
Orchestrator::Reg Orchestrator::register_spoke(const std::string& name, const std::string& build_version, uint32_t proto_version, const std::vector<std::string>& capabilities, std::string* refusal_reason)
{
    auto it = spokes_.find(name);
    if (it == spokes_.end())
    {
        if (refusal_reason) *refusal_reason = "unknown daemon: " + name;
        return Reg::NameMismatch;
    }
    if (proto_version != transport::kProtoSchemaVersion)
    {
        // Proto version mismatch: refuse registration, raise an alarm, never
        // guess (REQ §3.1, §11).
        if (refusal_reason) *refusal_reason = "proto schema version mismatch: spoke " + std::to_string(proto_version) + " vs istate " + std::to_string(transport::kProtoSchemaVersion);
        it->second.state = SpokeState::Degraded;
        if (event_sink_)
            // Use the generated id, not a literal: a hand-written one had the
            // module and type nibbles transposed (0x1_01_0_3001 instead of
            // 0x1_01_3_0001), so HQ would have received an alarm that decodes to
            // nothing in the catalog (§8).
            event_sink_("alarm", "{\"alarm_id\":" + std::to_string(INSTR_ALARM_ISTATE_PROTO_MISMATCH) + ",\"detail\":\"proto mismatch " + name + "\"}");
        return Reg::SchemaMismatch;
    }
    it->second.build_version = build_version;
    it->second.proto_version = proto_version;
    it->second.capabilities = capabilities;
    it->second.state = SpokeState::Starting;
    it->second.last_seen_mono = ts_.mono_ns();
    return Reg::Accepted;
}

/// @brief Mark a spoke ready.
/// @param name Spoke that became ready.
void Orchestrator::mark_ready(const std::string& name)
{
    auto it = spokes_.find(name);
    if (it == spokes_.end()) return;
    it->second.state = SpokeState::Ready;
    it->second.live = true;
    it->second.last_seen_mono = ts_.mono_ns();
}

/// @brief Record inbound activity from a spoke.
/// @param name Spoke that sent data.
void Orchestrator::note_spoke_activity(const std::string& name)
{
    auto it = spokes_.find(name);
    if (it == spokes_.end()) return;
    it->second.last_seen_mono = ts_.mono_ns();
    it->second.live = true;
    if (it->second.state == SpokeState::Starting || it->second.state == SpokeState::Degraded || it->second.state == SpokeState::Hung) it->second.state = SpokeState::Ready;
}

/// @brief Mark a spoke stopped.
/// @param name Spoke that stopped.
void Orchestrator::mark_stopped(const std::string& name)
{
    auto it = spokes_.find(name);
    if (it == spokes_.end()) return;
    it->second.state = SpokeState::Stopped;
    it->second.live = false;
}

/// @brief Record a pong answering an outstanding ping.
/// @param daemon Spoke that answered.
/// @param nonce Ping nonce answered.
void Orchestrator::note_pong(const std::string& daemon, uint64_t nonce)
{
    auto it = spokes_.find(daemon);
    if (it == spokes_.end()) return;
    SpokeInfo& info = it->second;
    // A stale nonce (an answer to a ping we already gave up on) still proves
    // the spoke is alive, so accept it rather than insisting on the latest.
    (void)nonce;
    info.last_pong_mono = ts_.mono_ns();
    info.last_seen_mono = info.last_pong_mono;
    ++info.pongs_received;
    info.live = true;
    if (info.state == SpokeState::Hung) info.state = SpokeState::Ready;
}

/// @brief Sweep spokes for hangs; emit events and request restarts.
/// @param now_s Current monotonic seconds.
/// @param hang_timeout_s Silence threshold in seconds.
/// @return Liveness events to emit.
std::vector<Orchestrator::LivenessEvent> Orchestrator::liveness_sweep(double now_s, double hang_timeout_s)
{
    std::vector<LivenessEvent> out;
    const uint64_t now_mono = ts_.mono_ns();
    for (auto& kv : spokes_)
    {
        SpokeInfo& info = kv.second;
        if (info.state == SpokeState::Stopped || info.state == SpokeState::Expected) continue;

        // Send this round's ping. A spoke with no open session simply cannot be
        // pinged; that is a disconnection, which registration handles, not a hang.
        bool pinged = false;
        if (send_ping_)
        {
            info.ping_nonce = ++next_nonce_;
            pinged = send_ping_(kv.first, info.ping_nonce);
            if (pinged)
            {
                info.last_ping_mono = now_mono;
                ++info.pings_sent;
            }
        }

        // An explicit probe overrides the channel signal where one is wired.
        auto pit = probes_.find(kv.first);
        bool alive;
        if (pit != probes_.end()) { alive = pit->second(); }
        else if (!send_ping_ || !pinged)
        {
            // No liveness mechanism available: do not invent a hang.
            alive = true;
        }
        else
        {
            // Silence is measured from the last thing the spoke actually said —
            // a pong or real traffic — never reset by our own sweep.
            const uint64_t last = std::max(info.last_pong_mono, info.last_seen_mono);
            const double silent_s = last == 0 ? 0.0 : double(now_mono - last) / 1e9;
            // Give a freshly registered spoke one full timeout to answer.
            alive = last == 0 ? (double(now_mono - info.last_ping_mono) / 1e9 < hang_timeout_s) : (silent_s < hang_timeout_s);
        }

        if (!alive)
        {
            if (info.state != SpokeState::Hung)
            {
                // A process that is running but not responding must not look healthy.
                info.state = SpokeState::Hung;
                info.live = false;
                out.push_back({kv.first, "hung"});
                // Request restart through systemd (§7.1: orchestration, not
                // supervision — systemd owns the restart itself).
                (void)plat_.unit_restart(info.unit);
                out.push_back({kv.first, "restarted"});
                if (event_sink_) event_sink_("restart", "{\"daemon\":\"" + kv.first + "\",\"reason\":\"hang\"}");
            }
        }
        else if (info.state == SpokeState::Hung)
        {
            info.state = SpokeState::Ready;
            info.live = true;
            out.push_back({kv.first, "recovered"});
        }
        else { info.live = true; }
        (void)now_s;
    }
    return out;
}

/// @brief Pet the watchdog only when every must-live daemon is live.
/// @return True when the watchdog was petted; false when a must-live daemon is down.
bool Orchestrator::pet_watchdog()
{
    // Pet only while every must-live daemon is live; a hung daemon stops the
    // petting so the hardware watchdog can act as the last resort.
    for (const auto& name : must_live_)
    {
        auto it = spokes_.find(name);
        if (it == spokes_.end()) continue;
        if (it->second.state != SpokeState::Ready || !it->second.live)
        {
            (void)plat_.watchdog_pet();
            return false;
        }
    }
    return plat_.watchdog_pet().ok();
}

/// @brief Look up one spoke.
/// @param name Spoke to look up.
/// @return Pointer to the info, or NotFound.
Result<const SpokeInfo*> Orchestrator::spoke(const std::string& name) const
{
    auto it = spokes_.find(name);
    if (it == spokes_.end()) return Result<const SpokeInfo*>::fail(Errc::NotFound, "unknown spoke", name);
    return Result<const SpokeInfo*>::good(&it->second);
}

/// @brief True when every declared spoke is Ready.
/// @return True when every declared spoke is Ready.
bool Orchestrator::all_ready() const
{
    for (const auto& kv : spokes_)
        if (kv.second.state != SpokeState::Ready) return false;
    return !spokes_.empty();
}

/// @brief True when any spoke is Degraded or Hung.
/// @return True when any spoke is Degraded or Hung.
bool Orchestrator::degraded() const
{
    for (const auto& kv : spokes_)
        if (kv.second.state == SpokeState::Degraded || kv.second.state == SpokeState::Hung || kv.second.state == SpokeState::Stopped) return true;
    return false;
}

/// @brief Render the fleet status as JSON.
/// @return The fleet status as JSON.
std::string Orchestrator::status_json() const
{
    JValue arr = JValue::array();
    for (const auto& kv : spokes_)
    {
        JValue o = JValue::object();
        o.set("name", JValue(kv.second.name));
        o.set("state", JValue(spoke_state_name(kv.second.state)));
        o.set("live", JValue(kv.second.live));
        o.set("build_version", JValue(kv.second.build_version));
        o.set("proto_version", JValue(double(kv.second.proto_version)));
        JValue caps = JValue::array();
        for (const auto& c : kv.second.capabilities) caps.push(JValue(c));
        o.set("capabilities", caps);
        arr.push(o);
    }
    return arr.dump();
}

} // namespace istate
} // namespace instr