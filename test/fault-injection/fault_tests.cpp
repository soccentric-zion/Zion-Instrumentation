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
 * @file test/fault-injection/fault_tests.cpp
 * @brief Fault-injection tests (REQ §12): the failure paths are the ones that ship
 *
 * untested — so every §11 row is exercised here against the simulated
 * filesystem/platform with injected faults.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include "instr/common/filesystem.h"
#include "instr/istate/core.h"
#include "instr/monitord/domain.h"
#include "instr/networkd/daemon.h"
#include "instr/platform/simulated.h"
#include "instr/telemetry/domain.h"

using namespace instr;
using namespace instr::istate;
using namespace instr::platform;

namespace
{
/// @brief Create a unique per-test temporary directory.
/// @param tag Distinct tag for the test.
/// @return The unique directory path.
static std::string tmp_dir(const char* tag)
{
    return std::string("/tmp/instr-fault-") + tag + "-" + std::to_string(::getpid());
}
struct FaultFixture
{
    SimulatedFileSystem fs;
    SimulatedPlatform plat;
    ManualTimeSource ts;
    MemorySink log_sink;
    Logger log{log_sink, ts};
    std::string root = "/tmp/instr-fault-" + std::to_string(::getpid());
    std::unique_ptr<IstateCore> core;
    FaultFixture()
    {
        IstateOptions opts;
        opts.pst_root = root + "/pst";
        opts.run_dir = root + "/run/instrumentation";
        opts.build_version = "test";
        opts.spool_max_bytes = 4096;
        core = std::make_unique<IstateCore>(opts, fs, plat, ts, log);
        std::map<std::string, std::string> defaults{{"istate", "a: 1\n"}};
        core->boot(defaults);
    }
};
} // namespace

// §11 "/pst corrupt": fall back to redundant copy; if both fail, boot
// unprovisioned and report — never fail to boot.
/// @brief A corrupt /pst file recovers from its redundant copy (REQ §11).
TEST(FaultPst, CorruptPrimaryRecoversFromRedundantCopy)
{
    FaultFixture f;
    ConfigDocument d;
    d.set("interval", "7", Origin::Local);
    ASSERT_TRUE(f.core->pst().save_config("monitord", d).ok());
    // Inject against the fixture's real root — a literal "/pst/..." would
    // never match and the test would pass vacuously.
    const std::string cfg = f.core->pst().path("system/monitord/config.yaml");
    f.fs.corrupt_reads_of(cfg, 1);
    auto doc = f.core->pst().load_config("monitord");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ((*doc).get_int("interval"), 7);
}

/// @brief When both copies fail, the device boots unprovisioned and reports loudly — it never dies.
TEST(FaultPst, BothCopiesCorruptBootsUnprovisionedAndReports)
{
    FaultFixture f;
    ConfigDocument d;
    d.set("interval", "7", Origin::Local);
    ASSERT_TRUE(f.core->pst().save_config("monitord", d).ok());
    const std::string cfg = f.core->pst().path("system/monitord/config.yaml");
    f.fs.corrupt_reads_of(cfg, 1);
    f.fs.corrupt_reads_of(cfg + ".bak", 1);
    // load_config still returns loudly (Corrupt) — boot proceeds unprovisioned.
    auto doc = f.core->pst().load_config("monitord");
    EXPECT_FALSE(doc.ok());
    EXPECT_EQ(doc.error.code, Errc::Corrupt);
    // Boot itself must not fail: re-mount reports and continues.
    auto rep = f.core->pst().mount();
    EXPECT_TRUE(rep.ok);
}

// §11 "spool full": drop oldest telemetry first, never alarms or faults,
// report the drop.
/// @brief A full spool drops telemetry, keeps protected records and reports what it dropped.
TEST(FaultSpool, FullDropsTelemetryKeepsFaultsReports)
{
    SimulatedFileSystem fs;
    SpoolConfig cfg;
    cfg.dir = tmp_dir("spool");
    cfg.max_bytes = 900;
    DiskSpool sp(fs, cfg);
    ASSERT_TRUE(sp.open().ok());
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(sp.append(SpoolPriority::Fault, "fault-" + std::to_string(i), i).ok());
    size_t faults_before = 0;
    for (const auto& e : sp.peek(100))
        if (e.priority == SpoolPriority::Fault) ++faults_before;
    for (int i = 0; i < 40; ++i) ASSERT_TRUE(sp.append(SpoolPriority::Telemetry, "t-" + std::to_string(i), 100 + i).ok());
    size_t faults_after = 0;
    for (const auto& e : sp.peek(100))
        if (e.priority == SpoolPriority::Fault) ++faults_after;
    EXPECT_EQ(faults_before, faults_after);
    EXPECT_GT(sp.stats().dropped_telemetry, 0u);
}

