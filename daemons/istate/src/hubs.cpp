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
 * @file daemons/istate/src/hubs.cpp
 * @brief istate gRPC server wiring (REQ §3.1, §3.3): nine per-spoke hub services (one socket per daemon), the Configuration API, and the HQ session client.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/istate/hubs.h"

#include "instr/common/version_gen.h"

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

using transport::GenericHubMsg;
using transport::GenericSpokeMsg;
using transport::HubServiceBase;

// Socket permissions are how the call graph is enforced (§3.1, §10): each
// spoke's registration socket is mode 0660 owned instr:instr-<daemon>, so
// only that daemon's own user can dial it. A single shared group would let
// any spoke call any other's hub service, which is the N x N mesh R1 exists
// to prevent — expressed in filesystem permissions rather than in code.
//
// Running unprivileged (tests, the host harness, a container without
// CAP_CHOWN) the chown cannot succeed. That is reported once, loudly, rather
// than passed over: on a device it means the enforcement is not in place.
/// @brief Create/chown one spoke socket so only that daemon's user may connect (§3.1).
/// @param path Socket path to create/chown.
/// @param daemon Owning daemon (its user is looked up).
/// @param log Logger for diagnostics.
/// @return True when the socket was prepared.
static bool own_socket(const std::string& path, const std::string& daemon, Logger* log)
{
    if (::chmod(path.c_str(), 0660) != 0)
    {
        if (log) log->warning("istate", "cannot chmod " + path + ": " + std::strerror(errno));
        return false;
    }
    // "configuration" is the local UI peer (§3.3), which runs as its own user.
    // "application" is the colocated-application face (R5, §7.8): it is owned
    // by istate and grouped to instr-apps, because there is no single
    // application user — any number of apps join that group.
    const bool app_face = daemon == "application";
    const std::string user = app_face ? "instr-istate" : daemon == "configuration" ? "instr-configuration" : "instr-" + daemon;
    const std::string group = app_face ? "instr-apps" : "instr";
    struct passwd* pw = ::getpwnam(user.c_str());
    struct group* gr = ::getgrnam(group.c_str());
    if (!pw || !gr)
    {
        if (log) log->warning("istate", "socket ownership not applied for " + path + ": no such user '" + user + "' or group '" + group + "'");
        return false;
    }
    if (::chown(path.c_str(), pw->pw_uid, gr->gr_gid) != 0)
    {
        if (log) log->warning("istate", "cannot chown " + path + " to " + user + ":" + group + ": " + std::strerror(errno) + " — the socket call graph is NOT enforced");
        return false;
    }
    return true;
}

// ---- typed per-spoke hub services ----

/// @brief Typed per-spoke hub service bound to the istate core.
/// @tparam Service Generated `<X>Hub::Service` type.
/// @tparam SpokeFrame Typed spoke frame.
/// @tparam HubFrame Typed hub frame.
template <typename Service, typename SpokeFrame, typename HubFrame> class TypedHub : public HubServiceBase<Service, SpokeFrame, HubFrame>
{
  public:
    /// @brief Construct a hub for @p daemon wiring ingest into the istate core.
    /// @param core istate core the hub ingests into.
    /// @param daemon Spoke identity this hub accepts.
    /// @param ' '..
    /// @param S S..
    /// @param e E..
    /// @param r R..
    /// @param v V..
    /// @param i I..
    /// @param c C..
    /// @param e E..
    /// @param ' '..
    /// @param , ,..
    /// @param    ..
    /// @param ' '..
    /// @param S S..
    /// @param p P..
    /// @param o O..
    /// @param k K..
    /// @param e E..
    /// @param F F..
    /// @param r R..
    /// @param a A..
    /// @param m M..
    /// @param e E..
    /// @param ' '..
    /// @param ' '..
    /// @param S S..
    /// @param e E..
    /// @param r R..
    /// @param v V..
    /// @param i I..
    /// @param c C..
    /// @param e E..
    /// @param ' '..
    /// @param , ,..
    /// @param    ..
    /// @param ' '..
    /// @param S S..
    /// @param p P..
    /// @param o O..
    /// @param k K..
    /// @param e E..
    /// @param F F..
    /// @param r R..
    /// @param a A..
    /// @param m M..
    /// @param e E..
    /// @param ' '..
    /// @param ' '..
    /// @param S S..
    /// @param e E..
    /// @param r R..
    /// @param v V..
    /// @param i I..
    /// @param c C..
    /// @param e E..
    /// @param ' '..
    /// @param , ,..
    /// @param    ..
    /// @param ' '..
    /// @param S S..
    /// @param p P..
    /// @param o O..
    /// @param k K..
    /// @param e E..
    /// @param F F..
    /// @param r R..
    /// @param a A..
    /// @param m M..
    /// @param e E..
    /// @param ' '..
    /// @param ' '..
    /// @param S S..
    /// @param e E..
    /// @param r R..
    /// @param v V..
    /// @param i I..
    /// @param c C..
    /// @param e E..
    /// @param ' '..
    /// @param , ,..
    /// @param    ..
    /// @param ' '..
    /// @param S S..
    /// @param p P..
    /// @param o O..
    /// @param k K..
    /// @param e E..
    /// @param F F..
    /// @param r R..
    /// @param a A..
    /// @param m M..
    /// @param e E..
    /// @param ' '..
    TypedHub(IstateCore& core, std::string daemon) : HubServiceBase<Service, SpokeFrame, HubFrame>(daemon, INSTR_VERSION), core_(core)
    {
        this->set_ingest([this, &core](const GenericSpokeMsg& m) { core.on_spoke_msg(m); });
        // Record the handshake in the orchestrator: it owns spoke state, and a
        // schema mismatch has to raise an alarm rather than being refused in
        // silence (§3.1, §11).
        this->set_registration_fn([&core](const common::RegistrationRequest& req, bool accepted) { core.on_spoke_registration(req, accepted); });
    }
    // Forward hub frames to this spoke's open channel.
    /// @brief Forward a hub message to this spoke's open channel.
    /// @param m Hub message to relay.
    /// @return True when queued.
    bool forward(const GenericHubMsg& m)
    {
        return open_ ? (this->push(m), true) : false;
    }
    /// @brief Record whether the spoke channel is open.
    /// @param v True when the channel opens.
    void set_open(bool v)
    {
        open_ = v;
    }

  protected:
    IstateCore& core_;
    std::atomic<bool> open_{false};
};

// Macro-free explicit conversions per daemon (typed, compile-checked).

