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
 * @file daemons/istate/src/core.cpp
 * @brief istate hub core implementation (REQ §7.1): composition of provisioning, configuration, orchestration, northbound paths, and the external interface (R2: the only process with an external interface).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/istate/core.h"

#include "instr/alarms_gen.h"

#include <future>

#include "instr/common/version_gen.h"

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

/// @brief Construct the core and all subsystems.
IstateCore::IstateCore(IstateOptions opts, IFileSystem& fs, platform::IPlatform& plat, TimeSource& ts, Logger& log)
    : opts_(std::move(opts)), fs_(fs), plat_(plat), ts_(ts), log_(log), budgets_(fs, opts_.pst_root), pst_(fs, opts_.pst_root, budgets_), provisioner_(pst_, plat, fs), orchestrator_(plat, ts), timeline_(5000), northbound_spool_(
                                                                                                                                                                                                                      fs,
                                                                                                                                                                                                                      [&] {
                                                                                                                                                                                                                          SpoolConfig c;
                                                                                                                                                                                                                          c.dir = opts_.pst_root + "/state/spool";
                                                                                                                                                                                                                          c.max_bytes = opts_.spool_max_bytes;
                                                                                                                                                                                                                          c.segment_bytes = 4 * 1024 * 1024;
                                                                                                                                                                                                                          return c;
                                                                                                                                                                                                                      }(),
                                                                                                                                                                                                                      &budgets_),
      config_service_(pst_, fs), audit_(fs, opts_.pst_root + "/state/audit.log", 2000), reconciler_(ts)
{
    budgets_.set_budget("spool", uint64_t(opts_.budget_spool));
    budgets_.set_budget("logs", uint64_t(opts_.budget_logs));
    budgets_.set_budget("ota", uint64_t(opts_.budget_ota));
    budgets_.set_budget("backup", uint64_t(opts_.budget_backup));
}

