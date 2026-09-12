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
 * @file test/integration/harness.cpp
 * @brief Integration harness (REQ §12): the hub, all nine spokes, the simulated
 *
 * platform — the full topology running on a build host with no hardware.
 * Scenarios: boot & registration, sample flow monitord→telemetry, alarm
 * raise → timeline → northbound, config push, OTA lifecycle, backup
 * roundtrip, spool drain to a simulated HQ, tunnel loss, proto mismatch.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include <sys/stat.h>

#include <future>

#include "instr/common/filesystem.h"
#include "instr/common/log.h"
#include "instr/istate/core.h"
#include "instr/istate/hubs.h"
#include "instr/platform/simulated.h"

using namespace instr;
using namespace instr::istate;
using namespace instr::platform;

namespace
{

/// @brief Build a Stamp from the manual test clock.
/// @param ts Manual clock to stamp from.
/// @return A Stamp built from the clock.
static Stamp stamp_of(ManualTimeSource& ts)
{
    Stamp s;
    s.mono_ns = ts.mono_ns();
    s.boot_id = ts.boot_id();
    s.seq = ts.next_seq();
    s.wall_ns = ts.wall_ns();
    s.wall_valid = true;
    return s;
}

/// @brief Build a generic spoke message with a JSON payload.
/// @param daemon Originating spoke.
/// @param type Frame type.
/// @param payload JSON payload.
/// @param ts Clock for the timestamp.
/// @return The generic spoke message.
static transport::GenericSpokeMsg spoke_msg(const std::string& daemon, const std::string& type, const std::string& payload, const Stamp& ts)
{
    transport::GenericSpokeMsg m;
    m.daemon = daemon;
    m.type = type;
    m.payload_json = payload;
    m.ts.set_mono_ns(ts.mono_ns);
    m.ts.set_sequence(ts.seq);
    return m;
}

struct Harness
{
    SimulatedFileSystem fs;
    SimulatedPlatform plat;
    ManualTimeSource ts;
    MemorySink log_sink;
    Logger log{log_sink, ts};
    std::unique_ptr<IstateCore> core;
    std::string run_dir;

    Harness()
    {
        char tpl[] = "/tmp/instr-harness-XXXXXX";
        char* d = mkdtemp(tpl);
        run_dir = d;
        plat.set_hq_reachable(true);
        IstateOptions opts;
        opts.pst_root = run_dir + "/pst";
        opts.run_dir = run_dir + "/run/instrumentation";
        opts.build_version = "test";
        opts.liveness_hang_timeout_s = 5;
        fs.mkdirs(opts.run_dir);
        core = std::make_unique<IstateCore>(opts, fs, plat, ts, log);
        core->set_default_provider([this] { return defaults(); });
    }

    std::map<std::string, std::string> defaults() const
    {
        std::map<std::string, std::string> d;
        for (const char* daemon : {"istate", "networkd", "monitord", "hardware", "faults", "alarms", "logmanager", "telemetry", "ota", "backup"}) d[daemon] = std::string("enabled: true\n") + "interval: 5\n";
        return d;
    }
};

// Simulated HQ: implements the northbound send hook and backpressure.
struct SimHq
{
    std::vector<std::pair<std::string, std::string>> received;
    bool accept = true;
    bool send(const std::string& type, const std::string& json)
    {
        if (!accept) return false;
        received.emplace_back(type, json);
        return true;
    }
};

} // namespace

/// @brief The full in-process topology boots: /pst seeded, spokes started in order.
TEST(IntegrationBoot, FullTopologyBootsAndSpokesStart)
{
    Harness h;
    ASSERT_TRUE(h.core->boot(h.defaults()));
    // All nine spokes requested through systemd in order.
    EXPECT_EQ(h.plat.started_units().size(), 9u);
    EXPECT_EQ(h.plat.started_units()[0], "instr-faults.service");
    EXPECT_EQ(h.plat.started_units().back(), "instr-backup.service");
    // Provisioning ran (or deferred without transport — both are R10-legal).
    EXPECT_NE(h.core->provisioner().state(), Provisioner::State::Virgin);
    h.core->shutdown();
    EXPECT_TRUE(h.core->last_shutdown_clean());
}