class MonitorHubImpl : public TypedHub<monitord::MonitorHub::Service, monitord::MonitorSpokeFrame, monitord::MonitorHubFrame>
{
  public:
    using TypedHub::TypedHub;
    /// @brief Convert a typed monitord spoke frame into the generic transport message.
    /// @param f Typed spoke frame to convert.
    /// @return The generic transport message.
    GenericSpokeMsg to_generic(const monitord::MonitorSpokeFrame& f) override
    {
        GenericSpokeMsg m;
        m.daemon = "monitord";
        if (f.has_sample())
        {
            m.type = "sample";
            JValue o = JValue::object();
            o.set("metric", JValue(f.sample().metric()));
            o.set("value", JValue(f.sample().value()));
            o.set("unit", JValue(f.sample().unit()));
            o.set("resource_class", JValue(f.sample().resource_class()));
            JValue tags = JValue::object();
            for (const auto& kv : f.sample().tags()) tags.set(kv.first, JValue(kv.second));
            o.set("tags", tags);
            o.set("mono_ns", JValue(double(f.sample().ts().mono_ns())));
            m.payload_json = o.dump();
            m.ts = f.sample().ts();
        }
        else if (f.has_wear())
        {
            m.type = "wear";
            JValue o = JValue::object();
            o.set("device", JValue(f.wear().device()));
            o.set("wear_pct", JValue(f.wear().wear_pct()));
            o.set("remaining_days", JValue(double(f.wear().remaining_days())));
            m.payload_json = o.dump();
        }
        else if (f.has_ack())
        {
            m.type = "ack";
            m.payload_json = "null";
            core_.on_spoke_ack("monitord", f.ack());
        }
        else if (f.has_pong())
        {
            m.type = "pong";
            m.payload_json = "{\"nonce\":" + std::to_string(f.pong().nonce()) + "}";
        }
        else if (f.has_config_request())
        {
            m.type = "config_request";
            m.payload_json = "{}";
        }
        else if (f.has_config_ack())
        {
            m.type = "config_ack";
            JValue o = JValue::object();
            o.set("revision", JValue(double(f.config_ack().revision())));
            o.set("applied", JValue(f.config_ack().applied()));
            o.set("reason", JValue(f.config_ack().reason()));
            m.payload_json = o.dump();
        }
        else if (!f.status_json().empty())
        {
            m.type = "status";
            m.payload_json = f.status_json();
        }
        return m;
    }
    /// @brief Apply a generic hub message onto a typed monitord hub frame.
    /// @param f Typed hub frame to fill.
    /// @param m Generic hub message.
    /// @return None.
    void apply(monitord::MonitorHubFrame& f, const GenericHubMsg& m) override
    {
        if (m.has_command) *f.mutable_command() = m.command;
        if (m.has_backpressure) *f.mutable_backpressure() = m.backpressure;
        if (m.has_relay) *f.mutable_relay() = m.relay;
        if (m.has_time_sync) *f.mutable_time_sync() = m.time_sync;
        if (m.has_config) *f.mutable_config() = m.config;
    }
};

// The remaining eight spokes share the identical shape; conversions differ
// only in payload field names. Implemented explicitly per daemon.

class NetworkHubImpl : public TypedHub<networkd::NetworkHub::Service, networkd::NetworkSpokeFrame, networkd::NetworkHubFrame>
{
  public:
    using TypedHub::TypedHub;
    /// @brief Convert a typed networkd spoke frame into the generic transport message.
    /// @param f Typed spoke frame to convert.
    /// @return The generic transport message.
    GenericSpokeMsg to_generic(const networkd::NetworkSpokeFrame& f) override
    {
        GenericSpokeMsg m;
        m.daemon = "networkd";
        if (f.has_event())
        {
            m.type = "network_event";
            m.payload_json = f.event().detail_json().empty() ? "{}" : f.event().detail_json();
            m.ts = f.event().ts();
        }
        else if (f.has_ack())
        {
            m.type = "ack";
            core_.on_spoke_ack("networkd", f.ack());
        }
        else if (f.has_pong())
        {
            m.type = "pong";
            m.payload_json = "{\"nonce\":" + std::to_string(f.pong().nonce()) + "}";
        }
        else if (f.has_config_request())
        {
            m.type = "config_request";
            m.payload_json = "{}";
        }
        else if (f.has_config_ack())
        {
            m.type = "config_ack";
            JValue o = JValue::object();
            o.set("revision", JValue(double(f.config_ack().revision())));
            o.set("applied", JValue(f.config_ack().applied()));
            o.set("reason", JValue(f.config_ack().reason()));
            m.payload_json = o.dump();
        }
        else if (!f.status_json().empty())
        {
            m.type = "status";
            m.payload_json = f.status_json();
        }
        return m;
    }
    /// @brief Apply a generic hub message onto a typed networkd hub frame.
    /// @param f Typed hub frame to fill.
    /// @param m Generic hub message.
    /// @return None.
    void apply(networkd::NetworkHubFrame& f, const GenericHubMsg& m) override
    {
        if (m.has_command) *f.mutable_command() = m.command;
        if (m.has_backpressure) *f.mutable_backpressure() = m.backpressure;
        if (m.has_relay) *f.mutable_relay() = m.relay;
        if (m.has_time_sync) *f.mutable_time_sync() = m.time_sync;
        if (m.has_config) *f.mutable_config() = m.config;
    }
};

class HardwareHubImpl : public TypedHub<hardware::HardwareHub::Service, hardware::HardwareSpokeFrame, hardware::HardwareHubFrame>
{
  public:
    using TypedHub::TypedHub;
    /// @brief Convert a typed hardware spoke frame into the generic transport message.
    /// @param f Typed spoke frame to convert.
    /// @return The generic transport message.
    GenericSpokeMsg to_generic(const hardware::HardwareSpokeFrame& f) override
    {
        GenericSpokeMsg m;
        m.daemon = "hardware";
        if (f.has_result())
        {
            m.type = "health_result";
            JValue o = JValue::object();
            o.set("device", JValue(f.result().device()));
            o.set("check", JValue(f.result().check()));
            o.set("state", JValue(common::HealthState_Name(f.result().state())));
            o.set("detail", JValue(f.result().detail()));
            m.payload_json = o.dump();
            m.ts = f.result().ts();
        }
        else if (f.has_ack())
        {
            m.type = "ack";
            core_.on_spoke_ack("hardware", f.ack());
        }
        else if (f.has_pong())
        {
            m.type = "pong";
            m.payload_json = "{\"nonce\":" + std::to_string(f.pong().nonce()) + "}";
        }
        else if (f.has_config_request())
        {
            m.type = "config_request";
            m.payload_json = "{}";
        }
        else if (f.has_config_ack())
        {
            m.type = "config_ack";
            JValue o = JValue::object();
            o.set("revision", JValue(double(f.config_ack().revision())));
            o.set("applied", JValue(f.config_ack().applied()));
            o.set("reason", JValue(f.config_ack().reason()));
            m.payload_json = o.dump();
        }
        else if (!f.status_json().empty())
        {
            m.type = "status";
            m.payload_json = f.status_json();
        }
        return m;
    }
    /// @brief Apply a generic hub message onto a typed hardware hub frame.
    /// @param f Typed hub frame to fill.
    /// @param m Generic hub message.
    /// @return None.
    void apply(hardware::HardwareHubFrame& f, const GenericHubMsg& m) override
    {
        if (m.has_command) *f.mutable_command() = m.command;
        if (m.has_backpressure) *f.mutable_backpressure() = m.backpressure;
        if (m.has_relay) *f.mutable_relay() = m.relay;
        if (m.has_time_sync) *f.mutable_time_sync() = m.time_sync;
        if (m.has_config) *f.mutable_config() = m.config;
    }
};

class FaultHubImpl : public TypedHub<faults::FaultHub::Service, faults::FaultSpokeFrame, faults::FaultHubFrame>
{
  public:
    using TypedHub::TypedHub;
    /// @brief Convert a typed faults spoke frame into the generic transport message.
    /// @param f Typed spoke frame to convert.
    /// @return The generic transport message.
    GenericSpokeMsg to_generic(const faults::FaultSpokeFrame& f) override
    {
        GenericSpokeMsg m;
        m.daemon = "faults";
        if (f.has_event())
        {
            m.type = "fault";
            JValue o = JValue::object();
            o.set("fault_id", JValue(double(f.event().fault_id())));
            o.set("rule", JValue(f.event().rule()));
            o.set("severity", JValue(double(f.event().severity())));
            o.set("raw_line", JValue(f.event().raw_line()));
            o.set("occurrences", JValue(double(f.event().occurrences())));
            o.set("device_key", JValue(f.event().device_key()));
            o.set("x733_type", JValue(f.event().x733_type()));
            o.set("backfill", JValue(f.event().backfill()));
            o.set("mono_ns", JValue(double(f.event().ts().mono_ns())));
            m.payload_json = o.dump();
            m.ts = f.event().ts();
        }
        else if (f.has_ack())
        {
            m.type = "ack";
            core_.on_spoke_ack("faults", f.ack());
        }
        else if (f.has_pong())
        {
            m.type = "pong";
            m.payload_json = "{\"nonce\":" + std::to_string(f.pong().nonce()) + "}";
        }
        else if (f.has_config_request())
        {
            m.type = "config_request";
            m.payload_json = "{}";
        }
        else if (f.has_config_ack())
        {
            m.type = "config_ack";
            JValue o = JValue::object();
            o.set("revision", JValue(double(f.config_ack().revision())));
            o.set("applied", JValue(f.config_ack().applied()));
            o.set("reason", JValue(f.config_ack().reason()));
            m.payload_json = o.dump();
        }
        else if (!f.status_json().empty())
        {
            m.type = "status";
            m.payload_json = f.status_json();
        }
        return m;
    }
    /// @brief Apply a generic hub message onto a typed faults hub frame.
    /// @param f Typed hub frame to fill.
    /// @param m Generic hub message.
    /// @return None.
    void apply(faults::FaultHubFrame& f, const GenericHubMsg& m) override
    {
        if (m.has_command) *f.mutable_command() = m.command;
        if (m.has_backpressure) *f.mutable_backpressure() = m.backpressure;
        if (m.has_relay) *f.mutable_relay() = m.relay;
        if (m.has_time_sync) *f.mutable_time_sync() = m.time_sync;
        if (m.has_config) *f.mutable_config() = m.config;
    }
};