/// @brief Mount, provision, seed and start the fleet in dependency order.
/// @param default_yaml Image default YAML per daemon.
/// @return True when the platform is usable; false only when unusable.
bool IstateCore::boot(const std::map<std::string, std::string>& default_yaml)
{
    // 1. Mount and validate /pst; corrupt → redundant copy; both fail → boot
    // unprovisioned and report, never fail to boot (§11).
    auto mount = pst_.mount();
    if (!mount.corrupt.empty())
    {
        log_.critical("istate", "/pst unrecoverable files: booting unprovisioned", {{"files", std::to_string(mount.corrupt.size())}});
        timeline_.append("boot", "istate", "{\"pst_corrupt\":true}", ts_);
        pending_alarms_.push_back({INSTR_ALARM_ISTATE_PST_CORRUPT, std::to_string(mount.corrupt.size()) + " /pst file(s) failed both copies"});
    }
    // Unclean shutdown detection via the clean-shutdown marker (§11). The
    // marker lives under /pst/state — /run is tmpfs and would be empty on
    // every boot, which would report every boot as unclean.
    observed_clean_shutdown_ = fs_.exists(clean_shutdown_marker_path());
    if (!observed_clean_shutdown_)
    {
        log_.warning("istate", "previous shutdown was not clean");
        timeline_.append("boot", "istate", "{\"unclean_shutdown\":true}", ts_);
        pending_alarms_.push_back({INSTR_ALARM_ISTATE_UNCLEAN_SHUTDOWN, "previous shutdown left no clean-shutdown marker"});
        northbound_spool_.enqueue(SpoolPriority::Event, "{\"kind\":\"unclean_shutdown\"}", ts_.mono_ns());
    }
    // Clear it for this run: a crash now leaves no marker for the next boot.
    fs_.remove(clean_shutdown_marker_path(), false);
    // 2. Virgin detection + default seeding.
    if (mount.virgin)
    {
        Error e = pst_.seed_defaults(default_yaml);
        if (!e.ok()) log_.error("istate", "default seeding failed: " + e.str());
        timeline_.append("boot", "istate", "{\"virgin\":true,\"seeded\":true}", ts_);
    }
    else
    {
        // 2b. Config migration on the first boot after an update (§6, §7.9),
        // driven by .schema-version. A release that adds keys must not leave a
        // device running without them, and must not overwrite operator edits
        // while adding them — merge_release adds only what is missing.
        const std::string on_disk = pst_.schema_version();
        if (on_disk != kConfigSchemaVersion)
        {
            auto added = pst_.migrate(default_yaml);
            Error e = pst_.set_schema_version(kConfigSchemaVersion);
            if (!e.ok()) log_.error("istate", "schema version write failed: " + e.str());
            log_.info("istate", "config migrated " + on_disk + " → " + kConfigSchemaVersion + ": " + std::to_string(added.size()) + " new keys");
            JValue ev = JValue::object();
            ev.set("from", JValue(on_disk));
            ev.set("to", JValue(std::string(kConfigSchemaVersion)));
            JValue keys = JValue::array();
            for (const auto& a : added) keys.push(JValue(a));
            ev.set("added_keys", keys);
            timeline_.append("config", "istate", ev.dump(), ts_);
            northbound_spool_.enqueue(SpoolPriority::Event, ev.dump(), ts_.mono_ns());
            audit_.append("istate", "update", "config_migration", ev.dump(), ts_);
        }
    }

    // 3. Load persisted schemas for config validation (registered by wiring).
    // 4. Provisioning state machine (§6.2).
    auto enrolled = provisioner_.run_first_boot([this](const DeviceIdentity& id) {
        // HQ enrollment round-trip: over the tunnel when available; without the
        // tunnel the device stays operational and enrollment retries (R10).
        if (hq_send_)
        {
            JValue req = JValue::object();
            req.set("identity", JValue(id.hostname));
            req.set("pubkey", JValue(id.pubkey_b64()));
            req.set("factory_sig", JValue(id.factory_sig));
            // Synchronous enrollment via the HQ session is wired in production; the
            // simulated HQ responds through the same callback.
            if (hq_send_("enroll", req.dump()))
            {
                TunnelConfig t;
                t.hq_endpoint = "hq:51820";
                t.valid = true;
                return Result<TunnelConfig>::good(t);
            }
            return Result<TunnelConfig>::fail(Errc::Unavailable, "HQ unreachable for enrollment", "enroll");
        }
        return Result<TunnelConfig>::fail(Errc::Unavailable, "no HQ transport wired", "enroll");
    });
    if (enrolled.ok())
        log_.info("istate", "provisioning state: " + std::string(provisioner_.state_name(provisioner_.state())));
    else
        log_.warning("istate", "enrollment deferred: " + enrolled.error.str());

    // 5. Orchestrate the spokes (observation daemons start first, §7.1).
    // Liveness is a ping over each spoke's own channel; the answer arrives as
    // a pong in on_spoke_msg.
    orchestrator_.set_ping_fn([this](const std::string& daemon, uint64_t nonce) {
        if (!relay_to_spoke_) return false;
        transport::GenericHubMsg m;
        m.has_command = true;
        m.command.set_op("liveness-ping");
        m.command.set_json_args("{\"nonce\":" + std::to_string(nonce) + "}");
        m.command.set_issued_by("istate");
        return relay_to_spoke_(daemon, m);
    });
    orchestrator_.declare_spokes(startup_order());
    orchestrator_.start_all();

    // 6. Open the northbound spool; HQ session is last and never a precondition.
    Error se = northbound_spool_.open();
    if (!se.ok()) log_.error("istate", "northbound spool failed: " + se.str());

    // 7. Watchdog: pet while all must-live daemons are live.
    orchestrator_.set_watchdog_required({"faults", "monitord", "hardware", "networkd"});
    (void)plat_.watchdog_set_timeout_s(60);

    // Alarms observed during boot are raised now that the relay exists. They
    // are queued rather than dropped: the Alarms daemon starts after istate,
    // so a boot-time condition would otherwise be silently lost.
    for (const auto& a : pending_alarms_) raise_alarm(a.first, a.second);
    pending_alarms_.clear();

    audit_.load();
    audit_.append("istate", "boot", "boot", "{\"version\":\"" INSTR_VERSION "\"}", ts_);
    audit_.persist();
    timeline_.append("boot", "istate", "{\"version\":\"" INSTR_VERSION "\"}", ts_);
    return true;
}