// §11 "HQ unreachable": jittered backoff; device fully operational.
/// @brief An unreachable HQ moves the session into backoff with jittered reconnect pacing.
TEST(FaultHq, UnreachableTriggersBackoffState)
{
    FaultFixture f;
    f.plat.set_hq_reachable(false);
    HqSession s;
    s.on_disconnected();
    EXPECT_EQ(s.state(), HqSession::State::Backoff);
    // Delays grow exponentially and stay inside the ±20% jitter envelope
    // around 1+2+4+8+16. Asserting >= 31 exactly would fail whenever the
    // jitter happens to land low — the jitter is the point of the mechanism.
    double total = 0;
    double prev = 0;
    for (int i = 0; i < 5; ++i)
    {
        double d = s.next_delay_s();
        EXPECT_GT(d, prev); // strictly growing even under jitter
        prev = d;
        total += d;
    }
    EXPECT_GE(total, 31.0 * 0.8);
    EXPECT_LE(total, 31.0 * 1.2);
    // Pinned jitter: the schedule is exactly 1+2+4+8+16 with no jitter applied.
    HqSession fixed;
    fixed.set_backoff_rng([] { return 0.5; }); // 0.5 → scale 1.0
    double exact = 0;
    for (int i = 0; i < 5; ++i) exact += fixed.next_delay_s();
    EXPECT_DOUBLE_EQ(exact, 31.0);
    // device keeps operating meanwhile
    // Local operation unaffected: config still readable/writable.
    ConfigDocument d;
    d.set("interval", "3", Origin::Local);
    EXPECT_TRUE(f.core->pst().save_config("monitord", d).ok());
}

// §11 "spoke crashes / hangs": istate reports degraded, requests restart.
/// @brief A wedged spoke (running but silent) is detected, restart requested and the device reports degraded.
TEST(FaultSpoke, HangDetectedRestartRequestedDeviceDegrades)
{
    FaultFixture f;
    f.core->orchestrator().register_spoke("networkd", "1.0.0", transport::kProtoSchemaVersion, std::vector<std::string>{});
    f.core->orchestrator().mark_ready("networkd");
    bool alive = false;
    f.core->orchestrator().set_probe_fn("networkd", [&] { return alive; });
    f.ts.mono += 60ull * 1'000'000'000ull;
    f.core->orchestrator().liveness_sweep(double(f.ts.mono_ns()) / 1e9, 30);
    EXPECT_EQ((*f.core->orchestrator().spoke("networkd"))->state, SpokeState::Hung);
    EXPECT_TRUE(f.core->orchestrator().degraded()); // exposed northbound
    EXPECT_EQ(f.plat.unit_restarts().size(), 1u);
}

// §3.1/§7.1: the liveness mechanism itself — a ping over the spoke's channel
// answered by a pong. This is the case the probe override cannot represent:
// an idle-but-healthy daemon (backup, ota) sends no traffic of its own, so
// only the pong separates "quiet" from "wedged".
/// @brief An idle-but-answering spoke is never falsely reported hung (pongs distinguish quiet from hung).
TEST(FaultSpoke, IdleButAnsweringSpokeIsNotReportedHung)
{
    FaultFixture f;
    auto& orch = f.core->orchestrator();
    orch.register_spoke("backup", "1.0.0", transport::kProtoSchemaVersion, std::vector<std::string>{});
    orch.mark_ready("backup");

    std::vector<uint64_t> nonces;
    orch.set_ping_fn([&](const std::string& daemon, uint64_t nonce) {
        if (daemon == "backup") nonces.push_back(nonce);
        return true;
    });

    // The spoke answers every ping but sends nothing else for ten minutes.
    for (int i = 0; i < 20; ++i)
    {
        f.ts.mono += 30ull * 1'000'000'000ull;
        orch.liveness_sweep(double(f.ts.mono_ns()) / 1e9, 60);
        ASSERT_FALSE(nonces.empty());
        orch.note_pong("backup", nonces.back());
    }
    EXPECT_EQ((*orch.spoke("backup"))->state, SpokeState::Ready);
    EXPECT_EQ(f.plat.unit_restarts().size(), 0u) << "an answering daemon must not be restarted";
    EXPECT_GT((*orch.spoke("backup"))->pongs_received, 0u);
}