class AlarmHubImpl : public TypedHub<alarms::AlarmHub::Service, alarms::AlarmSpokeFrame, alarms::AlarmHubFrame>
{
  public:
    using TypedHub::TypedHub;
    /// @brief Convert a typed alarms spoke frame into the generic transport message.
    /// @param f Typed spoke frame to convert.
    /// @return The generic transport message.
    GenericSpokeMsg to_generic(const alarms::AlarmSpokeFrame& f) override
    {
        GenericSpokeMsg m;
        m.daemon = "alarms";
        if (f.has_event())
        {
            m.type = "alarm";
            JValue o = JValue::object();
            o.set("alarm_id", JValue(double(f.event().alarm_id())));
            o.set("action", JValue(f.event().action()));
            o.set("severity", JValue(double(f.event().severity())));
            o.set("owner", JValue(f.event().owner()));
            o.set("additional_text", JValue(f.event().additional_text()));
            o.set("count", JValue(double(f.event().count())));
            o.set("acknowledged", JValue(f.event().acknowledged()));
            o.set("needs_snapshot", JValue(f.event().needs_snapshot()));
            o.set("mono_ns", JValue(double(f.event().ts().mono_ns())));
            m.payload_json = o.dump();
            m.ts = f.event().ts();
        }
        else if (f.has_ack())
        {
            m.type = "ack";
            core_.on_spoke_ack("alarms", f.ack());
        }
        else if (f.has_pong())
        {
            m.type = "pong";
            m.payload_json = "{\"nonce\":" + std::to_string(f.pong().nonce()) + "}";
        }
        else if (f.has_config_request())
        {
            m.type = "config_request";
            m.payload_json = "{}";
        }
        else if (f.has_config_ack())
        {
            m.type = "config_ack";
            JValue o = JValue::object();
            o.set("revision", JValue(double(f.config_ack().revision())));
            o.set("applied", JValue(f.config_ack().applied()));
            o.set("reason", JValue(f.config_ack().reason()));
            m.payload_json = o.dump();
        }
        else if (!f.status_json().empty())
        {
            m.type = "status";
            m.payload_json = f.status_json();
        }
        return m;
    }
    /// @brief Apply a generic hub message onto a typed alarms hub frame.
    /// @param f Typed hub frame to fill.
    /// @param m Generic hub message.
    /// @return None.
    void apply(alarms::AlarmHubFrame& f, const GenericHubMsg& m) override
    {
        if (m.has_command) *f.mutable_command() = m.command;
        if (m.has_backpressure) *f.mutable_backpressure() = m.backpressure;
        if (m.has_relay) *f.mutable_relay() = m.relay;
        if (m.has_time_sync) *f.mutable_time_sync() = m.time_sync;
        if (m.has_config) *f.mutable_config() = m.config;
    }
};

class LogHubImpl : public TypedHub<logmanager::LogHub::Service, logmanager::LogSpokeFrame, logmanager::LogHubFrame>
{
  public:
    using TypedHub::TypedHub;
    /// @brief Convert a typed logmanager spoke frame into the generic transport message.
    /// @param f Typed spoke frame to convert.
    /// @return The generic transport message.
    GenericSpokeMsg to_generic(const logmanager::LogSpokeFrame& f) override
    {
        GenericSpokeMsg m;
        m.daemon = "logmanager";
        if (f.has_line())
        {
            m.type = "tail_line";
            JValue o = JValue::object();
            o.set("source", JValue(f.line().source()));
            o.set("level", JValue(double(f.line().level())));
            o.set("message", JValue(f.line().message()));
            o.set("mono_ns", JValue(double(f.line().ts().mono_ns())));
            m.payload_json = o.dump();
            m.ts = f.line().ts();
        }
        else if (f.has_bundle())
        {
            m.type = "bundle";
            JValue o = JValue::object();
            o.set("handle", JValue(f.bundle().handle()));
            o.set("meta", JValue(f.bundle().meta_json()));
            o.set("size", JValue(double(f.bundle().size_bytes())));
            o.set("sha256", JValue(f.bundle().sha256()));
            m.payload_json = o.dump();
        }
        else if (f.has_crash())
        {
            m.type = "crash";
            JValue o = JValue::object();
            o.set("daemon", JValue(f.crash().daemon()));
            o.set("coredump_handle", JValue(f.crash().coredump_handle()));
            o.set("tail_lines", JValue(double(f.crash().tail_lines())));
            m.payload_json = o.dump();
        }
        else if (f.has_ack())
        {
            m.type = "ack";
            core_.on_spoke_ack("logmanager", f.ack());
        }
        else if (f.has_pong())
        {
            m.type = "pong";
            m.payload_json = "{\"nonce\":" + std::to_string(f.pong().nonce()) + "}";
        }
        else if (f.has_config_request())
        {
            m.type = "config_request";
            m.payload_json = "{}";
        }
        else if (f.has_config_ack())
        {
            m.type = "config_ack";
            JValue o = JValue::object();
            o.set("revision", JValue(double(f.config_ack().revision())));
            o.set("applied", JValue(f.config_ack().applied()));
            o.set("reason", JValue(f.config_ack().reason()));
            m.payload_json = o.dump();
        }
        else if (!f.status_json().empty())
        {
            m.type = "status";
            m.payload_json = f.status_json();
        }
        return m;
    }
    /// @brief Apply a generic hub message onto a typed logmanager hub frame.
    /// @param f Typed hub frame to fill.
    /// @param m Generic hub message.
    /// @return None.
    void apply(logmanager::LogHubFrame& f, const GenericHubMsg& m) override
    {
        if (m.has_command) *f.mutable_command() = m.command;
        if (m.has_backpressure) *f.mutable_backpressure() = m.backpressure;
        if (m.has_relay) *f.mutable_relay() = m.relay;
        if (m.has_time_sync) *f.mutable_time_sync() = m.time_sync;
        if (m.has_config) *f.mutable_config() = m.config;
    }
};

