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
 * @file daemons/networkd/test/test_domain.cpp
 * @brief networkd domain (REQ §12)
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include "instr/common/timestamps.h"
#include "instr/networkd/daemon.h"
#include "instr/platform/simulated.h"

using namespace instr;
using namespace instr::networkd;
using namespace instr::networkd::domain;

/// @brief A plan renders every section (interfaces, firewall, routes, dns, wireless, wg, time).
TEST(NetworkdPlan, RendersCompletePlan)
{
    NetworkPlan p;
    IfaceConfig eth;
    eth.name = "eth0";
    eth.mode = "dhcp";
    eth.priority = 10;
    p.ifaces.push_back(eth);
    FirewallRule r;
    r.action = "deny";
    r.proto = "tcp";
    r.src = "any";
    r.dst_port = 23;
    r.comment = "telnet";
    p.firewall.push_back(r);
    p.ssh_enabled = false;
    p.wg.hq_endpoint = "hq.example:51820";
    p.wg_up = true;
    auto j = render_plan_json(p);
    ASSERT_TRUE(j.is_object());
    EXPECT_TRUE(j.has("interfaces"));
    EXPECT_TRUE(j.has("firewall"));
    EXPECT_TRUE(j.has("wireguard"));
    EXPECT_FALSE(j.find("ssh_enabled")->as_bool());
    EXPECT_TRUE(j.find("wireguard")->find("up")->as_bool());
}

/// @brief The tunnel guard keeps a change when HQ stays reachable.
TEST(NetworkdTunnelGuard, KeepsChangeWhenHqReachable)
{
    NetworkPlan prev, next;
    double now = 100;
    auto res = TunnelGuard::guarded_change(prev, next, [](const NetworkPlan&) { return Error::none(); }, [] { return true; }, 5.0, [&now] { return now; });
    EXPECT_TRUE(res.applied);
    EXPECT_FALSE(res.reverted);
}

/// @brief The guard reverts when HQ is unreachable past the timeout (§7.2).
TEST(NetworkdTunnelGuard, RevertsWhenHqUnreachablePastTimeout)
{
    // Mandatory behaviour (§7.2): firewall change that kills the tunnel
    // auto-reverts if HQ is not reachable within the timeout.
    NetworkPlan prev, next;
    double now = 100;
    int applies = 0;
    auto res = TunnelGuard::guarded_change(
        prev, next,
        [&](const NetworkPlan&) {
            ++applies;
            return Error::none();
        },
        [] { return false; }, 0.5, // never reachable
        [&now] {
            now += 0.1; // test clock advances per probe
            return now;
        });
    EXPECT_TRUE(res.applied);
    EXPECT_TRUE(res.reverted);
    EXPECT_EQ(applies, 2); // next applied, then previous re-applied
    EXPECT_NE(res.detail.find("auto-reverted"), std::string::npos);
}

/// @brief SSH windows expire automatically at their TTL.
TEST(NetworkdRemoteAccess, OpenExpiresAutomatically)
{
    RemoteAccessManager mgr;
    auto w = mgr.open("alice", 60, 1'700'000'000);
    ASSERT_TRUE(w.ok());
    EXPECT_TRUE(mgr.any_active(1'700'000'030));
    auto expired = mgr.sweep(1'700'000'061);
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired[0].operator_name, "alice");
    EXPECT_FALSE(mgr.any_active(1'700'000'062));
}

/// @brief Windows require attribution and are clamped to the TTL cap.
TEST(NetworkdRemoteAccess, RequiresAttributionAndBoundsTtl)
{
    RemoteAccessManager mgr;
    EXPECT_FALSE(mgr.open("", 60, 0).ok()); // must be attributed
    EXPECT_FALSE(mgr.open("bob", 0, 0).ok()); // ttl > 0
    EXPECT_FALSE(mgr.open("bob", 25 * 3600ull, 0).ok()); // ttl ≤ 24h
}

/// @brief The time policy renders chrony/ptp4l/phc2sys configuration.
TEST(NetworkdTimeDaemons, RendersChronyAndPtp)
{
    TimePolicy p;
    p.ntp_servers = {"pool.example.org"};
    p.ptp_domain = 7;
    p.ptp_profile = "g8275-1";
    auto c = render_time_daemons(p);
    EXPECT_NE(c.chrony_conf.find("server pool.example.org iburst"), std::string::npos);
    EXPECT_NE(c.chrony_conf.find("refclock SHM 0"), std::string::npos); // gnss priority
    EXPECT_NE(c.ptp4l_args.find("-d 7"), std::string::npos);
    EXPECT_NE(c.ptp4l_args.find("--profile=g8275-1"), std::string::npos);
}

/// @brief Failover ranking prefers reachable interfaces by priority.
TEST(NetworkdFailover, RanksReachablePreferredInterfacesFirst)
{
    IfaceConfig a, b;
    a.name = "eth0";
    a.priority = 5;
    b.name = "wwan0";
    b.priority = 10;
    std::vector<IfaceConfig> cfgs = {b, a};
    std::vector<Reachability> probes = {{"eth0", false, 0}, {"wwan0", true, 20}};
    auto ranked = ConnectivityRanker::rank(cfgs, probes);
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0], "wwan0"); // reachable beats lower-priority-but-down
}

/// @brief A guarded apply through the platform records the revert event.
TEST(NetworkdCore, GuardedApplyThroughPlatformRecordsRevert)
{
    platform::SimulatedPlatform plat;
    ManualTimeSource ts;
    NetworkdCore core(plat, ts);
    NetworkPlan next;
    core.set_reachability_fn([] { return false; });
    double t = 0;
    core.set_clock_fn([&t] {
        t += 0.05; // test clock advances per probe
        return t;
    });
    auto res = core.guarded_apply(next, 0.4);
    EXPECT_TRUE(res.reverted);
    EXPECT_GE(plat.applied_plans().size(), 2u);
}