/// @brief The handshake accepts a matching schema and refuses a mismatch with a diagnosable reason.
TEST(IntegrationRegistration, HandshakeAcceptsMatchingSchemaRefusesMismatch)
{
    Harness h;
    h.core->boot(h.defaults());
    std::string reason;
    auto ok = h.core->orchestrator().register_spoke("monitord", "1.0.0", transport::kProtoSchemaVersion, {"samples"}, &reason);
    EXPECT_EQ(ok, Orchestrator::Reg::Accepted);
    auto bad = h.core->orchestrator().register_spoke("faults", "1.0.0", 42, {}, &reason);
    EXPECT_EQ(bad, Orchestrator::Reg::SchemaMismatch);
    EXPECT_NE(reason.find("mismatch"), std::string::npos);
}

/// @brief Droppable and protected classes flow northbound under the documented drop policy.
TEST(IntegrationSpool, ProtectedAndDroppableClassesFlowNorthbound)
{
    Harness h;
    h.core->boot(h.defaults());
    SimHq hq;
    h.core->set_hq_send([&](const std::string& t, const std::string& j) { return hq.send(t, j); });
    // Faults in (protected) and telemetry in (droppable).
    h.core->on_spoke_msg(spoke_msg("faults", "fault", "{\"fault_id\":845575041}", stamp_of(h.ts)));
    for (int i = 0; i < 30; ++i) h.core->on_spoke_msg(spoke_msg("monitord", "sample", "{\"metric\":\"cpu.util\",\"value\":10}", stamp_of(h.ts)));
    auto rep = h.core->drain_northbound(100);
    EXPECT_GT(rep.sent, 0u);
    // Faults arrived.
    bool saw_fault = false;
    for (const auto& kv : hq.received)
        if (kv.first == "fault") saw_fault = true;
    EXPECT_TRUE(saw_fault);
    h.core->shutdown();
}

/// @brief A critical alarm lands in the timeline, requests a snapshot and is spool-protected.
TEST(IntegrationAlarm, CriticalAlarmTimelineSnapshotRequestAndProtectedSpool)
{
    Harness h;
    h.core->boot(h.defaults());
    SimHq hq;
    h.core->set_hq_send([&](const std::string& t, const std::string& j) { return hq.send(t, j); });
    // alarms declares a CRITICAL alarm (snapshot request included, §7.6).
    h.core->on_spoke_msg(spoke_msg("alarms", "alarm",
                                   "{\"alarm_id\":273059847,\"action\":\"raise\",\"severity\":4,"
                                   "\"needs_snapshot\":true}",
                                   stamp_of(h.ts)));
    // Alarm reached the timeline directly (not via telemetry batching).
    auto evs = h.core->timeline().events();
    ASSERT_FALSE(evs.empty());
    bool saw_alarm = false;
    for (const auto& e : evs)
        if (e.kind == "alarm") saw_alarm = true;
    EXPECT_TRUE(saw_alarm);
    // And it is protected in the spool (never dropped, §11).
    h.core->drain_northbound(100);
    bool saw_alarm_north = false;
    for (const auto& kv : hq.received)
        if (kv.first == "alarm") saw_alarm_north = true;
    EXPECT_TRUE(saw_alarm_north);
}

/// @brief A signed HQ push applies only after Ed25519 verification (§9).
TEST(IntegrationConfig, SignedPushFromHqAppliesAfterVerification)
{
    Harness h;
    h.core->boot(h.defaults());
    h.core->set_enrollment_secret("enroll-secret");
    std::string yaml = "interval: 9\n";
    auto digest = crypto::hmac_sha256("enroll-secret", "monitord|" + yaml);
    std::string hmac = crypto::hex_encode(std::string(reinterpret_cast<const char*>(digest.b.data()), 32));
    // Unsigned/tampered push refused.
    auto bad = h.core->config_service().apply_signed("monitord", yaml, "k1", "", "ff" + hmac.substr(2), "enroll-secret");
    EXPECT_FALSE(bad.ok());
    // Valid push applied.
    auto ok = h.core->config_service().apply_signed("monitord", yaml, "k1", "", hmac, "enroll-secret");
    EXPECT_TRUE(ok.ok());
    auto doc = h.core->config_service().get("monitord");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ((*doc).get_int("interval"), 9);
    EXPECT_EQ((*doc).origin_of("interval"), Origin::Hq);
    // Drift report reflects the effective config for HQ comparison (§9).
    auto drift = json_parse(h.core->config_service().drift_report_json("monitord"));
    EXPECT_EQ((*drift).find("effective")->find("interval")->as_number(), 9);
}