class TelemetryHubImpl : public TypedHub<telemetry::TelemetryHub::Service, telemetry::TelemetrySpokeFrame, telemetry::TelemetryHubFrame>
{
  public:
    using TypedHub::TypedHub;
    /// @brief Convert a typed telemetry spoke frame into the generic transport message.
    /// @param f Typed spoke frame to convert.
    /// @return The generic transport message.
    GenericSpokeMsg to_generic(const telemetry::TelemetrySpokeFrame& f) override
    {
        GenericSpokeMsg m;
        m.daemon = "telemetry";
        if (f.has_batch())
        {
            m.type = "batch";
            JValue o = JValue::object();
            o.set("records", JValue(double(f.batch().records_size())));
            o.set("health_score", JValue(f.batch().health().score()));
            o.set("firmware_version", JValue(f.batch().firmware_version()));
            o.set("mono_ns", JValue(double(f.batch().ts().mono_ns())));
            m.payload_json = o.dump();
            m.ts = f.batch().ts();
        }
        else if (f.has_ack())
        {
            m.type = "ack";
            core_.on_spoke_ack("telemetry", f.ack());
        }
        else if (f.has_pong())
        {
            m.type = "pong";
            m.payload_json = "{\"nonce\":" + std::to_string(f.pong().nonce()) + "}";
        }
        else if (f.has_config_request())
        {
            m.type = "config_request";
            m.payload_json = "{}";
        }
        else if (f.has_config_ack())
        {
            m.type = "config_ack";
            JValue o = JValue::object();
            o.set("revision", JValue(double(f.config_ack().revision())));
            o.set("applied", JValue(f.config_ack().applied()));
            o.set("reason", JValue(f.config_ack().reason()));
            m.payload_json = o.dump();
        }
        else if (!f.status_json().empty())
        {
            m.type = "status";
            m.payload_json = f.status_json();
        }
        return m;
    }
    /// @brief Apply a generic hub message onto a typed telemetry hub frame.
    /// @param f Typed hub frame to fill.
    /// @param m Generic hub message.
    /// @return None.
    void apply(telemetry::TelemetryHubFrame& f, const GenericHubMsg& m) override
    {
        if (m.has_command) *f.mutable_command() = m.command;
        if (m.has_backpressure) *f.mutable_backpressure() = m.backpressure;
        if (m.has_relay) *f.mutable_relay() = m.relay;
        if (m.has_time_sync) *f.mutable_time_sync() = m.time_sync;
        if (m.has_config) *f.mutable_config() = m.config;
    }
};

class OtaHubImpl : public TypedHub<ota::OtaHub::Service, ota::OtaSpokeFrame, ota::OtaHubFrame>
{
  public:
    using TypedHub::TypedHub;
    /// @brief Convert a typed ota spoke frame into the generic transport message.
    /// @param f Typed spoke frame to convert.
    /// @return The generic transport message.
    GenericSpokeMsg to_generic(const ota::OtaSpokeFrame& f) override
    {
        GenericSpokeMsg m;
        m.daemon = "ota";
        if (f.has_event())
        {
            m.type = "ota_event";
            JValue o = JValue::object();
            o.set("stage", JValue(f.event().stage()));
            o.set("progress", JValue(f.event().progress()));
            o.set("message", JValue(f.event().message()));
            o.set("from_version", JValue(f.event().from_version()));
            o.set("to_version", JValue(f.event().to_version()));
            m.payload_json = o.dump();
            m.ts = f.event().ts();
        }
        else if (f.has_ack())
        {
            m.type = "ack";
            core_.on_spoke_ack("ota", f.ack());
        }
        else if (f.has_pong())
        {
            m.type = "pong";
            m.payload_json = "{\"nonce\":" + std::to_string(f.pong().nonce()) + "}";
        }
        else if (f.has_config_request())
        {
            m.type = "config_request";
            m.payload_json = "{}";
        }
        else if (f.has_config_ack())
        {
            m.type = "config_ack";
            JValue o = JValue::object();
            o.set("revision", JValue(double(f.config_ack().revision())));
            o.set("applied", JValue(f.config_ack().applied()));
            o.set("reason", JValue(f.config_ack().reason()));
            m.payload_json = o.dump();
        }
        else if (!f.status_json().empty())
        {
            m.type = "status";
            m.payload_json = f.status_json();
        }
        return m;
    }
    /// @brief Apply a generic hub message onto a typed ota hub frame.
    /// @param f Typed hub frame to fill.
    /// @param m Generic hub message.
    /// @return None.
    void apply(ota::OtaHubFrame& f, const GenericHubMsg& m) override
    {
        if (m.has_command) *f.mutable_command() = m.command;
        if (m.has_backpressure) *f.mutable_backpressure() = m.backpressure;
        if (m.has_relay) *f.mutable_relay() = m.relay;
        if (m.has_time_sync) *f.mutable_time_sync() = m.time_sync;
        if (m.has_config) *f.mutable_config() = m.config;
    }
};

class BackupHubImpl : public TypedHub<backup::BackupHub::Service, backup::BackupSpokeFrame, backup::BackupHubFrame>
{
  public:
    using TypedHub::TypedHub;
    /// @brief Convert a typed backup spoke frame into the generic transport message.
    /// @param f Typed spoke frame to convert.
    /// @return The generic transport message.
    GenericSpokeMsg to_generic(const backup::BackupSpokeFrame& f) override
    {
        GenericSpokeMsg m;
        m.daemon = "backup";
        if (f.has_event())
        {
            m.type = "backup_event";
            JValue o = JValue::object();
            o.set("action", JValue(f.event().action()));
            o.set("archive", JValue(f.event().archive()));
            o.set("ok", JValue(f.event().ok()));
            o.set("error", JValue(f.event().error()));
            o.set("meta", JValue(f.event().meta_json()));
            m.payload_json = o.dump();
            m.ts = f.event().ts();
        }
        else if (f.has_ack())
        {
            m.type = "ack";
            core_.on_spoke_ack("backup", f.ack());
        }
        else if (f.has_pong())
        {
            m.type = "pong";
            m.payload_json = "{\"nonce\":" + std::to_string(f.pong().nonce()) + "}";
        }
        else if (f.has_config_request())
        {
            m.type = "config_request";
            m.payload_json = "{}";
        }
        else if (f.has_config_ack())
        {
            m.type = "config_ack";
            JValue o = JValue::object();
            o.set("revision", JValue(double(f.config_ack().revision())));
            o.set("applied", JValue(f.config_ack().applied()));
            o.set("reason", JValue(f.config_ack().reason()));
            m.payload_json = o.dump();
        }
        else if (!f.status_json().empty())
        {
            m.type = "status";
            m.payload_json = f.status_json();
        }
        return m;
    }
    /// @brief Apply a generic hub message onto a typed backup hub frame.
    /// @param f Typed hub frame to fill.
    /// @param m Generic hub message.
    /// @return None.
    void apply(backup::BackupHubFrame& f, const GenericHubMsg& m) override
    {
        if (m.has_command) *f.mutable_command() = m.command;
        if (m.has_backpressure) *f.mutable_backpressure() = m.backpressure;
        if (m.has_relay) *f.mutable_relay() = m.relay;
        if (m.has_time_sync) *f.mutable_time_sync() = m.time_sync;
        if (m.has_config) *f.mutable_config() = m.config;
    }
};

// ---- Configuration-facing API (§3.3: Configuration is architecturally
// identical to a spoke; every read/write is an istate call) ----

class ConfigurationApiImpl final : public istate::ConfigurationApi::Service
{
  public:
    /// @brief Construct the Configuration API service over the core.
    /// @param core istate core.
    explicit ConfigurationApiImpl(IstateCore& core) : core_(core)
    {
    }

    /// @brief Configuration/Application API: system info for the peer.
    /// @param reply Receives the system info.
    /// @return gRPC status.
    grpc::Status GetSystemInfo(grpc::ServerContext*, const istate::SystemInfoRequest*, istate::SystemInfo* reply) override
    {
        auto j = json_parse(core_.system_info_json());
        if (!j.ok()) return grpc::Status(grpc::StatusCode::INTERNAL, j.error.message);
        reply->set_hostname((*j).get_string("hostname"));
        reply->set_serial((*j).get_string("serial"));
        reply->set_hw_revision((*j).get_string("hw_revision"));
        reply->set_board_variant((*j).get_string("board_variant"));
        reply->set_firmware_version((*j).get_string("firmware_version"));
        reply->set_suite_version((*j).get_string("suite_version"));
        reply->set_config_schema_version((*j).get_string("config_schema_version"));
        reply->set_istate_state((*j).get_string("istate_state"));
        reply->set_provisioning_state((*j).get_string("provisioning_state"));
        reply->set_boot_id((*j).get_string("boot_id"));
        reply->set_boot_count(uint32_t((*j).get_int("boot_count")));
        reply->set_clean_shutdown((*j).get_bool("clean_shutdown"));
        return grpc::Status::OK;
    }