/// @brief Stop spokes, persist, write the clean-shutdown marker.
void IstateCore::shutdown()
{
    write_clean_shutdown_marker();
    orchestrator_.stop_all();
}

/// @brief Re-stamp spooled data when a time source acquires lock.
/// @param source Lock source name.
/// @param offset_ns Wall-clock correction in nanoseconds.
/// @return Number of pending records re-stamped.
size_t IstateCore::on_time_lock(const std::string& source, int64_t offset_ns)
{
    time_policy_.locked = true;
    time_policy_.current_source = source;
    // Reconciliation: spooled data, alarms, logs re-stamped; history never
    // reordered (§4).
    size_t n = reconciler_.reconcile(offset_ns, source);
    log_.info("istate", "time lock via " + source + "; re-stamped " + std::to_string(n) + " records");
    return n;
}

/// @brief Route a spoke frame by type (telemetry/faults/alarms/logs/events).
/// @param msg Spoke frame routed by type.
void IstateCore::on_spoke_msg(const transport::GenericSpokeMsg& msg)
{
    // First message from a spoke ⇒ it is alive and attached.
    orchestrator_.note_spoke_activity(msg.daemon);
    if (msg.type == "config_request")
    {
        // R4: the spoke never opens /pst; istate reads and delivers.
        deliver_config(msg.daemon);
        return;
    }
    else if (msg.type == "pong")
    {
        // Liveness answer (§3.1, §7.1): proves the spoke's event loop is turning,
        // not merely that its process exists.
        uint64_t nonce = 0;
        auto j = json_parse(msg.payload_json);
        if (j.ok())
        {
            if (const JValue* v = (*j).find("nonce")) nonce = uint64_t(v->as_number());
        }
        orchestrator_.note_pong(msg.daemon, nonce);
        return;
    }
    else if (msg.type == "config_ack")
    {
        auto j = json_parse(msg.payload_json);
        ConfigAckState st;
        if (j.ok())
        {
            if (const JValue* v = (*j).find("revision")) st.revision = uint64_t(v->as_number());
            if (const JValue* v = (*j).find("applied")) st.applied = v->as_bool();
            if (const JValue* v = (*j).find("reason")) st.reason = v->as_string();
        }
        {
            std::lock_guard<std::mutex> lk(config_mu_);
            config_acks_[msg.daemon] = st;
        }
        if (!st.applied)
        {
            // A spoke that refused its configuration is running on the previous
            // one: say so loudly rather than assuming the push landed (§9).
            log_.error("istate", "config rejected by " + msg.daemon + ": " + st.reason);
            timeline_.append("config", msg.daemon, "{\"config_rejected\":true,\"reason\":\"" + st.reason + "\"}", ts_);
            northbound_spool_.enqueue(SpoolPriority::Event, "{\"kind\":\"config_rejected\",\"daemon\":\"" + msg.daemon + "\"}", ts_.mono_ns());
        }
        return;
    }
    if (msg.type == "sample")
    {
        // monitord → telemetry relay (R1: all traffic through istate).
        transport::GenericHubMsg relay;
        relay.has_relay = true;
        relay.relay.set_source_daemon(msg.daemon);
        relay.relay.set_type("sample");
        relay.relay.set_payload_json(msg.payload_json);
        relay.relay.mutable_ts()->CopyFrom(msg.ts);
        if (relay_to_spoke_) relay_to_spoke_("telemetry", relay);
        // Also spool a compact telemetry record northbound.
        northbound_spool_.enqueue(SpoolPriority::Telemetry, msg.payload_json, msg.ts.mono_ns());
    }
    else if (msg.type == "fault")
    {
        // Faults are protected: never dropped (§11).
        northbound_spool_.enqueue(SpoolPriority::Fault, msg.payload_json, msg.ts.mono_ns());
        timeline_.append("fault", msg.daemon, msg.payload_json, ts_);
    }
    else if (msg.type == "alarm")
    {
        // Alarm state changes go to istate directly as events (§7.6) — protected
        // class, never delayed by batching.
        northbound_spool_.enqueue(SpoolPriority::Alarm, msg.payload_json, msg.ts.mono_ns());
        timeline_.append("alarm", msg.daemon, msg.payload_json, ts_);
        // Snapshot-on-fault requests trigger a bounded diagnostic capture via
        // LogManager (evidence at the moment it matters).
        auto j = json_parse(msg.payload_json);
        if (j.ok())
        {
            if (const JValue* v = (*j).find("needs_snapshot"); v && v->as_bool())
            {
                transport::GenericHubMsg cmd;
                cmd.has_command = true;
                cmd.command.set_op("crash-report");
                cmd.command.set_json_args("{}");
                cmd.command.set_issued_by("istate");
                if (relay_to_spoke_) relay_to_spoke_("logmanager", cmd);
            }
        }
    }
    else if (msg.type == "tail_line")
    {
        // Live tail fanout to HQ and Configuration (§7.1).
        tail_fanout_.publish(msg.payload_json);
        northbound_spool_.enqueue(SpoolPriority::LogRef, msg.payload_json, msg.ts.mono_ns());
    }
    else if (msg.type == "bundle")
    {
        // Bulk transfer by handle: control path carries handle + metadata only.
        northbound_spool_.enqueue(SpoolPriority::LogRef, msg.payload_json, msg.ts.mono_ns());
    }
    else if (msg.type == "ota_event")
    {
        timeline_.append("update", msg.daemon, msg.payload_json, ts_);
        northbound_spool_.enqueue(SpoolPriority::Event, msg.payload_json, msg.ts.mono_ns());
    }
    else if (msg.type == "backup_event")
    {
        timeline_.append("backup", msg.daemon, msg.payload_json, ts_);
        northbound_spool_.enqueue(SpoolPriority::Event, msg.payload_json, msg.ts.mono_ns());
    }
    else if (msg.type == "network_event")
    {
        timeline_.append("network", msg.daemon, msg.payload_json, ts_);
        northbound_spool_.enqueue(SpoolPriority::Event, msg.payload_json, msg.ts.mono_ns());
    }
    else if (msg.type == "health_result") { northbound_spool_.enqueue(SpoolPriority::Telemetry, msg.payload_json, msg.ts.mono_ns()); }
    else if (msg.type == "batch")
    {
        // Telemetry batches: spool northbound (protected of drops: telemetry is
        // the droppable class, §11).
        northbound_spool_.enqueue(SpoolPriority::Telemetry, msg.payload_json, msg.ts.mono_ns());
    }
}