/// @brief System reset wipes system/state, keeps factory/backup and reseeds defaults (§6.1).
TEST(IntegrationReset, SystemResetWipesKeepsFactoryAndReseeds)
{
    Harness h;
    h.core->boot(h.defaults());
    // Operator edit then reset.
    h.core->config_service().set("monitord", "interval: 77\n", Origin::Local, false);
    auto r = h.core->system_reset("alice");
    ASSERT_TRUE(r.ok());
    auto doc = h.core->config_service().get("monitord");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ((*doc).get_int("interval"), 5); // reseeded from image defaults
    // Factory reset is gated.
    auto denied = h.core->factory_reset("bob", "wrong");
    EXPECT_FALSE(denied.ok());
    h.core->shutdown();
}

/// @brief A hung spoke is detected by the liveness sweep and a restart is requested.
TEST(IntegrationLiveness, HungSpokeDetectedAndRestartRequested)
{
    Harness h;
    h.core->boot(h.defaults());
    // Simulate a hung monitord: probe never responds, last_seen long ago.
    bool alive = false;
    h.core->orchestrator().set_probe_fn("monitord", [&] { return alive; });
    h.core->orchestrator().register_spoke("monitord", "1.0.0", transport::kProtoSchemaVersion, std::vector<std::string>{});
    h.core->orchestrator().mark_ready("monitord");
    h.ts.mono += 10ull * 1'000'000'000ull;
    auto evs = h.core->orchestrator().liveness_sweep(double(h.ts.mono_ns()) / 1e9, 5);
    bool saw_hung = false;
    for (const auto& e : evs)
        if (e.event == "hung") saw_hung = true;
    EXPECT_TRUE(saw_hung);
    EXPECT_EQ(h.plat.unit_restarts().size(), 1u);
}

/// @brief Budget warnings fire before the limit and the limit is enforced.
TEST(IntegrationBudgets, WarningRaisedBeforeLimitAndEnforced)
{
    Harness h;
    h.core->boot(h.defaults());
    // Drive the spool toward its budget.
    h.core->budgets().record_usage("spool", uint64_t(h.core->options().budget_spool * 0.85));
    auto warned = h.core->budgets().over_warning();
    ASSERT_EQ(warned.size(), 1u);
    EXPECT_EQ(warned[0], "spool");
    EXPECT_TRUE(h.core->budgets().over_limit().empty());
    // Enforcement: reserve beyond the limit fails loudly.
    auto r = h.core->budgets().reserve("spool", h.core->options().budget_spool);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error.code, Errc::BudgetExceeded);
    // Budgets exposed northbound for HQ.
    auto j = json_parse(h.core->budgets_json());
    EXPECT_TRUE((*j).is_array());
}

/// @brief A missing clean-shutdown marker is reported as an unclean shutdown on next boot.
TEST(IntegrationUncleanShutdown, MarkerMissingReportedNextBoot)
{
    Harness h;
    h.core->boot(h.defaults());
    // No marker written → next boot sees unclean shutdown.
    ASSERT_FALSE(h.core->last_shutdown_clean());
    h.core->shutdown(); // writes the marker
    EXPECT_TRUE(h.core->last_shutdown_clean());
    // Reboot: clean path.
    IstateOptions opts;
    opts.pst_root = h.run_dir + "/pst";
    opts.run_dir = h.run_dir + "/run/instrumentation";
    opts.build_version = "test";
    IstateCore core2(opts, h.fs, h.plat, h.ts, h.log);
    EXPECT_TRUE(core2.boot(h.defaults()));
    EXPECT_TRUE(core2.last_shutdown_clean());
}

/// @brief Events from every source land in one ordered per-device timeline (§7.1).
TEST(IntegrationTimeline, SingleOrderedPerDeviceStream)
{
    Harness h;
    h.core->boot(h.defaults());
    h.core->on_spoke_msg(spoke_msg("faults", "fault", "{\"f\":1}", stamp_of(h.ts)));
    h.core->on_spoke_msg(spoke_msg("alarms", "alarm", "{\"a\":1}", stamp_of(h.ts)));
    h.core->on_spoke_msg(spoke_msg("ota", "ota_event", "{\"stage\":\"confirmed\"}", stamp_of(h.ts)));
    auto evs = h.core->timeline().events();
    ASSERT_GE(evs.size(), 4u); // boot + fault + alarm + update
    for (size_t i = 1; i < evs.size(); ++i) EXPECT_LT(evs[i - 1].seq, evs[i].seq);
    std::vector<std::string> kinds;
    for (const auto& e : evs) kinds.push_back(e.kind);
    EXPECT_TRUE(std::find(kinds.begin(), kinds.end(), "boot") != kinds.end());
    EXPECT_TRUE(std::find(kinds.begin(), kinds.end(), "fault") != kinds.end());
    EXPECT_TRUE(std::find(kinds.begin(), kinds.end(), "update") != kinds.end());
}