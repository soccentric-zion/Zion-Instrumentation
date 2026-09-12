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
 * @file daemons/networkd/src/domain.cpp
 * @brief networkd domain implementation (REQ §7.2, R8): plan rendering, firewall rules, remote-access windows, tunnel guard, time daemon state, and failover ranking.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/networkd/domain.h"

#include <algorithm>
#include <sstream>

namespace instr
{
/// @brief networkd: network plan application, tunnel guard and remote access (§7.2).
namespace networkd
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief Render a plan to the JSON shape handed to platform.apply_network_plan.
/// @param plan Plan to render.
/// @return The plan as the apply-seam JSON shape.
JValue render_plan_json(const NetworkPlan& plan)
{
    JValue j = JValue::object();
    JValue ifaces = JValue::array();
    for (const auto& i : plan.ifaces)
    {
        JValue o = JValue::object();
        o.set("name", JValue(i.name));
        o.set("mode", JValue(i.mode));
        if (i.mode == "static")
        {
            o.set("address", JValue(i.address));
            o.set("prefix", JValue(double(i.prefix)));
        }
        if (i.mode == "vlan") o.set("vlan_id", JValue(double(i.vlan_id)));
        if (i.mode == "bond") o.set("bond_member", JValue(i.bond_member));
        o.set("priority", JValue(double(i.priority)));
        ifaces.push(o);
    }
    j.set("interfaces", ifaces);
    JValue fw = JValue::array();
    for (const auto& r : plan.firewall)
    {
        JValue o = JValue::object();
        o.set("action", JValue(r.action));
        o.set("proto", JValue(r.proto));
        o.set("src", JValue(r.src));
        if (r.dst_port) o.set("dst_port", JValue(double(r.dst_port)));
        o.set("comment", JValue(r.comment));
        fw.push(o);
    }
    j.set("firewall", fw);
    JValue pf = JValue::array();
    for (const auto& f : plan.forwards)
    {
        JValue o = JValue::object();
        o.set("proto", JValue(f.proto));
        o.set("listen_port", JValue(double(f.listen_port)));
        o.set("dest_ip", JValue(f.dest_ip));
        o.set("dest_port", JValue(double(f.dest_port)));
        pf.push(o);
    }
    j.set("port_forwarding", pf);
    JValue routes = JValue::array();
    for (const auto& r : plan.routes)
    {
        JValue o = JValue::object();
        o.set("destination", JValue(r.destination));
        o.set("gateway", JValue(r.gateway));
        o.set("iface", JValue(r.iface));
        o.set("metric", JValue(double(r.metric)));
        routes.push(o);
    }
    j.set("routes", routes);
    JValue dns = JValue::object();
    JValue dns_servers = JValue::array();
    for (const auto& d : plan.dns.servers) dns_servers.push(JValue(d));
    dns.set("servers", dns_servers);
    JValue dns_search = JValue::array();
    for (const auto& d : plan.dns.search) dns_search.push(JValue(d));
    dns.set("search", dns_search);
    j.set("dns", dns);
    JValue wifi = JValue::object();
    wifi.set("enabled", JValue(plan.wireless.enabled));
    wifi.set("mode", JValue(plan.wireless.mode));
    wifi.set("iface", JValue(plan.wireless.iface));
    wifi.set("ssid", JValue(plan.wireless.ssid));
    wifi.set("country", JValue(plan.wireless.country));
    wifi.set("ap_channel", JValue(double(plan.wireless.ap_channel)));
    wifi.set("hidden", JValue(plan.wireless.hidden));
    // psk_ref names a key in /pst; the secret itself never enters the plan.
    wifi.set("psk_ref", JValue(plan.wireless.psk_ref));
    j.set("wireless", wifi);
    j.set("ssh_enabled", JValue(plan.ssh_enabled));
    JValue wg = JValue::object();
    wg.set("endpoint", JValue(plan.wg.hq_endpoint));
    wg.set("device_addr", JValue(plan.wg.device_addr));
    wg.set("hq_addr", JValue(plan.wg.hq_addr));
    wg.set("peer_pubkey", JValue(plan.wg.peer_pubkey)); // HQ's key: not a device secret
    wg.set("listen_port", JValue(double(plan.wg.listen_port)));
    wg.set("up", JValue(plan.wg_up));
    j.set("wireguard", wg);
    JValue tp = JValue::object();
    JValue prio = JValue::array();
    for (const auto& s : plan.time.source_priority) prio.push(JValue(s));
    tp.set("source_priority", prio);
    JValue servers = JValue::array();
    for (const auto& s : plan.time.ntp_servers) servers.push(JValue(s));
    tp.set("ntp_servers", servers);
    tp.set("ptp_domain", JValue(double(plan.time.ptp_domain)));
    tp.set("ptp_profile", JValue(plan.time.ptp_profile));
    JValue consts = JValue::array();
    for (const auto& s : plan.time.gnss_constellations) consts.push(JValue(s));
    tp.set("gnss_constellations", consts);
    j.set("time_policy", tp);
    return j;
}

/// @brief Apply @p next, reverting to @p previous when HQ becomes unreachable.
/// @param previous Currently running plan (revert target).
/// @param next Candidate plan.
/// @param apply Plan-apply callback.
/// @param reachability HQ-reachability probe.
/// @param timeout_s Grace period before reverting.
/// @param clock_s Monotonic seconds.
/// @return What happened (applied, reverted, detail).
TunnelGuard::Result TunnelGuard::guarded_change(const NetworkPlan& previous, const NetworkPlan& next, const std::function<Error(const NetworkPlan&)>& apply, const std::function<bool()>& reachability, double timeout_s, const std::function<double()>& clock_s)
{
    Result r;
    Error e = apply(next);
    if (!e.ok())
    {
        r.detail = "apply failed: " + e.str();
        // Re-apply the previous plan so the device keeps its working config.
        Error pe = apply(previous);
        r.reverted = pe.ok();
        return r;
    }
    r.applied = true;
    double deadline = clock_s() + timeout_s;
    // Probe HQ reachability: immediately and until the deadline.
    while (!reachability())
    {
        if (clock_s() >= deadline)
        {
            Error pe = apply(previous);
            r.reverted = pe.ok();
            r.detail = "HQ unreachable within " + std::to_string(timeout_s) + "s; change auto-reverted to the previous plan";
            return r;
        }
        struct timespec ts
        {
            0, 10 * 1000 * 1000
        };
        nanosleep(&ts, nullptr);
    }
    r.detail = "applied; HQ reachable";
    return r;
}

/// @brief Open a time-boxed, clamped SSH window.
/// @param operator_name Who requested the window.
/// @param ttl_s Requested lifetime (clamped to the cap).
/// @param now_unix_s Current wall time.
/// @return The opened window, or an error.
Result<SshWindow> RemoteAccessManager::open(const std::string& operator_name, uint64_t ttl_s, int64_t now_unix_s)
{
    if (operator_name.empty()) return Result<SshWindow>::fail(Errc::InvalidArgument, "remote access must be attributed to a named operator", "");
    if (ttl_s == 0 || ttl_s > 24ull * 3600) return Result<SshWindow>::fail(Errc::InvalidArgument, "ttl must be 1s..24h", "");
    // Clamp to the configured ceiling rather than refusing: the operator still
    // gets access, just not for longer than policy allows (§7.2).
    if (max_ttl_s_ > 0 && ttl_s > max_ttl_s_) ttl_s = max_ttl_s_;
    SshWindow w;
    w.operator_name = operator_name;
    w.opened_unix_s = now_unix_s;
    w.expires_unix_s = now_unix_s + int64_t(ttl_s);
    w.active = true;
    windows_.push_back(w);
    return Result<SshWindow>::good(w);
}

/// @brief Expire windows whose TTL elapsed.
/// @param now_unix_s Current wall time.
/// @return The windows expired by this sweep.
std::vector<SshWindow> RemoteAccessManager::sweep(int64_t now_unix_s)
{
    std::vector<SshWindow> expired;
    for (auto& w : windows_)
    {
        if (w.active && now_unix_s >= w.expires_unix_s)
        {
            w.active = false;
            expired.push_back(w);
        }
    }
    return expired;
}

/// @brief Windows still open.
/// @param now_unix_s Current wall time.
/// @return Windows still open.
std::vector<SshWindow> RemoteAccessManager::active(int64_t now_unix_s) const
{
    std::vector<SshWindow> out;
    for (const auto& w : windows_)
        if (w.active && now_unix_s < w.expires_unix_s) out.push_back(w);
    return out;
}

/// @brief True when any window is open.
/// @param now_unix_s Current wall time.
/// @return True when any window is open.
bool RemoteAccessManager::any_active(int64_t now_unix_s) const
{
    return !active(now_unix_s).empty();
}

/// @brief Close every window immediately.
void RemoteAccessManager::close_all()
{
    for (auto& w : windows_) w.active = false;
}

/// @brief Render chrony/ptp4l/phc2sys/gpsd configuration from the policy.
/// @param policy Time policy from istate.
/// @return The rendered daemon configurations.
TimeDaemonConfig render_time_daemons(const TimePolicy& policy)
{
    TimeDaemonConfig c;
    // chrony: NTP servers (fallback source).
    std::ostringstream ch;
    ch << "# generated by networkd from istate time policy\n";
    for (const auto& s : policy.ntp_servers) ch << "server " << s << " iburst\n";
    ch << "rtcsync\nmakestep 1.0 3\n";
    // GNSS takes priority when present: gpsd feeds chrony via SHM.
    bool has_gnss = false;
    for (const auto& s : policy.source_priority)
        if (s == "gnss") has_gnss = true;
    if (has_gnss) ch << "refclock SHM 0 offset 0.5 delay 0.2 refid GNSS\n";
    c.chrony_conf = ch.str();
    // ptp4l: domain and profile from policy.
    std::ostringstream ptp;
    ptp << "-f /etc/instrumentation/ptp4l.conf -i $IFACE -d " << policy.ptp_domain;
    if (!policy.ptp_profile.empty()) ptp << " -S --profile=" << policy.ptp_profile;
    c.ptp4l_args = ptp.str();
    c.phc2sys_args = "-a -r";
    std::ostringstream gps;
    bool gnss = false;
    for (const auto& s : policy.source_priority)
        if (s == "gnss") gnss = true;
    if (gnss)
    {
        gps << "-n -S /var/run/gpsd.sock";
        for (const auto& cst : policy.gnss_constellations) (void)cst; // gpsd auto-configures
    }
    c.gpsd_args = gps.str();
    return c;
}

/// @brief Rank interfaces: reachable by priority, unreachable last.
/// @param cfgs Configured interfaces with priorities.
/// @param probes Probe results per interface.
/// @return Interface names, best first.
std::vector<std::string> ConnectivityRanker::rank(const std::vector<IfaceConfig>& cfgs, const std::vector<Reachability>& probes)
{
    std::map<std::string, uint32_t> prio;
    for (const auto& c : cfgs) prio[c.name] = c.priority;
    std::vector<std::string> out;
    std::map<std::string, bool> reach;
    for (const auto& p : probes) reach[p.iface] = p.reachable;
    std::vector<std::pair<uint32_t, std::string>> ranked;
    for (const auto& kv : prio)
    {
        auto rit = reach.find(kv.first);
        bool r = (rit != reach.end()) ? rit->second : false;
        // Unreachable interfaces rank last: add a large penalty.
        uint32_t key = kv.second + (r ? 0 : 100000);
        ranked.emplace_back(key, kv.first);
    }
    std::sort(ranked.begin(), ranked.end());
    for (const auto& kv : ranked) out.push_back(kv.second);
    return out;
}

/// @brief Render the effective-state snapshot as JSON.
/// @param plan Current plan.
/// @param windows Open SSH windows.
/// @param tunnel_up Tunnel state.
/// @return The effective-state snapshot as JSON.
std::string plan_status_json(const NetworkPlan& plan, const std::vector<SshWindow>& windows, bool tunnel_up)
{
    JValue j = render_plan_json(plan);
    j.set("tunnel_up", JValue(tunnel_up));
    JValue arr = JValue::array();
    for (const auto& w : windows)
    {
        JValue o = JValue::object();
        o.set("operator", JValue(w.operator_name));
        o.set("opened_unix_s", JValue(double(w.opened_unix_s)));
        o.set("expires_unix_s", JValue(double(w.expires_unix_s)));
        o.set("active", JValue(w.active));
        arr.push(o);
    }
    j.set("ssh_windows", arr);
    return j.dump();
}

} // namespace domain
} // namespace networkd
} // namespace instr
namespace instr
{
/// @brief networkd: network plan application, tunnel guard and remote access (§7.2).
namespace networkd
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief Render supplicant/hostapd configuration for a wireless plan.
/// @param w Wireless configuration.
/// @param psk PSK resolved by the caller from /pst.
/// @return The rendered supplicant/AP configuration.
WirelessRender render_wireless(const WirelessConfig& w, const std::string& psk)
{
    WirelessRender r;
    r.iface = w.iface;
    r.mode = w.mode;
    if (!w.enabled || w.mode == "off") return r;

    if (w.mode == "client")
    {
        std::ostringstream o;
        o << "ctrl_interface=/run/wpa_supplicant\n";
        o << "update_config=1\n";
        if (!w.country.empty()) o << "country=" << w.country << "\n";
        o << "network={\n";
        o << "\tssid=\"" << w.ssid << "\"\n";
        if (w.hidden) o << "\tscan_ssid=1\n";
        if (psk.empty()) { o << "\tkey_mgmt=NONE\n"; }
        else
        {
            o << "\tkey_mgmt=WPA-PSK\n";
            o << "\tpsk=\"" << psk << "\"\n";
        }
        o << "}\n";
        r.wpa_supplicant_conf = o.str();
        return r;
    }

    if (w.mode == "ap")
    {
        // AP mode exists for field commissioning: an engineer on site brings the
        // device up with no wired network and configures it from a phone.
        std::ostringstream o;
        o << "interface=" << w.iface << "\n";
        o << "driver=nl80211\n";
        o << "ssid=" << w.ssid << "\n";
        o << "hw_mode=g\n";
        o << "channel=" << w.ap_channel << "\n";
        if (!w.country.empty()) o << "country_code=" << w.country << "\n";
        o << "ignore_broadcast_ssid=" << (w.hidden ? 1 : 0) << "\n";
        if (psk.empty())
        {
            o << "auth_algs=1\n"; // open: commissioning only, behind the firewall
        }
        else
        {
            o << "auth_algs=1\nwpa=2\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\n";
            o << "wpa_passphrase=" << psk << "\n";
        }
        r.hostapd_conf = o.str();
    }
    return r;
}

} // namespace domain
} // namespace networkd
} // namespace instr
