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
 * @file daemons/networkd/include/instr/networkd/domain.h
 * @brief owns everything network-adjacent (REQ §7.2, R8). Domain logic:
 *
 * plans, firewall, remote-access windows, tunnel guard, time daemons, and
 * failover ranking. Pure classes; the platform applies rendered plans.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "instr/common/error.h"
#include "instr/common/json.h"
#include "instr/common/timestamps.h"

namespace instr
{
/// @brief networkd: network plan application, tunnel guard and remote access (§7.2).
namespace networkd
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief One interface's configuration (persisted by istate in /pst/system/networkd).
struct IfaceConfig
{
    std::string name;           ///< Interface name.
    std::string mode;           ///< dhcp | static | vlan | bond | unused.
    std::string address;        ///< Static address.
    uint32_t prefix = 24;       ///< Prefix length for static mode.
    uint32_t vlan_id = 0;       ///< VLAN tag for vlan mode.
    std::string bond_member;    ///< Bond parent iface.
    uint32_t priority = 100;    ///< Failover ranking (lower = preferred).
};

/// @brief One firewall rule.
struct FirewallRule
{
    std::string action;        ///< allow | deny.
    std::string proto;         ///< tcp | udp | icmp | any.
    std::string src;           ///< CIDR or any.
    uint32_t dst_port = 0;     ///< 0 = any.
    std::string comment;       ///< Operator-facing comment.
};

/// @brief Wireless configuration (§7.2): client and AP mode.
///
/// The PSK is never stored here — credentials live in /pst via istate and are
/// referenced by name, so a plan can be logged without leaking one (§10).
struct WirelessConfig
{
    bool enabled = false;       ///< Master switch.
    std::string mode = "off";    ///< client | ap | off.
    std::string iface;          ///< e.g. wlan0.
    std::string ssid;           ///< Network name.
    std::string psk_ref;        ///< Key name in /pst, never the secret itself.
    std::string country;        ///< Regulatory domain.
    uint32_t ap_channel = 6;    ///< AP mode only.
    bool hidden = false;        ///< Hidden SSID for AP mode.
};

/// @brief A single wireless scan result (client mode).
struct WirelessNetwork
{
    std::string ssid;           ///< Network name.
    std::string bssid;          ///< Access point MAC.
    int32_t signal_dbm = 0;     ///< Signal strength.
    uint32_t channel = 0;       ///< Channel number.
    std::string security;       ///< open | wpa2 | wpa3.
};

/// @brief DNS resolver configuration.
struct DnsConfig
{
    std::vector<std::string> servers;   ///< Upstream servers.
    std::vector<std::string> search;    ///< Search domains.
};

/// @brief One static route.
struct RouteConfig
{
    std::string destination;    ///< CIDR, or "default".
    std::string gateway;        ///< Next hop.
    std::string iface;          ///< Egress interface.
    uint32_t metric = 0;        ///< Route metric.
};

/// @brief One port-forward rule.
struct PortForward
{
    std::string proto;            ///< tcp | udp.
    uint32_t listen_port = 0;     ///< External port.
    std::string dest_ip;          ///< Internal destination.
    uint32_t dest_port = 0;       ///< Internal port.
};

/// @brief One remote-access SSH window (time-boxed, attributed).
struct SshWindow
{
    std::string operator_name;         ///< Who opened it.
    int64_t opened_unix_s = 0;         ///< Opening time.
    int64_t expires_unix_s = 0;        ///< Automatic expiry.
    bool active = true;                ///< False once closed or expired.
};

/// @brief Time-source policy (networkd owns the daemons; istate owns the policy, §R8).
struct TimePolicy
{
    std::vector<std::string> source_priority = {"gnss", "ptp", "ntp"};   ///< Locked preference order.
    std::vector<std::string> ntp_servers;    ///< NTP pool servers.
    uint32_t ptp_domain = 0;                 ///< PTP domain number.
    std::string ptp_profile;                 ///< e.g. "g8275-1".
    std::vector<std::string> gnss_constellations = {"gps", "galileo", "glonass", "beidou"};   ///< Constellations to track.
};

/// @brief WireGuard tunnel configuration (device ↔ HQ).
struct WgConfig
{
    std::string hq_endpoint;     ///< HQ tunnel endpoint.
    std::string device_addr;     ///< Device tunnel address.
    std::string hq_addr;         ///< HQ tunnel address.
    std::string peer_pubkey;     ///< HQ's public key; ours never leaves the device.
    uint32_t listen_port = 51820;   ///< Local listen port.
};

/// @brief The complete network plan rendered from the delivered configuration.
struct NetworkPlan
{
    std::vector<IfaceConfig> ifaces;        ///< Interface configuration.
    std::vector<FirewallRule> firewall;     ///< Firewall rules.
    std::vector<PortForward> forwards;      ///< Port forwards.
    std::vector<RouteConfig> routes;        ///< Static routes.
    DnsConfig dns;                          ///< Resolver configuration.
    WirelessConfig wireless;                ///< Wireless configuration.
    bool ssh_enabled = false;               ///< Inbound SSH off by default (§7.2).
    WgConfig wg;                            ///< Tunnel configuration.
    bool wg_up = false;                     ///< Desired tunnel state.
    TimePolicy time;                        ///< Time policy.
};

/// @brief Render a plan to the JSON shape handed to platform.apply_network_plan.
/// @param plan Plan to render.
/// @return The plan as the apply-seam JSON shape.
JValue render_plan_json(const NetworkPlan& plan);

/// @brief Tunnel-loss guard (mandatory, §7.2): any change that kills the tunnel auto-reverts.
class TunnelGuard
{
  public:
    /// @brief Outcome of a guarded change.
    struct Result
    {
        bool applied = false;     ///< The new plan was applied.
        bool reverted = false;    ///< The previous plan was restored.
        std::string detail;       ///< Human-readable outcome.
    };
    /// @brief Apply @p next and revert to @p previous when HQ becomes unreachable within the timeout.
    ///
    /// @param previous Currently running plan (revert target).
    /// @param next Candidate plan.
    /// @param apply Applies a plan.
    /// @param reachability Can HQ be reached now?
    /// @param timeout_s Grace period before reverting.
    /// @param clock_s Monotonic seconds.
    /// @return What happened (applied, reverted, detail).
    static Result guarded_change(const NetworkPlan& previous, const NetworkPlan& next, const std::function<Error(const NetworkPlan&)>& apply, const std::function<bool()>& reachability, double timeout_s, const std::function<double()>& clock_s);
};

/// @brief Remote access: off by default, time-boxed, attributed, audited.
class RemoteAccessManager
{
  public:
    /// @brief Open a window; expires automatically.
    ///
    /// Logged as an alarm the customer can see (netd_remote_access_opened).
    ///
    /// @param operator_name Who requested it.
    /// @param ttl_s Requested lifetime (clamped to max_ttl_s).
    /// @param now_unix_s Current wall time.
    /// @return The opened window, or an error when access is disabled.
    Result<SshWindow> open(const std::string& operator_name, uint64_t ttl_s, int64_t now_unix_s);
    /// @brief Expire windows whose TTL elapsed.
    /// @return The windows expired by this sweep.
    /// @param now_unix_s Current wall time.
    std::vector<SshWindow> sweep(int64_t now_unix_s);
    /// @brief Windows still open at @p now_unix_s.
    /// @param now_unix_s Current wall time.
    /// @return Windows still open.
    std::vector<SshWindow> active(int64_t now_unix_s) const;
    /// @brief True when any window is open.
    /// @param now_unix_s Current wall time.
    /// @return True when any window is open.
    bool any_active(int64_t now_unix_s) const;
    /// @brief Close every window immediately.
    void close_all();
    /// @brief Upper bound on a window; longer requests are clamped, never granted (§7.2).
    /// @param s Window cap in seconds.
    void set_max_ttl_s(uint64_t s)
    {
        max_ttl_s_ = s;
    }
    /// @brief Configured window cap.
    uint64_t max_ttl_s() const
    {
        return max_ttl_s_;
    }