    grpc::Status GetConfig(grpc::ServerContext*, const istate::GetConfigRequest* req, istate::ConfigSnapshot* reply) override
    {
        auto doc = core_.config_service().get(req->daemon());
        if (!doc.ok()) return grpc::Status(grpc::StatusCode::NOT_FOUND, doc.error.str());
        reply->set_daemon(req->daemon());
        reply->set_yaml((*doc).to_yaml());
        for (const auto& kv : (*doc).origins()) (*reply->mutable_origins())[kv.first] = origin_name(kv.second);
        reply->set_schema_version(core_.pst().schema_version());
        return grpc::Status::OK;
    }

    /// @brief Map a proto origin enum to the domain enum.
    /// @param o Proto origin.
    /// @return The domain origin.
    static instr::Origin origin_from_proto(common::Origin o)
    {
        switch (o)
        {
        case common::ORIGIN_DEFAULT: return instr::Origin::Default;
        case common::ORIGIN_APP: return instr::Origin::App;
        case common::ORIGIN_LOCAL: return instr::Origin::Local;
        case common::ORIGIN_HQ: return instr::Origin::Hq;
        // protobuf emits INT_MIN/INT_MAX sentinels into every enum; they are
        // never valid values on the wire.
        default: break;
        }
        return instr::Origin::Default;
    }

    /// @brief Configuration API: merge values by origin rules.
    /// @param req Set request (daemon, YAML, origin, force).
    /// @param reply Receives rejected paths and the error text.
    /// @return gRPC status.
    grpc::Status SetConfig(grpc::ServerContext*, const istate::SetConfigRequest* req, istate::SetConfigReply* reply) override
    {
        auto r = core_.config_service().set(req->daemon(), req->yaml(), origin_from_proto(req->origin()), req->force());
        reply->set_ok(r.ok);
        if (!r.ok) reply->set_error(r.error);
        for (const auto& p : r.rejected) reply->add_rejected_paths(p);
        if (r.ok)
        {
            core_.audit().append(req->actor(), "configuration-ui", "config_set", "{\"daemon\":\"" + req->daemon() + "\"}", core_.clock());
            core_.audit().persist();
            core_.timeline().append("config_change", req->actor(), "{\"daemon\":\"" + req->daemon() + "\"}", core_.clock());
        }
        return grpc::Status::OK;
    }

    /// @brief Configuration API: list managed spokes and their state.
    /// @param reply Receives the spoke states.
    /// @return gRPC status.
    grpc::Status ListSpokes(grpc::ServerContext*, const istate::ListSpokesRequest*, istate::SpokeStatusList* reply) override
    {
        auto j = json_parse(core_.orchestrator().status_json());
        if (!j.ok()) return grpc::Status(grpc::StatusCode::INTERNAL, j.error.message);
        for (const auto& item : (*j).items())
        {
            auto* s = reply->add_spokes();
            s->set_name(item.get_string("name"));
            s->set_state(item.get_string("state"));
            s->set_live(item.get_bool("live"));
            s->set_build_version(item.get_string("build_version"));
            s->set_proto_version(uint32_t(item.get_int("proto_version")));
        }
        return grpc::Status::OK;
    }

    /// @brief Configuration API: start/stop/restart a spoke unit.
    /// @param req Control request (unit, action).
    /// @param reply Receives the outcome ack.
    /// @return gRPC status.
    grpc::Status SpokeControl(grpc::ServerContext*, const istate::SpokeControlRequest* req, common::Ack* reply) override
    {
        auto info = core_.orchestrator().spoke(req->daemon());
        if (!info.ok()) return grpc::Status(grpc::StatusCode::NOT_FOUND, info.error.str());
        const std::string& action = req->action();
        if (action == "start")
            core_.platform().unit_start((*info)->unit);
        else if (action == "stop")
            core_.platform().unit_stop((*info)->unit);
        else if (action == "restart")
            core_.platform().unit_restart((*info)->unit);
        else
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "action must be start|stop|restart");
        reply->set_ok(true);
        core_.timeline().append("restart", req->action(), "{\"daemon\":\"" + req->daemon() + "\"}", core_.clock());
        return grpc::Status::OK;
    }

    /// @brief Configuration API: system or factory reset (explicitly gated).
    /// @param req Reset request (kind, confirmation).
    /// @param reply Receives the outcome ack.
    /// @return gRPC status.
    grpc::Status Reset(grpc::ServerContext*, const istate::ResetRequest* req, common::Ack* reply) override
    {
        Result<bool> r = req->factory() ? core_.factory_reset(req->actor(), req->confirmation()) : core_.system_reset(req->actor());
        reply->set_ok(r.ok());
        if (!r.ok()) reply->set_message(r.error.str());
        return grpc::Status::OK;
    }

    /// @brief Configuration API: stream timeline events matching @p req.
    /// @param req Event filter (since sequence).
    /// @param writer Stream of timeline events.
    /// @return gRPC status when the stream ends.
    grpc::Status StreamEvents(grpc::ServerContext* /*ctx*/, const istate::EventFilter* req, grpc::ServerWriter<istate::TimelineEvent>* writer) override
    {
        // Ordered per-device event stream (§7.1).
        for (const auto& ev : core_.timeline().events())
        {
            if (ev.seq <= req->since_seq()) continue;
            if (!req->kinds().empty())
            {
                bool match = false;
                for (const auto& k : req->kinds()) match |= (k == ev.kind);
                if (!match) continue;
            }
            istate::TimelineEvent out;
            out.set_seq(ev.seq);
            out.set_kind(ev.kind);
            out.set_actor(ev.actor);
            out.set_detail_json(ev.detail_json);
            transport::set_ts(out.mutable_ts(), ev.ts);
            if (!writer->Write(out)) break;
        }
        return grpc::Status::OK;
    }

    /// @brief Configuration API: live log tail to the peer.
    /// @param writer Stream of log lines.
    /// @return gRPC status when the stream ends.
    grpc::Status StreamLogs(grpc::ServerContext*, const istate::TailSpec*, grpc::ServerWriter<logmanager::TailLine>* writer) override
    {
        // Fans out the live tail (subscribed via TailFanout; this stream receives
        // lines while the RPC is open).
        int sub = core_.tail_fanout().subscribe([&writer](const std::string& json_line) {
            auto j = json_parse(json_line);
            if (!j.ok()) return;
            logmanager::TailLine line;
            line.set_source((*j).get_string("source"));
            line.set_message((*j).get_string("message"));
            line.mutable_ts()->set_mono_ns(uint64_t((*j).get_number("mono_ns")));
            writer->Write(line); // best-effort; closes on client disconnect
        });
        // Keep the stream open until the client goes away.
        // Simple bounded hold: the fanout delivers via Write above.
        while (true)
        {
            struct timespec ts
            {
                0, 200 * 1000 * 1000
            };
            nanosleep(&ts, nullptr);
            if (!writer->Write(logmanager::TailLine{})) break; // heartbeat / disconnect check
        }
        core_.tail_fanout().unsubscribe(sub);
        return grpc::Status::OK;
    }

    /// @brief Configuration API: last computed health score.
    /// @param reply Receives the score and its factors.
    /// @return gRPC status.
    grpc::Status GetHealthScore(grpc::ServerContext*, const istate::HealthScoreRequest*, istate::HealthScoreReply* reply) override
    {
        // Health comes from the telemetry spoke's last computed score via relay.
        auto fut = core_.relay_command("telemetry", []() {
            common::Command c;
            c.set_op("health-score");
            c.set_json_args("{}");
            c.set_issued_by("istate");
            return c;
        }());
        if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) { return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "telemetry spoke not responsive"); }
        auto j = json_parse(fut.get());
        if (!j.ok()) return grpc::Status(grpc::StatusCode::INTERNAL, j.error.message);
        if (const JValue* d = (*j).find("data"))
        {
            reply->set_score(d->find("score") ? d->find("score")->as_number() : 0);
            if (const JValue* f2 = d->find("factors"))
                for (const auto& kv : f2->members()) (*reply->mutable_factors())[kv.first] = kv.second.as_number();
        }
        return grpc::Status::OK;
    }

    /// @brief Configuration API: provisioning/enrollment state.
    /// @param reply Receives the state.
    /// @return gRPC status.
    grpc::Status GetEnrollmentStatus(grpc::ServerContext*, const istate::EnrollmentRequest*, istate::EnrollmentStatus* reply) override
    {
        reply->set_state(core_.provisioner().state_name(core_.provisioner().state()));
        reply->set_hostname(core_.provisioner().identity().hostname);
        reply->set_pubkey_b64(core_.provisioner().identity().pubkey_b64());
        auto tun = core_.pst().read_protected("system/networkd/wg-peer.yaml");
        reply->set_tunnel_up(core_.platform().hq_reachable());
        if (tun.ok())
        {
            auto j = json_parse(*tun);
            if (j.ok()) reply->set_hq_endpoint((*j).get_string("hq_endpoint"));
        }
        return grpc::Status::OK;
    }

    /// @brief Configuration API: rotate the tunnel key and re-enroll.
    /// @param req Rotate request.
    /// @param reply Receives the outcome ack.
    /// @return gRPC status.
    grpc::Status RotateKey(grpc::ServerContext*, const istate::RotateKeyRequest* req, common::Ack* reply) override
    {
        // Key rotation without a site visit (§6.2).
        auto r = core_.provisioner().rotate_key([](const DeviceIdentity&) {
            // HQ re-enrollment over the tunnel; simulated wiring in tests.
            TunnelConfig t;
            t.valid = true;
            return Result<TunnelConfig>::good(t);
        });
        reply->set_ok(r.ok());
        if (!r.ok())
            reply->set_message(r.error.str());
        else
        {
            core_.audit().append(req->actor(), "hq", "key_rotation", "{}", core_.clock());
            core_.audit().persist();
        }
        return grpc::Status::OK;
    }

    /// @brief Configuration API: relay a command to a spoke, correlated to its ack.
    /// @param req Relay request (daemon, op, args).
    /// @param reply Receives the spoke's ack.
    /// @return gRPC status.
    grpc::Status RelayCommand(grpc::ServerContext*, const istate::RelayRequest* req, common::Ack* reply) override
    {
        // Full-coverage pass-through to a spoke (Configuration → spoke via istate).
        auto fut = core_.relay_command(req->daemon(), req->command());
        if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "spoke " + req->daemon() + " not responsive");
        auto j = json_parse(fut.get());
        if (!j.ok()) return grpc::Status(grpc::StatusCode::INTERNAL, j.error.message);
        reply->set_id(req->command().id());
        if (const JValue* ok = (*j).find("ok")) reply->set_ok(ok->as_bool());
        if (const JValue* e = (*j).find("error"))
        {
            if (const JValue* c = e->find("code")) reply->set_code(c->as_string());
            if (const JValue* m = e->find("message")) reply->set_message(m->as_string());
        }
        if (const JValue* d = (*j).find("data")) reply->set_json_payload(d->dump());
        return grpc::Status::OK;
    }

    /// @brief Configuration API: per-consumer budget usage.
    /// @param reply Receives the budget report.
    /// @return gRPC status.
    grpc::Status Budgets(grpc::ServerContext*, const istate::BudgetsRequest*, istate::BudgetsReply* reply) override
    {
        auto j = json_parse(core_.budgets_json());
        if (!j.ok()) return grpc::Status(grpc::StatusCode::INTERNAL, j.error.message);
        for (const auto& item : (*j).items())
        {
            auto* b = reply->add_budgets();
            b->set_consumer(item.get_string("consumer"));
            b->set_limit_bytes(uint64_t(item.get_number("limit_bytes")));
            b->set_used_bytes(uint64_t(item.get_number("used_bytes")));
            b->set_warning(item.get_bool("warning"));
        }
        return grpc::Status::OK;
    }

  private:
    IstateCore& core_;
};