/// @brief Relay a command to a spoke, correlating the ack.
/// @param daemon Spoke to relay to.
/// @param cmd Command to run.
/// @return Resolves with the ack JSON when the spoke answers.
std::future<std::string> IstateCore::relay_command(const std::string& daemon, const common::Command& cmd)
{
    auto pending = std::make_shared<PendingRelay>();
    std::future<std::string> fut = pending->promise.get_future();
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(relay_mu_);
        id = next_relay_id_++;
        pending_relays_[id] = pending;
    }
    common::Command c = cmd;
    c.set_id(id);
    transport::GenericHubMsg m;
    m.has_command = true;
    m.command = c;
    if (relay_to_spoke_ && !relay_to_spoke_(daemon, m))
    {
        std::lock_guard<std::mutex> lk(relay_mu_);
        pending_relays_.erase(id);
        pending->promise.set_value("{\"ok\":false,\"error\":{\"code\":\"unavailable\",\"message\":\"spoke not connected\"}}");
    }
    return fut;
}

/// @brief Complete a pending relay with a spoke's ack.
/// @param daemon Spoke that acked.
/// @param ack Ack frame.
void IstateCore::on_spoke_ack(const std::string& daemon, const common::Ack& ack)
{
    (void)daemon;
    std::shared_ptr<PendingRelay> pending;
    {
        std::lock_guard<std::mutex> lk(relay_mu_);
        auto it = pending_relays_.find(ack.id());
        if (it != pending_relays_.end())
        {
            pending = it->second;
            pending_relays_.erase(it);
        }
    }
    if (pending)
    {
        CommandResult r;
        r.ok = ack.ok();
        r.error_code = ack.code();
        r.error_message = ack.message();
        r.json_data = ack.json_payload();
        pending->promise.set_value(r.envelope());
    }
}