  private:
    std::vector<SshWindow> windows_;      ///< Open (and recently closed) windows.
    uint64_t max_ttl_s_ = 8 * 3600;      ///< Maximum window lifetime.
};

/// @brief Time-daemon configuration rendering (networkd owns chrony, ptp4l, phc2sys, gpsd).
struct TimeDaemonConfig
{
    std::string chrony_conf;    ///< Rendered chrony.conf.
    std::string ptp4l_args;     ///< ptp4l command line.
    std::string phc2sys_args;   ///< phc2sys command line.
    std::string gpsd_args;      ///< gpsd command line.
};

/// @brief Render the time daemons' configuration from the policy.
/// @param policy Time policy from istate.
/// @return The rendered daemon configurations.
TimeDaemonConfig render_time_daemons(const TimePolicy& policy);

/// @brief Wireless rendering result (§7.2).
///
/// The PSK is resolved by the caller from /pst at apply time and is never part
/// of the rendered text held in memory or logged.
struct WirelessRender
{
    std::string wpa_supplicant_conf;   ///< Client mode configuration.
    std::string hostapd_conf;          ///< AP mode configuration.
    std::string iface;                 ///< Interface the configuration applies to.
    std::string mode;                  ///< client | ap | off.
};

/// @brief Render the supplicant / AP configuration for a wireless plan.
/// @param w Wireless configuration.
/// @param psk PSK resolved by the caller from /pst.
/// @return The rendered supplicant/AP configuration.
WirelessRender render_wireless(const WirelessConfig& w, const std::string& psk);

/// @brief One interface's reachability probe result.
struct Reachability
{
    std::string iface;          ///< Interface probed.
    bool reachable = false;     ///< Whether the probe succeeded.
    uint64_t rtt_ms = 0;        ///< Round-trip time.
};

/// @brief Connectivity / failover ranking over configured interfaces.
class ConnectivityRanker
{
  public:
    /// @brief Rank reachable interfaces by configured priority, unreachable last.
    /// @param cfgs Configured interfaces (with priorities).
    /// @param probes Probe results per interface.
    /// @return Interface names, best first.
    static std::vector<std::string> rank(const std::vector<IfaceConfig>& cfgs, const std::vector<Reachability>& probes);
};

/// @brief Effective-state snapshot for status reporting.
/// @param plan Current plan.
/// @param windows Open SSH windows.
/// @param tunnel_up Tunnel state.
/// @return The effective-state snapshot as JSON.
std::string plan_status_json(const NetworkPlan& plan, const std::vector<SshWindow>& windows, bool tunnel_up);

} // namespace domain
} // namespace networkd
} // namespace instr