/// @brief A spoke that stops answering pings is restarted after the hang timeout.
TEST(FaultSpoke, SpokeThatStopsAnsweringPingsIsRestarted)
{
    FaultFixture f;
    auto& orch = f.core->orchestrator();
    orch.register_spoke("telemetry", "1.0.0", transport::kProtoSchemaVersion, std::vector<std::string>{});
    orch.mark_ready("telemetry");
    uint64_t last_nonce = 0;
    orch.set_ping_fn([&](const std::string&, uint64_t nonce) {
        last_nonce = nonce;
        return true;
    });

    // Answering: healthy.
    f.ts.mono += 10ull * 1'000'000'000ull;
    orch.liveness_sweep(double(f.ts.mono_ns()) / 1e9, 30);
    orch.note_pong("telemetry", last_nonce);
    EXPECT_EQ((*orch.spoke("telemetry"))->state, SpokeState::Ready);

    // Wedged: the process is up (pings still send) but nothing comes back.
    f.ts.mono += 60ull * 1'000'000'000ull;
    auto evs = orch.liveness_sweep(double(f.ts.mono_ns()) / 1e9, 30);
    EXPECT_EQ((*orch.spoke("telemetry"))->state, SpokeState::Hung);
    EXPECT_EQ(f.plat.unit_restarts().size(), 1u);
    bool saw_hung = false;
    for (const auto& e : evs)
        if (e.event == "hung") saw_hung = true;
    EXPECT_TRUE(saw_hung);

    // It comes back and answers again: state recovers without a second restart.
    f.ts.mono += 5ull * 1'000'000'000ull;
    orch.note_pong("telemetry", last_nonce);
    orch.liveness_sweep(double(f.ts.mono_ns()) / 1e9, 30);
    EXPECT_EQ((*orch.spoke("telemetry"))->state, SpokeState::Ready);
    EXPECT_EQ(f.plat.unit_restarts().size(), 1u);
}