/// @brief Drain the northbound spool toward HQ.
/// @param max_batch Maximum records this drain.
/// @return The drain report.
NorthboundSpool::DrainReport IstateCore::drain_northbound(size_t max_batch)
{
    if (hq_backpressured_.load())
    {
        NorthboundSpool::DrainReport rep;
        rep.paused = true;
        return rep;
    }
    return northbound_spool_.drain(
        [this](const DiskSpool::Entry& e) {
            if (!hq_send_) return false;
            const char* type = "event";
            switch (e.priority)
            {
            case SpoolPriority::Telemetry: type = "telemetry"; break;
            case SpoolPriority::LogRef: type = "log_ref"; break;
            case SpoolPriority::Fault: type = "fault"; break;
            case SpoolPriority::Alarm: type = "alarm"; break;
            case SpoolPriority::Event: type = "event"; break;
            }
            return hq_send_(type, e.payload);
        },
        max_batch);
}

/// @brief Apply or release HQ backpressure.
/// @param pause True to pause; false to resume draining.
void IstateCore::set_hq_backpressure(bool pause)
{
    hq_backpressured_.store(pause);
    if (pause)
    {
        hq_session_.on_backpressure(true);
        log_.warning("istate", "HQ backpressure: northbound drain paused");
    }
    else { hq_session_.on_backpressure(false); }
}

/// @brief Record a registration handshake result.
/// @param req Registration request.
/// @param accepted Whether the handshake was accepted.
void IstateCore::on_spoke_registration(const common::RegistrationRequest& req, bool accepted)
{
    std::vector<std::string> caps(req.capabilities().begin(), req.capabilities().end());
    if (!accepted)
    {
        // The transport already refused it; record and alarm so the condition is
        // visible rather than merely logged (§11 "proto version mismatch").
        log_.critical("istate", "registration refused for " + req.daemon() + " (proto " + std::to_string(req.proto_schema_version()) + " vs " + std::to_string(transport::kProtoSchemaVersion) + ")");
        timeline_.append("registration", req.daemon(), "{\"refused\":true,\"proto_version\":" + std::to_string(req.proto_schema_version()) + "}", ts_);
        raise_alarm(INSTR_ALARM_ISTATE_PROTO_MISMATCH, req.daemon() + " refused: proto schema " + std::to_string(req.proto_schema_version()) + " != " + std::to_string(transport::kProtoSchemaVersion));
        return;
    }
    std::string reason;
    orchestrator_.register_spoke(req.daemon(), req.build_version(), req.proto_schema_version(), caps, &reason);
    timeline_.append("registration", req.daemon(), "{\"accepted\":true,\"build\":\"" + req.build_version() + "\"}", ts_);
}

/// @brief Declare an alarm through the Alarms relay (never writes the table directly).
/// @param alarm_id Packed alarm identity.
/// @param text Additional text.
void IstateCore::raise_alarm(uint32_t alarm_id, const std::string& text)
{
    if (!relay_to_spoke_) return; // alarms not attached yet; nothing to do
    JValue args = JValue::object();
    args.set("id", JValue(double(alarm_id)));
    args.set("owner", JValue(std::string("istate")));
    args.set("text", JValue(text));
    transport::GenericHubMsg m;
    m.has_command = true;
    m.command.set_op("raise");
    m.command.set_json_args(args.dump());
    m.command.set_issued_by("istate");
    (void)relay_to_spoke_("alarms", m);
}

/// @brief Clear an istate-declared alarm through the relay.
/// @param alarm_id Packed alarm identity.
void IstateCore::clear_alarm(uint32_t alarm_id)
{
    if (!relay_to_spoke_) return;
    JValue args = JValue::object();
    args.set("id", JValue(double(alarm_id)));
    args.set("owner", JValue(std::string("istate")));
    transport::GenericHubMsg m;
    m.has_command = true;
    m.command.set_op("clear");
    m.command.set_json_args(args.dump());
    m.command.set_issued_by("istate");
    (void)relay_to_spoke_("alarms", m);
}