// ---- Application-facing API (R5, §7.6, §7.8, §6.2) ----
//
// Colocated applications reach istate and nothing else, exactly as a spoke
// and the Configuration UI do (R1, R2). Alarm and metric calls are relayed
// to their spoke through the same hub relay everything else uses, and config
// writes go through ConfigService, so /pst stays istate's alone (R4).

// Expands a dotted config path into nested YAML: "a.b.c" + "7" becomes
// "a:\n  b:\n    c: 7". ConfigService::set takes a YAML document, and an
// application sets one key at a time.
/// @brief Convert an application's dotted key into a nested YAML snippet.
/// @param key Dotted key ("a.b.c").
/// @param value Scalar value.
/// @return The nested YAML snippet.
static std::string dotted_key_to_yaml(const std::string& key, const std::string& value)
{
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= key.size())
    {
        size_t dot = key.find('.', pos);
        if (dot == std::string::npos)
        {
            parts.push_back(key.substr(pos));
            break;
        }
        parts.push_back(key.substr(pos, dot - pos));
        pos = dot + 1;
    }
    std::string out;
    for (size_t i = 0; i + 1 < parts.size(); ++i) out += std::string(i * 2, ' ') + parts[i] + ":\n";
    out += std::string((parts.size() - 1) * 2, ' ') + parts.back() + ": " + value + "\n";
    return out;
}

class ApplicationApiImpl final : public istate::ApplicationApi::Service
{
  public:
    /// @brief Construct the application API service over the core (R5, §7.6, §7.8).
    /// @param core istate core.
    explicit ApplicationApiImpl(IstateCore& core) : core_(core)
    {
    }

    grpc::Status GetSystemInfo(grpc::ServerContext*, const istate::SystemInfoRequest*, istate::SystemInfo* reply) override
    {
        auto j = json_parse(core_.system_info_json());
        if (!j.ok()) return grpc::Status(grpc::StatusCode::INTERNAL, j.error.message);
        reply->set_hostname((*j).get_string("hostname"));
        reply->set_serial((*j).get_string("serial"));
        reply->set_hw_revision((*j).get_string("hw_revision"));
        reply->set_board_variant((*j).get_string("board_variant"));
        reply->set_firmware_version((*j).get_string("firmware_version"));
        reply->set_suite_version((*j).get_string("suite_version"));
        reply->set_config_schema_version((*j).get_string("config_schema_version"));
        reply->set_istate_state((*j).get_string("istate_state"));
        reply->set_provisioning_state((*j).get_string("provisioning_state"));
        reply->set_boot_id((*j).get_string("boot_id"));
        reply->set_boot_count(uint32_t((*j).get_int("boot_count")));
        reply->set_clean_shutdown((*j).get_bool("clean_shutdown"));
        return grpc::Status::OK;
    }

    grpc::Status GetConfig(grpc::ServerContext*, const istate::GetConfigRequest* req, istate::ConfigSnapshot* reply) override
    {
        auto doc = core_.config_service().get(req->daemon());
        if (!doc.ok()) return grpc::Status(grpc::StatusCode::NOT_FOUND, doc.error.str());
        reply->set_daemon(req->daemon());
        reply->set_yaml((*doc).to_yaml());
        for (const auto& kv : (*doc).origins()) (*reply->mutable_origins())[kv.first] = origin_name(kv.second);
        reply->set_schema_version(core_.pst().schema_version());
        return grpc::Status::OK;
    }