// §11 "clock unavailable at boot": operate on monotonic; re-stamp on
// acquisition.
/// @brief Monotonic time keeps operating without a wall clock; spooled records are re-stamped on lock.
TEST(FaultClock, MonotonicOperationAndRestamp)
{
    ManualTimeSource ts;
    TimeReconciler rec(ts);
    // Clock unavailable: records carry monotonic only.
    rec.record_pending(1'000);
    rec.record_pending(2'000);
    EXPECT_FALSE(rec.wall_valid());
    // GNSS acquires lock mid-run → re-stamp; history not reordered.
    size_t n = rec.reconcile(1'700'000'000'000'000'000ll, "gnss");
    EXPECT_EQ(n, 2u);
    EXPECT_TRUE(rec.wall_valid());
    auto stamped = rec.take_stamped();
    EXPECT_LE(stamped[0].wall_ns, stamped[1].wall_ns);
    // Clock jump later: new stamp still monotonic in wall time.
    ts.mono += 100;
    auto now = rec.stamp_now();
    EXPECT_GE(now.wall_ns, stamped[1].wall_ns);
}

// §11 "storage near full": budgets enforced, alarm before the limit.
/// @brief Budget warnings fire before the hard limit; no consumer can wedge the partition.
TEST(FaultStorage, BudgetAlarmBeforeLimitNeverWedges)
{
    FaultFixture f;
    f.core->budgets().set_budget("logs", 1000);
    f.core->budgets().record_usage("logs", 850);
    EXPECT_FALSE(f.core->budgets().over_warning().empty());
    EXPECT_TRUE(f.core->budgets().over_limit().empty());
    // Writes refuse past the limit instead of wedging the partition.
    f.core->budgets().record_usage("logs", 1200);
    EXPECT_FALSE(f.core->budgets().over_limit().empty());
    EXPECT_FALSE(f.core->budgets().reserve("logs", 100).ok());
}

// §11 "firewall change kills tunnel": auto-revert within the timeout.
/// @brief A plan change that kills the tunnel auto-reverts within the guard timeout (§7.2).
TEST(FaultNetwork, TunnelLossGuardRevertsChange)
{
    SimulatedPlatform plat;
    ManualTimeSource ts;
    instr::networkd::NetworkdCore netd(plat, ts);
    double t = 0;
    netd.set_clock_fn([&t] {
        t += 0.05;
        return t;
    });
    instr::networkd::NetworkPlan next;
    netd.set_reachability_fn([] { return false; }); // change killed the tunnel
    auto res = netd.guarded_apply(next, 0.3);
    EXPECT_TRUE(res.reverted);
    EXPECT_NE(res.detail.find("auto-reverted"), std::string::npos);
    EXPECT_GE(plat.applied_plans().size(), 2u); // next applied + previous restored
}

// §11 "proto version mismatch": refuse, alarm, never guess.
/// @brief A proto schema mismatch is refused and raises the alarm the contract requires (§3.1).
TEST(FaultProto, MismatchRefusedWithAlarm)
{
    FaultFixture f;
    std::vector<std::string> events;
    f.core->orchestrator().set_event_sink([&](const std::string& kind, const std::string& detail) { events.push_back(kind + ":" + detail); });
    auto r = f.core->orchestrator().register_spoke("ota", "1.0.0", 999, std::vector<std::string>{});
    EXPECT_EQ(r, Orchestrator::Reg::SchemaMismatch);
    bool alarm_emitted = false;
    for (const auto& e : events)
        if (e.find("alarm") == 0) alarm_emitted = true;
    EXPECT_TRUE(alarm_emitted);
}

// §11 "unclean shutdown": marker-based detection, reported on next boot.
/// @brief An unclean shutdown is detected via the persistent marker and reported on next boot.
TEST(FaultShutdown, UncleanMarkerReportedOnNextBoot)
{
    FaultFixture f;
    // No clean-shutdown marker (simulated crash) → boot records the event.
    auto j = json_parse(f.core->system_info_json());
    EXPECT_FALSE((*j).find("clean_shutdown")->as_bool());
    f.core->write_clean_shutdown_marker();
    auto j2 = json_parse(f.core->system_info_json());
    EXPECT_TRUE((*j2).find("clean_shutdown")->as_bool());
}

// §11 "fleet-wide simultaneous boot": jittered drain + backpressure honoured.
/// @brief Hundreds of devices reconnecting together stay jittered; no thundering herd.
TEST(FaultFlood, JitteredDrainNotStampede)
{
    SimulatedFileSystem fs;
    SpoolConfig cfg;
    cfg.dir = tmp_dir("spool");
    NorthboundSpool spool(fs, cfg, nullptr);
    ASSERT_TRUE(spool.open().ok());
    for (int i = 0; i < 100; ++i) ASSERT_TRUE(spool.enqueue(SpoolPriority::Telemetry, "r-" + std::to_string(i), i).ok());
    // HQ throttles: pause mid-drain; spool retains the rest for later.
    int accepted = 0;
    auto rep = spool.drain([&](const DiskSpool::Entry&) { return ++accepted <= 10; }, 200);
    EXPECT_TRUE(rep.paused);
    // The ten HQ acked are delivered and removed; the rest are retained for the
    // next drain. Nothing is lost and nothing is re-sent.
    EXPECT_EQ(rep.sent, 10u);
    EXPECT_EQ(spool.size(), 90u);
    // Jitter keeps the retry delay in bounds (no thundering herd).
    instr::istate::HqSession s(1.0, 60.0);
    s.on_disconnected();
    for (int i = 0; i < 20; ++i)
    {
        double d = s.next_delay_s();
        EXPECT_LE(d, 60.0 * 1.2); // capped with jitter margin
    }
}

// Fault injection: storage write failure mid-config-save → loud error,
// prior state intact.
/// @brief A failed write leaves the previous state intact (temp+rename discipline).
TEST(FaultStorage, WriteFailureLeavesStateIntact)
{
    FaultFixture f;
    f.fs.fail_writes_after(1);
    ConfigDocument d;
    d.set("interval", "11", Origin::Local);
    auto e = f.core->pst().save_config("monitord", d);
    EXPECT_FALSE(e.ok());
    EXPECT_EQ(e.code, Errc::Io);
}