/// @brief Push a daemon's current config over its session.
/// @param daemon Daemon to push to.
/// @return False when the spoke has no open session.
bool IstateCore::deliver_config(const std::string& daemon)
{
    auto doc = pst_.load_config(daemon);
    if (!doc.ok())
    {
        // No config yet (virgin, or unrecoverable): the spoke keeps its compiled
        // defaults and stays up — never block on configuration (R10).
        log_.warning("istate", "no deliverable config for " + daemon + ": " + doc.error.str());
        return false;
    }
    uint64_t rev;
    {
        std::lock_guard<std::mutex> lk(config_mu_);
        rev = ++config_revisions_[daemon];
    }
    transport::GenericHubMsg m;
    m.has_config = true;
    m.config.set_daemon(daemon);
    m.config.set_yaml((*doc).to_yaml());
    m.config.set_schema_version(pst_.schema_version());
    m.config.set_revision(rev);
    for (const auto& kv : (*doc).origins())
    {
        common::Origin o = common::ORIGIN_DEFAULT;
        switch (kv.second)
        {
        case Origin::Default: o = common::ORIGIN_DEFAULT; break;
        case Origin::App: o = common::ORIGIN_APP; break;
        case Origin::Local: o = common::ORIGIN_LOCAL; break;
        case Origin::Hq: o = common::ORIGIN_HQ; break;
        }
        (*m.config.mutable_origins())[kv.first] = o;
    }
    if (!relay_to_spoke_) return false;
    return relay_to_spoke_(daemon, m);
}

/// @brief Push config to every spoke with an open session.
void IstateCore::deliver_config_all()
{
    for (const auto& name : startup_order()) (void)deliver_config(name);
}

/// @brief Last config revision delivered to a daemon.
/// @param daemon Daemon to query.
/// @return The last delivered revision (0 = never).
uint64_t IstateCore::config_revision(const std::string& daemon) const
{
    std::lock_guard<std::mutex> lk(config_mu_);
    auto it = config_revisions_.find(daemon);
    return it == config_revisions_.end() ? 0 : it->second;
}

/// @brief Last delivery ack state for a daemon.
/// @param daemon Daemon to query.
/// @return The last ack state for the daemon.
IstateCore::ConfigAckState IstateCore::config_ack_state(const std::string& daemon) const
{
    std::lock_guard<std::mutex> lk(config_mu_);
    auto it = config_acks_.find(daemon);
    return it == config_acks_.end() ? ConfigAckState{} : it->second;
}

/// @brief Render system info for the Configuration API.
/// @return The system info as JSON.
std::string IstateCore::system_info_json() const
{
    auto fid = plat_.read_factory_identity();
    JValue o = JValue::object();
    o.set("hostname", JValue(provisioner_.identity().hostname));
    o.set("serial", JValue(fid.ok() ? fid->serial : ""));
    o.set("hw_revision", JValue(fid.ok() ? fid->hw_revision : ""));
    o.set("board_variant", JValue(fid.ok() ? fid->board_variant : ""));
    o.set("firmware_version", JValue(INSTR_VERSION));
    o.set("suite_version", JValue(INSTR_VERSION));
    o.set("config_schema_version", JValue(pst_.schema_version()));
    o.set("istate_state", JValue(orchestrator_.degraded() ? "degraded" : "operational"));
    o.set("provisioning_state", JValue(provisioner_.state_name(provisioner_.state())));
    o.set("boot_id", JValue(ts_.boot_id()));
    o.set("boot_count", JValue(double(plat_.boot_count())));
    o.set("clean_shutdown", JValue(last_shutdown_clean()));
    // Capability manifest (§5.1): what this board actually has, from its
    // descriptor — not a fixed list that claims every feature everywhere.
    JValue caps = JValue::array();
    for (const auto& c : capabilities_) caps.push(JValue(c));
    o.set("capabilities", caps);

    // Time sources: which exist on this board and what each is doing (§7.1).
    JValue sources = JValue::array();
    for (const auto& t : const_cast<platform::IPlatform&>(plat_).time_sources())
    {
        JValue s = JValue::object();
        s.set("kind", JValue(t.kind));
        s.set("present", JValue(t.present));
        s.set("locked", JValue(t.locked));
        s.set("offset_ms", JValue(t.offset_ms));
        s.set("quality", JValue(t.quality));
        sources.push(s);
    }
    o.set("time_sources", sources);
    o.set("time_source", JValue(time_policy_.current_source));

    // Measured boot state, where the platform supports it (§10). Absence is
    // reported as absence rather than silently omitted, so HQ can tell an
    // unattested board from one it simply has no data for.
    auto mb = const_cast<platform::IPlatform&>(plat_).measured_boot();
    JValue boot = JValue::object();
    boot.set("available", JValue(mb.available));
    boot.set("mechanism", JValue(mb.mechanism.empty() ? std::string("none") : mb.mechanism));
    boot.set("secure_boot_enabled", JValue(mb.secure_boot_enabled));
    JValue pcrs = JValue::object();
    for (const auto& kv : mb.pcrs) pcrs.set(std::to_string(kv.first), JValue(kv.second));
    boot.set("pcrs", pcrs);
    o.set("measured_boot", boot);

    return o.dump();
}