    /// @brief Application API: write config with origin=App (§6.2, attributed).
    /// @param req Set request (app name, dotted keys).
    /// @param reply Receives rejected paths and the error text.
    /// @return gRPC status.
    grpc::Status SetAppConfig(grpc::ServerContext*, const istate::SetAppConfigRequest* req, istate::SetConfigReply* reply) override
    {
        if (req->app().empty()) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "app is required");
        if (req->key().empty()) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "key is required");
        // Origin is forced to App and force is false: an application may add its
        // own keys but must never overwrite an operator's local edit (§4, §9).
        auto r = core_.config_service().set(req->daemon(), dotted_key_to_yaml(req->key(), req->value()), instr::Origin::App, /*force=*/false);
        reply->set_ok(r.ok);
        if (!r.ok) reply->set_error(r.error);
        for (const auto& p : r.rejected) reply->add_rejected_paths(p);
        if (r.ok)
        {
            const std::string detail = "{\"daemon\":\"" + req->daemon() + "\",\"key\":\"" + req->key() + "\"}";
            core_.audit().append(req->app(), "application", "config_set", detail, core_.clock());
            core_.audit().persist();
            core_.timeline().append("config_change", req->app(), detail, core_.clock());
        }
        return grpc::Status::OK;
    }

    /// @brief Application API: raise an alarm owned by the application (R5).
    /// @param req Raise request (owner, id, text).
    /// @param reply Receives the outcome.
    /// @return gRPC status.
    grpc::Status RaiseAlarm(grpc::ServerContext*, const istate::RaiseAlarmRequest* req, istate::AlarmAck* reply) override
    {
        if (req->owner().empty()) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "owner is required: an alarm is declared by an application (R5)");
        JValue args = JValue::object();
        args.set("id", JValue(double(req->alarm_id())));
        args.set("owner", JValue(req->owner()));
        args.set("text", JValue(req->additional_text()));
        std::string corr;
        for (int i = 0; i < req->correlated_size(); ++i)
        {
            if (!corr.empty()) corr += ",";
            corr += std::to_string(req->correlated(i));
        }
        if (!corr.empty()) args.set("correlated", JValue(corr));
        return relay_alarm("raise", args, req->owner(), req->alarm_id(), reply);
    }

    /// @brief Application API: clear an alarm it owns.
    /// @param req Clear request (owner, id).
    /// @param reply Receives the outcome.
    /// @return gRPC status.
    grpc::Status ClearAlarm(grpc::ServerContext*, const istate::ClearAlarmRequest* req, istate::AlarmAck* reply) override
    {
        if (req->owner().empty()) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "owner is required (R5)");
        JValue args = JValue::object();
        args.set("id", JValue(double(req->alarm_id())));
        args.set("owner", JValue(req->owner()));
        return relay_alarm("clear", args, req->owner(), req->alarm_id(), reply);
    }

    /// @brief Application API: register a metric with Telemetry (§7.8).
    /// @param req Registration request (name, unit, type, app).
    /// @param reply Receives the outcome ack.
    /// @return gRPC status.
    grpc::Status RegisterMetric(grpc::ServerContext*, const istate::RegisterMetricRequest* req, common::Ack* reply) override
    {
        if (req->app().empty() || req->name().empty()) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "app and name are required");
        JValue args = JValue::object();
        args.set("name", JValue(req->name()));
        args.set("unit", JValue(req->unit()));
        args.set("type", JValue(req->type()));
        args.set("app", JValue(req->app()));
        auto r = relay("telemetry", "register-metric", args);
        if (!r.first) return grpc::Status(grpc::StatusCode::UNAVAILABLE, r.second);
        reply->set_ok(true);
        return grpc::Status::OK;
    }

    /// @brief Application API: report metric samples.
    /// @param req Samples batched by the application.
    /// @param reply Receives per-sample outcomes.
    /// @return gRPC status.
    grpc::Status ReportMetrics(grpc::ServerContext*, const istate::ReportMetricsRequest* req, istate::ReportMetricsReply* reply) override
    {
        uint32_t accepted = 0, rejected = 0;
        std::string last_error;
        for (const auto& s : req->samples())
        {
            if (s.name().empty())
            {
                ++rejected;
                last_error = "sample with empty metric name";
                continue;
            }
            JValue args = JValue::object();
            args.set("metric", JValue(s.name()));
            args.set("value", JValue(s.value()));
            auto r = relay("telemetry", "ingest", args);
            if (r.first) { ++accepted; }
            else
            {
                ++rejected;
                last_error = r.second;
            }
        }
        reply->set_accepted(accepted);
        reply->set_rejected(rejected);
        if (rejected) reply->set_error(last_error);
        return grpc::Status::OK;
    }

  private:
    // Relays one command to a spoke and reports whether it succeeded. Returns
    // {ok, message}; a spoke that is down is Unavailable, never a hang (R10).
    /// @brief Relay a command to a spoke and wait for its ack.
    /// @param daemon Spoke to relay to.
    /// @param op Operation name.
    /// @param args Parsed argument object.
    /// @param data_json JSON payload relayed with the command.
    /// @return (delivered, ack JSON or error).
    std::pair<bool, std::string> relay(const std::string& daemon, const std::string& op, const JValue& args, std::string* data_json = nullptr)
    {
        common::Command cmd;
        cmd.set_op(op);
        cmd.set_json_args(args.dump());
        cmd.set_issued_by("application");
        auto fut = core_.relay_command(daemon, cmd);
        if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return {false, daemon + " not responsive"};
        auto j = json_parse(fut.get());
        if (!j.ok()) return {false, j.error.message};
        bool ok = false;
        if (const JValue* v = (*j).find("ok")) ok = v->as_bool();
        if (ok)
        {
            if (data_json)
                if (const JValue* d = (*j).find("data")) *data_json = d->dump();
            return {true, ""};
        }
        std::string msg = daemon + " rejected " + op;
        if (const JValue* e = (*j).find("error"))
            if (const JValue* m = e->find("message")) msg = m->as_string();
        return {false, msg};
    }

    /// @brief Relay an alarm raise/clear through the Alarms daemon on behalf of an application.
    /// @param op Operation name (raise/clear).
    /// @param args Parsed argument object.
    /// @param owner Alarm owner.
    /// @param alarm_id Packed alarm id.
    /// @param reply Receives the alarm-operation outcome.
    /// @return gRPC status reflecting the outcome.
    grpc::Status relay_alarm(const std::string& op, const JValue& args, const std::string& owner, uint32_t alarm_id, istate::AlarmAck* reply)
    {
        std::string data;
        auto r = relay("alarms", op, args, &data);
        reply->set_accepted(r.first);
        if (!r.first)
        {
            reply->set_error(r.second);
            // A declared alarm that cannot reach the alarms daemon is not silently
            // lost: it is a reportable condition, and the caller is told.
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, r.second);
        }
        auto j = json_parse(data);
        if (j.ok())
        {
            if (const JValue* a = (*j).find("action")) reply->set_action(a->as_string());
            if (const JValue* n = (*j).find("needs_snapshot")) reply->set_needs_snapshot(n->as_bool());
        }
        core_.timeline().append("alarm", owner, "{\"id\":" + std::to_string(alarm_id) + ",\"op\":\"" + op + "\"}", core_.clock());
        return grpc::Status::OK;
    }

    IstateCore& core_;
};

// ---- server builder ----

