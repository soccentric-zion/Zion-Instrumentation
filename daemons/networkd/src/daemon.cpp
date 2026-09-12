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
 * @file daemons/networkd/src/daemon.cpp
 * @brief networkd daemon facade: core state, shared command table construction, and spoke channel wiring (REQ §7.2, R8).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/networkd/daemon.h"

#include <sstream>

namespace instr
{
/// @brief networkd: network plan application, tunnel guard and remote access (§7.2).
namespace networkd
{

/// @brief Set the running plan without applying it.
/// @param plan Plan to make current (without applying).
Error NetworkdCore::load_plan(const NetworkPlan& plan)
{
    plan_ = plan;
    return Error::none();
}

/// @brief Render and apply the current plan through the platform.
Error NetworkdCore::apply()
{
    JValue j = render_plan_json(plan_);
    Error e = plat_.apply_network_plan(j.dump());
    if (!e.ok()) return e;
    if (event_sink_) event_sink_("plan_applied", "{}");
    return Error::none();
}

/// @brief Apply a new plan under the tunnel-loss guard.
/// @param next Candidate plan.
/// @param timeout_s Revert grace period in seconds.
/// @return What happened (applied, reverted, detail).
TunnelGuard::Result NetworkdCore::guarded_apply(const NetworkPlan& next, double timeout_s)
{
    NetworkPlan previous = plan_;
    auto reach = reachability_ ? reachability_ : [this] { return plat_.hq_reachable(); };
    auto clock = clock_fn_ ? clock_fn_ : [this] { return double(ts_.mono_ns()) / 1e9; };
    auto result = TunnelGuard::guarded_change(
        previous, next,
        [this](const NetworkPlan& p) {
            plan_ = p;
            return apply();
        },
        reach, timeout_s, clock);
    if (result.reverted)
    {
        if (event_sink_) event_sink_("guard_revert", "{\"reason\":\"hq-unreachable\"}");
    }
    return result;
}

/// @brief Render and hand the time-daemon configs to the platform.
/// @param policy Time policy from istate.
Error NetworkdCore::apply_time_policy(const TimePolicy& policy)
{
    plan_.time = policy;
    time_daemons_ = render_time_daemons(policy);
    JValue j = JValue::object();
    j.set("kind", JValue("time_policy"));
    j.set("chrony_conf", JValue(time_daemons_.chrony_conf));
    j.set("ptp4l_args", JValue(time_daemons_.ptp4l_args));
    j.set("phc2sys_args", JValue(time_daemons_.phc2sys_args));
    j.set("gpsd_args", JValue(time_daemons_.gpsd_args));
    Error e = plat_.apply_network_plan(j.dump());
    if (!e.ok()) return e;
    if (event_sink_) event_sink_("time_policy_applied", "{}");
    return Error::none();
}

/// @brief Render effective state as JSON.
/// @return The effective state as JSON.
std::string NetworkdCore::status_json() const
{
    int64_t now = ts_.wall_ns() / 1'000'000'000ll;
    std::vector<SshWindow> active = ssh_.active(now);
    return plan_status_json(plan_, active, tunnel_up_);
}

// ---------------- command table (full coverage; runtime-only) ----------------

/// @brief Build a NetworkPlan from a delivered config document.
/// @param doc Parsed, schema-validated document.
/// @return The rendered plan, or an error.
Result<NetworkPlan> NetworkdCore::plan_from_config(const ConfigDocument& doc)
{
    NetworkPlan p;
    for (size_t i = 0;; ++i)
    {
        const std::string b = "interfaces." + std::to_string(i) + ".";
        if (!doc.has(b + "name")) break;
        IfaceConfig ic;
        ic.name = doc.get_string(b + "name");
        ic.mode = doc.get_string(b + "mode", "dhcp");
        ic.address = doc.get_string(b + "address");
        ic.prefix = uint32_t(doc.get_int(b + "prefix", 24));
        ic.vlan_id = uint32_t(doc.get_int(b + "vlan_id", 0));
        ic.bond_member = doc.get_string(b + "bond_member");
        ic.priority = uint32_t(doc.get_int(b + "priority", 100));
        if (ic.mode == "static" && ic.address.empty()) return Result<NetworkPlan>::fail(Errc::InvalidArgument, "static interface " + ic.name + " has no address", "networkd config");
        if (ic.mode == "vlan" && ic.vlan_id == 0) return Result<NetworkPlan>::fail(Errc::InvalidArgument, "vlan interface " + ic.name + " has no vlan_id", "networkd config");
        p.ifaces.push_back(ic);
    }
    for (size_t i = 0;; ++i)
    {
        const std::string b = "firewall." + std::to_string(i) + ".";
        if (!doc.has(b + "action")) break;
        FirewallRule r;
        r.action = doc.get_string(b + "action");
        r.proto = doc.get_string(b + "proto", "any");
        r.src = doc.get_string(b + "src", "any");
        r.dst_port = uint32_t(doc.get_int(b + "dst_port", 0));
        r.comment = doc.get_string(b + "comment");
        p.firewall.push_back(r);
    }
    for (size_t i = 0;; ++i)
    {
        const std::string b = "port_forwards." + std::to_string(i) + ".";
        if (!doc.has(b + "listen_port")) break;
        PortForward f;
        f.proto = doc.get_string(b + "proto", "tcp");
        f.listen_port = uint32_t(doc.get_int(b + "listen_port", 0));
        f.dest_ip = doc.get_string(b + "dest_ip");
        f.dest_port = uint32_t(doc.get_int(b + "dest_port", 0));
        p.forwards.push_back(f);
    }
    for (size_t i = 0;; ++i)
    {
        const std::string b = "routes." + std::to_string(i) + ".";
        if (!doc.has(b + "destination")) break;
        RouteConfig r;
        r.destination = doc.get_string(b + "destination");
        r.gateway = doc.get_string(b + "gateway");
        r.iface = doc.get_string(b + "iface");
        r.metric = uint32_t(doc.get_int(b + "metric", 0));
        p.routes.push_back(r);
    }
    p.dns.servers = doc.get_list("dns.servers");
    p.dns.search = doc.get_list("dns.search");

    p.wireless.enabled = doc.get_bool("wireless.enabled", false);
    p.wireless.mode = doc.get_string("wireless.mode", "off");
    p.wireless.iface = doc.get_string("wireless.interface");
    p.wireless.ssid = doc.get_string("wireless.ssid");
    p.wireless.country = doc.get_string("wireless.country");
    p.wireless.ap_channel = uint32_t(doc.get_int("wireless.ap_channel", 6));
    // The PSK is referenced, never carried: credentials live in /pst (§7.2).
    p.wireless.psk_ref = p.wireless.ssid.empty() ? "" : ("wifi-psk-" + p.wireless.ssid);
    if (p.wireless.enabled && p.wireless.mode != "off" && p.wireless.iface.empty()) return Result<NetworkPlan>::fail(Errc::InvalidArgument, "wireless enabled without an interface", "networkd config");

    // Inbound SSH is off by default and is never turned on by configuration
    // alone — it is opened per device, time-boxed and attributed (§7.2, §10).
    p.ssh_enabled = false;
    return Result<NetworkPlan>::good(std::move(p));
}

/// @brief Apply an istate-delivered config document.
/// @param doc Parsed, schema-validated document.
/// @return ok(), or an error describing why the values were refused.
Error NetworkdCore::apply_config(const ConfigDocument& doc)
{
    Config c;
    c.enabled = doc.get_bool("enabled", true);
    c.tunnel_guard_timeout_s = doc.get_double("tunnel_guard_timeout_s", 30.0);
    c.remote_ssh_default_enabled = doc.get_bool("remote_ssh_default_enabled", false);
    c.remote_ssh_max_ttl_s = uint32_t(doc.get_int("remote_ssh_max_ttl_s", 8 * 3600));
    c.connectivity_probe_interval_s = doc.get_double("connectivity_probe_interval_s", 10.0);
    c.ssh_sweep_interval_s = doc.get_double("ssh_sweep_interval_s", 30.0);
    if (c.tunnel_guard_timeout_s <= 0) return Error::make(Errc::InvalidArgument, "tunnel_guard_timeout_s must be > 0", "networkd config");

    auto next = plan_from_config(doc);
    if (!next.ok()) return next.error;
    // Preserve what configuration does not own: the tunnel parameters come
    // from enrollment, and the time policy comes from istate (R8).
    (*next).wg = plan_.wg;
    (*next).wg_up = plan_.wg_up;
    (*next).time = plan_.time;
    (*next).ssh_enabled = plan_.ssh_enabled;

    // A configuration change is a network change: it goes through the same
    // tunnel-loss guard as any other, so a bad push auto-reverts (§7.2).
    auto res = guarded_apply(*next, c.tunnel_guard_timeout_s);
    cfg_ = c;
    ssh_.set_max_ttl_s(c.remote_ssh_max_ttl_s);
    if (on_config_) on_config_(cfg_);
    if (res.reverted) return Error::make(Errc::Unavailable, "configuration reverted: it made HQ unreachable (" + res.detail + ")", "networkd config");
    return Error::none();
}

/// @brief Expire SSH windows and return the events to emit.
/// @return Events to emit upward.
std::vector<std::pair<std::string, std::string>> NetworkdCore::sweep_remote_access()
{
    std::vector<std::pair<std::string, std::string>> events;
    for (const auto& w : ssh_.sweep(wall_now_s()))
    {
        // Expiry is customer-visible: the window was alarm-visible when opened,
        // so its close must be too (§7.2, §10).
        events.emplace_back("remote_access_expired", "{\"operator\":\"" + w.operator_name + "\"}");
    }
    if (!ssh_.any_active(wall_now_s()) && plan_.ssh_enabled)
    {
        plan_.ssh_enabled = false;
        (void)apply();
        events.emplace_back("remote_access_closed", "{}");
    }
    for (const auto& e : events)
        if (event_sink_) event_sink_(e.first, e.second);
    return events;
}

/// @brief Probe interfaces and re-rank; returns events to emit.
/// @return Events to emit upward.
std::vector<std::pair<std::string, std::string>> NetworkdCore::probe_connectivity()
{
    std::vector<std::pair<std::string, std::string>> events;
    const bool up = reachability_ ? reachability_() : plat_.hq_reachable();
    if (up != tunnel_up_)
    {
        tunnel_up_ = up;
        events.emplace_back("tunnel_state", std::string("{\"up\":") + (up ? "true" : "false") + "}");
    }
    // Re-rank interfaces so failover reflects what is actually reachable.
    std::vector<Reachability> probes;
    for (const auto& i : plat_.interfaces())
    {
        Reachability r;
        r.iface = i.name;
        r.reachable = i.up && i.link;
        probes.push_back(r);
    }
    auto ranked = ConnectivityRanker::rank(plan_.ifaces, probes);
    if (ranked != last_rank_)
    {
        last_rank_ = ranked;
        std::string detail = "{\"order\":[";
        for (size_t i = 0; i < ranked.size(); ++i)
        {
            if (i) detail += ",";
            detail += "\"" + ranked[i] + "\"";
        }
        detail += "]}";
        events.emplace_back("interface_priority", detail);
    }
    for (const auto& e : events)
        if (event_sink_) event_sink_(e.first, e.second);
    return events;
}

/// @brief Build the networkd ctrl command table.
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_networkd_table(NetworkdCore* core)
{
    CommandTable t;
    t.add({"status", "full network status (plan, tunnel, ssh windows)", {}, [core](const std::string&) { return CommandResult::good(core->status_json()); }});
    t.add({"ifaces", "list configured interfaces", {{"verbose", false, "include mode details (true/false)"}}, [core](const std::string& args) {
               auto a = args_object(args);
               bool verbose = a.ok() && (*a).get_bool("verbose");
               JValue arr = JValue::array();
               for (const auto& i : core->plan().ifaces)
               {
                   JValue o = JValue::object();
                   o.set("name", JValue(i.name));
                   o.set("mode", JValue(i.mode));
                   o.set("priority", JValue(double(i.priority)));
                   if (verbose)
                   {
                       o.set("address", JValue(i.address));
                       o.set("vlan_id", JValue(double(i.vlan_id)));
                   }
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    t.add({"apply-plan", "apply the current plan through the platform", {}, [core](const std::string&) {
               Error e = core->apply();
               return e.ok() ? CommandResult::good(std::string("{\"applied\":true}")) : CommandResult::from_error(e);
           }});
    t.add({"firewall-get", "list firewall rules", {}, [core](const std::string&) {
               JValue arr = JValue::array();
               for (const auto& r : core->plan().firewall)
               {
                   JValue o = JValue::object();
                   o.set("action", JValue(r.action));
                   o.set("proto", JValue(r.proto));
                   o.set("src", JValue(r.src));
                   o.set("dst_port", JValue(double(r.dst_port)));
                   o.set("comment", JValue(r.comment));
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    t.add({"firewall-add",
           "add a runtime firewall rule (persistent config via istate)",
           {{"action", true, "allow|deny"}, {"proto", true, "tcp|udp|icmp|any"}, {"src", false, "source cidr (any)"}, {"dst_port", false, "destination port (0=any)"}, {"comment", false, "rule comment"}},
           [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               FirewallRule r;
               r.action = (*a).get_string("action");
               r.proto = (*a).get_string("proto", "any");
               r.src = (*a).get_string("src", "any");
               r.dst_port = uint32_t((*a).get_number("dst_port", 0.0));
               r.comment = (*a).get_string("comment");
               if (r.action != "allow" && r.action != "deny") return CommandResult::err(Errc::InvalidArgument, "action must be allow|deny");
               core->mutable_plan().firewall.push_back(r);
               return CommandResult::good(std::string("{\"added\":true}"));
           }});
    t.add({"ssh-open", "open a time-boxed, attributed inbound SSH window (alarm-visible)", {{"operator", true, "named operator for attribution"}, {"ttl_s", true, "window TTL 1s..24h"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               std::string op = (*a).get_string("operator");
               uint64_t ttl = uint64_t((*a).get_number("ttl_s", 0));
               auto w = core->ssh().open(op, ttl, core->wall_now_s());
               if (!w.ok()) return CommandResult::from_error(w.error);
               JValue o = JValue::object();
               o.set("operator", JValue(w->operator_name));
               o.set("expires_unix_s", JValue(double(w->expires_unix_s)));
               return CommandResult::good(o.dump());
           }});
    t.add({"ssh-close", "close all inbound SSH windows", {}, [core](const std::string&) {
               core->ssh().close_all();
               return CommandResult::good(std::string("{\"closed\":true}"));
           }});
    t.add({"ssh-list", "list SSH windows", {}, [core](const std::string&) {
               JValue arr = JValue::array();
               for (const auto& w : core->ssh().active(core->wall_now_s()))
               {
                   JValue o = JValue::object();
                   o.set("operator", JValue(w.operator_name));
                   o.set("expires_unix_s", JValue(double(w.expires_unix_s)));
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    t.add({"tunnel-status", "WireGuard tunnel state", {}, [core](const std::string&) {
               JValue o = JValue::object();
               o.set("up", JValue(core->tunnel_up()));
               o.set("endpoint", JValue(core->plan().wg.hq_endpoint));
               return CommandResult::good(o.dump());
           }});
    t.add({"time-policy-render", "render time daemon configs from the current policy", {}, [core](const std::string&) {
               JValue o = JValue::object();
               o.set("chrony_conf", JValue(core->time_daemons().chrony_conf));
               o.set("ptp4l_args", JValue(core->time_daemons().ptp4l_args));
               o.set("phc2sys_args", JValue(core->time_daemons().phc2sys_args));
               o.set("gpsd_args", JValue(core->time_daemons().gpsd_args));
               return CommandResult::good(o.dump());
           }});
    t.add({"failover-rank", "rank interfaces by priority and reachability", {}, [core](const std::string&) {
               std::vector<Reachability> probes;
               for (const auto& i : core->platform().interfaces())
               {
                   Reachability r;
                   r.iface = i.name;
                   r.reachable = i.link;
                   probes.push_back(r);
               }
               auto ranked = ConnectivityRanker::rank(core->plan().ifaces, probes);
               JValue arr = JValue::array();
               for (const auto& name : ranked) arr.push(JValue(name));
               return CommandResult::good(arr.dump());
           }});
    return t;
}

// ---------------- spoke ----------------

/// @brief Wire the channel, config sink and handlers together.
/// @param core Domain core.
/// @param log Logger.
/// @param table Command table.
/// @param hub_target istate gRPC target.
/// @param build_version Build version to advertise.
NetworkdSpoke::NetworkdSpoke(NetworkdCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version) : core_(core), log_(log), table_(table)
{
    SpokeChannel<networkd::NetworkHub, networkd::NetworkSpokeFrame, networkd::NetworkHubFrame>::Options o;
    o.target = hub_target;
    o.daemon = "networkd";
    o.build_version = build_version;
    o.capabilities = {"wired", "wireless", "firewall", "tunnel", "time-daemons"};
    channel_ = std::make_unique<SpokeChannel<networkd::NetworkHub, networkd::NetworkSpokeFrame, networkd::NetworkHubFrame>>(std::move(o), *log);
    channel_->set_hub_handler([this](const networkd::NetworkHubFrame& hf) {
        if (hf.has_command())
        {
            common::Ack ack = dispatch_to_ack(table_, hf.command());
            channel_->send_ack(ack);
        }
    });
    core_->set_event_sink([this](const std::string& kind, const std::string& detail) {
        networkd::NetworkEvent ev;
        ev.set_kind(kind);
        ev.set_detail_json(detail);
        send_event(ev);
    });
    // Until a schema is installed, deliveries still apply — just unvalidated.
    // A packaging slip must not leave a daemon running unconfigured (R10).
    set_schema(JValue::object());
}

/// @brief Install the schema every config delivery must satisfy.
/// @param schema Schema every config delivery must satisfy.
void NetworkdSpoke::set_schema(JValue schema)
{
    config_sink_ = std::make_unique<ConfigSink>("networkd", std::move(schema), [this](const ConfigDocument& doc) { return core_->apply_config(doc); });
    channel_->set_config_handler([this](const common::ConfigDelivery& delivery) { return config_sink_->deliver(delivery.yaml(), delivery.revision()); });
}

/// @brief Start the transport channel.
void NetworkdSpoke::start()
{
    channel_->start();
}
/// @brief Stop the transport channel.
void NetworkdSpoke::stop()
{
    channel_->stop();
}

/// @brief Queue a network event to istate.
/// @param ev Network event to queue.
void NetworkdSpoke::send_event(const networkd::NetworkEvent& ev)
{
    channel_->enqueue([ev](networkd::NetworkSpokeFrame& f) { *f.mutable_event() = ev; });
}

} // namespace networkd
} // namespace instr