/// @brief Render the budget usage report.
/// @return The budget usage report as JSON.
std::string IstateCore::budgets_json() const
{
    JValue arr = JValue::array();
    for (const auto& kv : budgets_.budgets())
    {
        JValue o = JValue::object();
        o.set("consumer", JValue(kv.first));
        o.set("limit_bytes", JValue(double(kv.second.limit_bytes)));
        o.set("used_bytes", JValue(double(budgets_.usage(kv.first))));
        o.set("warning", JValue(budgets_.usage(kv.first) >= uint64_t(double(kv.second.limit_bytes) * kv.second.alarm_at_pct)));
        arr.push(o);
    }
    return arr.dump();
}

/// @brief System reset: audit, wipe and reseed, then restart spokes.
/// @param actor Who requested the reset (audit).
/// @return True when performed, or an error.
Result<bool> IstateCore::system_reset(const std::string& actor)
{
    // System reset: wipes system/ and state/, keeps factory/ and backup/,
    // reseeds from image defaults (§6.1).
    std::map<std::string, std::string> defaults;
    // Defaults come from the image set already registered with the wiring.
    if (default_provider_) defaults = default_provider_();
    Error e = pst_.system_reset(defaults);
    if (!e.ok()) return Result<bool>::fail(e);
    audit_.append(actor, "configuration-ui", "reset", "{\"type\":\"system\"}", ts_);
    audit_.persist();
    timeline_.append("config_change", actor, "{\"reset\":\"system\"}", ts_);
    log_.warning("istate", "system reset performed by " + actor);
    return Result<bool>::good(true);
}

/// @brief Factory reset: audit and wipe everything (explicitly gated).
/// @param actor Who requested the reset (audit).
/// @param confirmation Required confirmation string.
/// @return True when performed, or an error.
Result<bool> IstateCore::factory_reset(const std::string& actor, const std::string& confirmation)
{
    // Manufacturing operation only, explicitly gated (§6.1).
    if (confirmation != "FACTORY-RESET") return Result<bool>::fail(Errc::InvalidArgument, "factory reset requires explicit confirmation string FACTORY-RESET", "factory_reset");
    Error e = pst_.factory_reset();
    if (!e.ok()) return Result<bool>::fail(e);
    audit_.append(actor, "local-console", "reset", "{\"type\":\"factory\"}", ts_);
    audit_.persist();
    log_.critical("istate", "FACTORY RESET performed by " + actor);
    return Result<bool>::good(true);
}

/// @brief Marker file path.
/// @return The marker file path.
std::string IstateCore::clean_shutdown_marker_path() const
{
    return opts_.pst_root + "/state/clean-shutdown";
}

/// @brief Write the clean-shutdown marker.
void IstateCore::write_clean_shutdown_marker()
{
    fs_.mkdirs(opts_.pst_root + "/state");
    fs_.write(clean_shutdown_marker_path(), std::to_string(ts_.wall_ns()));
    // The suite is now in a clean-shutdown state; system_info reports it.
    observed_clean_shutdown_ = true;
}

} // namespace istate
} // namespace instr