/// @brief Build and start the gRPC server hosting every hub service.
/// @param core istate core the hubs ingest into.
/// @param run_dir Directory holding the sockets.
/// @param services Receives the built hub services.
/// @return The started server, or nullptr when startup failed.
std::unique_ptr<grpc::Server> build_hub_server(IstateCore& core, const std::string& run_dir, std::vector<std::unique_ptr<grpc::Service>>* services)
{
    grpc::ServerBuilder builder;
    // Ownership can only be applied once the socket file exists, which is
    // after BuildAndStart — applying it here (as this once did) chmod'd a
    // path that was not there yet and silently achieved nothing.
    std::vector<std::string> socket_names;
    auto own = [&](const std::string& name) {
        std::string path = run_dir + "/" + name + ".sock";
        builder.AddListeningPort("unix:" + path, grpc::InsecureServerCredentials());
        socket_names.push_back(name);
        return path;
    };
    auto monitor = std::make_unique<MonitorHubImpl>(core, "monitord");
    auto network = std::make_unique<NetworkHubImpl>(core, "networkd");
    auto hardware = std::make_unique<HardwareHubImpl>(core, "hardware");
    auto faults = std::make_unique<FaultHubImpl>(core, "faults");
    auto alarms = std::make_unique<AlarmHubImpl>(core, "alarms");
    auto logs = std::make_unique<LogHubImpl>(core, "logmanager");
    auto telemetry = std::make_unique<TelemetryHubImpl>(core, "telemetry");
    auto ota = std::make_unique<OtaHubImpl>(core, "ota");
    auto backup = std::make_unique<BackupHubImpl>(core, "backup");
    auto config = std::make_unique<ConfigurationApiImpl>(core);
    auto application = std::make_unique<ApplicationApiImpl>(core);

    // Hub-bound routing (R1: everything reaches a spoke through istate). This
    // is what carries config deliveries, liveness pings, and relayed data such
    // as monitord samples on their way to telemetry. Without it the core holds
    // a null relay and every one of those silently does nothing.
    std::map<std::string, std::function<bool(const GenericHubMsg&)>> routes;
    auto route = [&routes](const std::string& name, auto* impl) {
        routes[name] = [impl](const GenericHubMsg& m) {
            if (!impl->spoke_registered()) return false; // no session to push into
            impl->push(m);
            return true;
        };
    };
    route("monitord", monitor.get());
    route("networkd", network.get());
    route("hardware", hardware.get());
    route("faults", faults.get());
    route("alarms", alarms.get());
    route("logmanager", logs.get());
    route("telemetry", telemetry.get());
    route("ota", ota.get());
    route("backup", backup.get());
    core.set_relay_fn([routes](const std::string& daemon, const GenericHubMsg& m) {
        auto it = routes.find(daemon);
        return it != routes.end() && it->second(m);
    });

    own("monitord");
    builder.RegisterService(monitor.get());
    own("networkd");
    builder.RegisterService(network.get());
    own("hardware");
    builder.RegisterService(hardware.get());
    own("faults");
    builder.RegisterService(faults.get());
    own("alarms");
    builder.RegisterService(alarms.get());
    own("logmanager");
    builder.RegisterService(logs.get());
    own("telemetry");
    builder.RegisterService(telemetry.get());
    own("ota");
    builder.RegisterService(ota.get());
    own("backup");
    builder.RegisterService(backup.get());
    own("configuration");
    builder.RegisterService(config.get());
    own("application");
    builder.RegisterService(application.get());

    if (services)
    {
        services->push_back(std::move(monitor));
        services->push_back(std::move(network));
        services->push_back(std::move(hardware));
        services->push_back(std::move(faults));
        services->push_back(std::move(alarms));
        services->push_back(std::move(logs));
        services->push_back(std::move(telemetry));
        services->push_back(std::move(ota));
        services->push_back(std::move(backup));
        services->push_back(std::move(config));
        services->push_back(std::move(application));
    }
    auto server = builder.BuildAndStart();
    if (server)
    {
        size_t owned = 0;
        for (const auto& name : socket_names)
            if (own_socket(run_dir + "/" + name + ".sock", name, &core.log())) ++owned;
        if (owned != socket_names.size()) core.log().warning("istate", "socket ownership applied to " + std::to_string(owned) + " of " + std::to_string(socket_names.size()) + " sockets; per-daemon access control is degraded");
    }
    return server;
}

// ---- HQ session client ----

/// @brief Construct the HQ client over the tunnel target.
/// @param core istate core.
/// @param log Logger.
/// @param hq_target HQ tunnel target.
HqClient::HqClient(IstateCore& core, Logger& log, const std::string& hq_target) : core_(core), log_(log), target_(hq_target)
{
}

/// @brief Start the session loop.
void HqClient::start()
{
    running_ = true;
    thread_ = std::thread([this] { run_loop(); });
}

/// @brief Stop the session loop.
void HqClient::stop()
{
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

/// @brief Session loop with jittered backoff between attempts.
void HqClient::run_loop()
{
    // The HQ session is last in the startup order and never a precondition
    // for local operation (§7.1). Jittered exponential backoff on reconnect
    // (§3.4/§11); fleet-wide simultaneous boot does not stampede HQ.
    while (running_.load())
    {
        if (try_session()) { backoff_.reset(); }
        else
        {
            double d = backoff_.next();
            log_.info("istate", "HQ session retry in " + std::to_string(d) + "s");
            double slept = 0;
            while (slept < d && running_.load())
            {
                struct timespec ts
                {
                    0, 100 * 1000 * 1000
                };
                nanosleep(&ts, nullptr);
                slept += 0.1;
            }
        }
    }
}

/// @brief One HQ session: stream spooled data, receive pushes and commands.
bool HqClient::try_session()
{
    auto channel = grpc::CreateCustomChannel(target_, grpc::InsecureChannelCredentials(), grpc::ChannelArguments());
    auto stub = istate::HqPeer::NewStub(channel);
    grpc::ClientContext ctx;
    auto rw = stub->Session(&ctx);
    connected_ = true;
    core_.hq_session().on_connected();
    log_.info("istate", "HQ session established");

    // Reader thread: HQ frames (config push / OTA command / backpressure).
    std::atomic<bool> open{true};
    // Reader thread: drains HQ replies so the writer never blocks on a full stream buffer.
    std::thread reader([this, &rw, &open] {
        istate::HqFrame hf;
        while (open.load() && rw->Read(&hf))
        {
            if (hf.has_backpressure()) { core_.set_hq_backpressure(hf.backpressure().pause()); }
            else if (hf.has_config_push())
            {
                // Signed config push verified on device (§9).
                auto r = core_.config_service().apply_signed(hf.config_push().daemon(), hf.config_push().yaml(), hf.config_push().key_id(), hf.config_push().signature(), crypto::hex_encode(hf.config_push().hmac()), core_.enrollment_secret());
                if (r.ok())
                {
                    log_.info("istate", "signed config push applied for " + hf.config_push().daemon());
                    if (core_.config_service().last_push_used_legacy_hmac())
                        log_.warning("istate", "config push verified by shared secret, not signature: provision an "
                                               "HQ signing key on this device");
                    // The spoke runs on what /pst now holds (R4).
                    core_.deliver_config(hf.config_push().daemon());
                }
                else
                {
                    log_.error("istate", "signed config push REFUSED: " + r.error.message);
                    core_.push_event_to_spool("{\"kind\":\"config_push_refused\",\"daemon\":\"" + hf.config_push().daemon() + "\"}");
                }
            }
            else if (hf.has_ota_command() || hf.has_command())
            {
                const common::Command& cmd = hf.has_ota_command() ? hf.ota_command() : hf.command();
                std::string daemon = hf.has_ota_command() ? "ota" : cmd.issued_by();
                core_.relay_command(daemon.empty() ? "ota" : daemon, cmd);
            }
        }
        open = false;
    });

    // Writer: drain the northbound spool while connected (with pacing so a
    // fleet booting together honours backpressure, §11).
    while (open.load() && running_.load())
    {
        if (core_.hq_backpressured())
        {
            struct timespec ts
            {
                0, 200 * 1000 * 1000
            };
            nanosleep(&ts, nullptr);
            continue;
        }
        auto rep = core_.drain_northbound(50);
        if (rep.sent == 0 && !rep.paused)
        {
            struct timespec ts
            {
                0, 500 * 1000 * 1000
            };
            nanosleep(&ts, nullptr);
        }
        if (core_.hq_session().state() == istate::HqSession::State::Backpressured) continue;
    }
    open = false;
    reader.join();
    connected_ = false;
    core_.hq_session().on_disconnected();
    return true;
}

} // namespace istate
} // namespace instr