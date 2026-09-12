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
 * @file test/integration/topology.cpp
 * @brief The real topology: hub + nine spokes over gRPC/UDS (REQ §12).
 *
 * harness.cpp exercises the istate core in-process with synthetic frames,
 * which is useful but is not what §12 asks for: "hub, all nine spokes,
 * simulated platform layer, running on a build host with no hardware".
 *
 * This file starts the actual gRPC server on real Unix domain sockets and
 * dials it with the actual SpokeChannel each daemon uses, so the paths under
 * test are the ones that ship — registration handshake, schema refusal,
 * config delivery and acknowledgement, streaming data, liveness ping/pong,
 * and reconnect. Everything runs on SimulatedPlatform, so no hardware and no
 * privileges are required.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "instr/alarms/daemon.h"
#include "instr/backup/daemon.h"
#include "instr/common/filesystem.h"
#include "instr/common/log.h"
#include "instr/faults/daemon.h"
#include "instr/hardware/daemon.h"
#include "instr/istate/core.h"
#include "instr/istate/hubs.h"
#include "instr/istate/table.h"
#include "instr/logmanager/daemon.h"
#include "instr/monitord/spoke.h"
#include "instr/monitord/table.h"
#include "instr/networkd/daemon.h"
#include "instr/ota/daemon.h"
#include "instr/platform/simulated.h"
#include "instr/telemetry/daemon.h"

using namespace instr;
using namespace instr::istate;
using namespace instr::platform;

namespace
{

// Poll until `pred` holds or the budget elapses. Tests must not sleep for a
// fixed period: on a loaded CI box that is either flaky or slow.
/// @brief Poll a predicate until true or timeout (real-transport tests).
/// @param pred Predicate to poll.
/// @param timeout_ms Poll budget in milliseconds.
/// @return True when the predicate held before the timeout.
template <typename Pred> bool wait_for(Pred pred, int timeout_ms = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

// The full device: istate with its real gRPC server, and every spoke dialling
// in over UDS exactly as the daemons do.
struct Topology
{
    NativeFileSystem real_fs; // sockets live on the real filesystem
    SimulatedFileSystem fs; // /pst is simulated
    SimulatedPlatform plat;
    SystemTimeSource ts;
    MemorySink log_sink;
    Logger log{log_sink, ts};

    std::string dir;
    std::string run_dir;
    IstateOptions opts;
    std::unique_ptr<IstateCore> core;
    std::unique_ptr<grpc::Server> server;
    std::vector<std::unique_ptr<grpc::Service>> services;

    // Spoke cores and their channels — the same objects the daemons build.
    std::unique_ptr<monitord::MonitorCore> mon_core;
    std::unique_ptr<monitord::domain::MonitorSpoke> mon;
    CommandTable mon_table;

    std::unique_ptr<faults::FaultsCore> fault_core;
    std::unique_ptr<faults::FaultsSpoke> flt;
    CommandTable fault_table;

    std::unique_ptr<telemetry::TelemetryCore> tel_core;
    std::unique_ptr<telemetry::TelemetrySpoke> tel;
    CommandTable tel_table;

    Topology()
    {
        // Short path: a Unix socket address is limited to ~108 bytes.
        char tpl[] = "/tmp/instr-topo-XXXXXX";
        dir = ::mkdtemp(tpl);
        run_dir = dir + "/run";
        ::mkdir(run_dir.c_str(), 0755);

        opts.pst_root = "/pst";
        opts.run_dir = run_dir;
        opts.build_version = "topology-test";
        opts.liveness_hang_timeout_s = 60;
        fs.mkdirs("/pst");
        plat.set_hq_reachable(true);

        core = std::make_unique<IstateCore>(opts, fs, plat, ts, log);
        core->set_default_provider([this] { return defaults(); });
        server = build_hub_server(*core, run_dir, &services);
        core->boot(defaults());
    }

    ~Topology()
    {
        if (mon) mon->stop();
        if (flt) flt->stop();
        if (tel) tel->stop();
        if (server)
        {
            server->Shutdown();
            server->Wait();
        }
    }

    std::map<std::string, std::string> defaults() const
    {
        std::map<std::string, std::string> d;
        d["istate"] = "enabled: true\n";
        d["monitord"] = "enabled: true\ninterval: 0.2\n";
        d["faults"] = "enabled: true\npoll_interval_s: 0.2\n";
        d["telemetry"] = "enabled: true\ninterval: 0.5\n";
        for (const char* x : {"networkd", "hardware", "alarms", "logmanager", "ota", "backup"}) d[x] = "enabled: true\n";
        return d;
    }

    std::string target(const std::string& daemon) const
    {
        return transport::uds_target(run_dir + "/" + daemon + ".sock");
    }

    void start_monitord()
    {
        mon_core = std::make_unique<monitord::MonitorCore>(plat, ts);
        mon_table = monitord::build_monitor_table(mon_core.get());
        mon = std::make_unique<monitord::domain::MonitorSpoke>(mon_core.get(), &log, &mon_table, target("monitord"), "1.0.0");
        mon->start();
    }
    void start_faults()
    {
        fault_core = std::make_unique<faults::FaultsCore>();
        fault_table = faults::build_faults_table(fault_core.get());
        flt = std::make_unique<faults::FaultsSpoke>(fault_core.get(), &log, &fault_table, target("faults"), "1.0.0");
        flt->start();
    }
    void start_telemetry()
    {
        tel_core = std::make_unique<telemetry::TelemetryCore>(ts);
        tel_table = telemetry::build_telemetry_table(tel_core.get());
        tel = std::make_unique<telemetry::TelemetrySpoke>(tel_core.get(), &log, &tel_table, target("telemetry"), "1.0.0");
        tel->start();
    }
};

} // namespace

/// @brief istate listens on one UDS per spoke (§3.1 topology).
TEST(Topology, ServerListensOnASocketPerSpoke)
{
    Topology t;
    ASSERT_NE(t.server, nullptr) << "the hub gRPC server did not start";
    // §3.1: one socket per daemon under /run/instrumentation, plus the
    // Configuration peer (§3.3).
    for (const char* d : {"monitord", "networkd", "hardware", "faults", "alarms", "logmanager", "telemetry", "ota", "backup", "configuration"})
    {
        struct stat st;
        const std::string p = t.run_dir + "/" + d + ".sock";
        EXPECT_EQ(::stat(p.c_str(), &st), 0) << "no socket for " << d << " at " << p;
    }
}

/// @brief A spoke completes the real registration handshake over gRPC.
TEST(Topology, SpokeRegistersOverTheRealTransport)
{
    Topology t;
    t.start_monitord();
    ASSERT_TRUE(wait_for([&] { return t.mon->registered(); })) << "monitord never completed the registration handshake";
    // istate sees it as an attached spoke, not merely an expected one.
    ASSERT_TRUE(wait_for([&] {
        auto s = t.core->orchestrator().spoke("monitord");
        return s.ok() && (*s)->state == SpokeState::Ready;
    })) << "the hub never marked monitord ready";
}

/// @brief Config delivery flows over the channel and the spoke acks it (§9).
TEST(Topology, ConfigurationIsDeliveredAndAcknowledgedOverTheChannel)
{
    Topology t;
    t.start_monitord();
    ASSERT_TRUE(wait_for([&] { return t.mon->registered(); }));

    // §R4/§9: the spoke asks on registration and istate answers from /pst. The
    // spoke never opens a config file itself.
    ASSERT_TRUE(wait_for([&] { return t.mon->config_revision() > 0; })) << "monitord never received its configuration";
    // The delivered value actually took effect in the domain object.
    EXPECT_NEAR(t.mon_core->config().interval, 0.2, 1e-9);
    // And istate saw the acknowledgement.
    ASSERT_TRUE(wait_for([&] { return t.core->config_ack_state("monitord").revision > 0; }));
    EXPECT_TRUE(t.core->config_ack_state("monitord").applied);
}

/// @brief A config change pushes to a running spoke without a restart.
TEST(Topology, ConfigChangePropagatesToARunningSpoke)
{
    Topology t;
    t.start_monitord();
    ASSERT_TRUE(wait_for([&] { return t.mon->config_revision() > 0; }));
    const uint64_t first = t.mon->config_revision();

    // An operator changes the interval through istate — the only writer of
    // /pst (R4) — and the running spoke picks it up without a restart.
    auto doc = t.core->pst().load_config("monitord");
    ASSERT_TRUE(doc.ok());
    (*doc).set_number("interval", 1.5, Origin::Local);
    ASSERT_TRUE(t.core->pst().save_config("monitord", *doc).ok());
    ASSERT_TRUE(t.core->deliver_config("monitord"));

    ASSERT_TRUE(wait_for([&] { return t.mon->config_revision() > first; }));
    EXPECT_NEAR(t.mon_core->config().interval, 1.5, 1e-9);
}

/// @brief A schema-invalid delivery is refused and the previous document stays in force (R10).
TEST(Topology, InvalidConfigIsRefusedAndThePreviousOneStaysInForce)
{
    Topology t;
    t.start_monitord();
    ASSERT_TRUE(wait_for([&] { return t.mon->config_revision() > 0; }));

    // thermal_critical_c must exceed thermal_warn_c; the spoke rejects the
    // delivery and keeps running on what it had (R10).
    auto doc = t.core->pst().load_config("monitord");
    ASSERT_TRUE(doc.ok());
    (*doc).set_number("thermal_warn_c", 90, Origin::Hq);
    (*doc).set_number("thermal_critical_c", 50, Origin::Hq);
    ASSERT_TRUE(t.core->pst().save_config("monitord", *doc).ok());
    ASSERT_TRUE(t.core->deliver_config("monitord"));

    // The rejection travels back to istate rather than being swallowed.
    ASSERT_TRUE(wait_for([&] { return !t.core->config_ack_state("monitord").applied; })) << "a refused configuration was reported as applied";
    EXPECT_FALSE(t.core->config_ack_state("monitord").reason.empty());
    // The interval from the last good delivery is untouched.
    EXPECT_NEAR(t.mon_core->config().interval, 0.2, 1e-9);
}

/// @brief monitord samples stream to istate over the real channel.
TEST(Topology, SamplesStreamFromMonitordToIstate)
{
    Topology t;
    t.start_monitord();
    ASSERT_TRUE(wait_for([&] { return t.mon->registered(); }));
    // §3.1: streaming where the data is a stream. The sampler runs on its own
    // interval and the frames arrive at the hub.
    ASSERT_TRUE(wait_for([&] { return t.mon->frames_enqueued() > 0; }, 8000));
    // They land in the northbound spool as telemetry (§7.1).
    EXPECT_TRUE(wait_for([&] { return t.core->northbound_spool().size() > 0; }, 8000));
}

/// @brief The liveness ping/pong works over the real channel (§3.1).
TEST(Topology, LivenessPingIsAnsweredOverTheRealChannel)
{
    Topology t;
    t.start_faults(); // faults sends nothing on its own: the idle case
    ASSERT_TRUE(wait_for([&] { return t.flt->registered(); }));

    auto pongs = [&] {
        auto s = t.core->orchestrator().spoke("faults");
        return s.ok() ? (*s)->pongs_received : 0;
    };
    const uint64_t before = pongs();
    // Sweeping sends a ping; the spoke's transport answers it.
    ASSERT_TRUE(wait_for(
        [&] {
            t.core->orchestrator().liveness_sweep(0, 60);
            return pongs() > before;
        },
        8000))
        << "an idle spoke never answered a liveness ping";
}

/// @brief A proto schema mismatch is refused at registration.
TEST(Topology, ProtoSchemaMismatchIsRefusedAtRegistration)
{
    Topology t;
    // §3.1/§11: istate verifies the schema version and refuses a mismatch
    // rather than guessing. Dial with a deliberately wrong version.
    using ChannelT = transport::SpokeChannel<monitord::MonitorHub, monitord::MonitorSpokeFrame, monitord::MonitorHubFrame>;
    ChannelT::Options o;
    o.target = t.target("monitord");
    o.daemon = "monitord";
    o.build_version = "1.0.0";
    o.proto_version = transport::kProtoSchemaVersion + 99;
    o.backoff_base_s = 0.05;
    o.backoff_max_s = 0.1;
    ChannelT ch(std::move(o), t.log);
    ch.start();
    // It must never register, however long it retries.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_FALSE(ch.registered());
    ch.stop();
}

/// @brief Registration metadata (build, proto, capabilities) reaches the orchestrator.
TEST(Topology, RegistrationMetadataReachesTheOrchestrator)
{
    // §3.1: the handshake carries daemon name, build version, proto schema
    // version and capabilities. The transport checked the version but nothing
    // recorded the rest, so `istate_ctrl spokes` showed every attached spoke
    // with an empty build and proto 0.
    Topology t;
    t.start_monitord();
    ASSERT_TRUE(wait_for([&] { return t.mon->registered(); }));
    ASSERT_TRUE(wait_for([&] {
        auto s = t.core->orchestrator().spoke("monitord");
        return s.ok() && !(*s)->build_version.empty();
    }));
    auto s = t.core->orchestrator().spoke("monitord");
    ASSERT_TRUE(s.ok());
    EXPECT_EQ((*s)->build_version, "1.0.0");
    EXPECT_EQ((*s)->proto_version, transport::kProtoSchemaVersion);
    EXPECT_FALSE((*s)->capabilities.empty()) << "the capability list was not recorded";
}

/// @brief A mismatch is reported to the orchestrator, not only refused silently.
TEST(Topology, ProtoMismatchIsReportedNotOnlyRefused)
{
    // §11: "Proto version mismatch — refuse registration, raise an alarm, do
    // not guess." The refusal happened inside the transport; the alarm lived
    // in a function nothing called, so a partially updated fleet looked fine.
    Topology t;
    using ChannelT = transport::SpokeChannel<monitord::MonitorHub, monitord::MonitorSpokeFrame, monitord::MonitorHubFrame>;
    ChannelT::Options o;
    o.target = t.target("monitord");
    o.daemon = "monitord";
    o.build_version = "9.9.9";
    o.proto_version = transport::kProtoSchemaVersion + 1;
    o.backoff_base_s = 0.05;
    o.backoff_max_s = 0.1;
    ChannelT ch(std::move(o), t.log);
    ch.start();

    // The refusal reaches istate's timeline, so it is visible northbound.
    EXPECT_TRUE(wait_for([&] {
        for (const auto& e : t.core->timeline().events())
            if (e.kind == "registration" && e.detail_json.find("refused") != std::string::npos) return true;
        return false;
    })) << "a refused registration left no trace in the event timeline";
    EXPECT_FALSE(ch.registered());
    ch.stop();
}

/// @brief A spoke reconnects and re-registers after the hub restarts.
TEST(Topology, SpokeReconnectsAfterTheHubRestarts)
{
    Topology t;
    t.start_monitord();
    ASSERT_TRUE(wait_for([&] { return t.mon->registered(); }));

    // §11 "istate crashes": spokes keep running and re-attach. Drop the server
    // and stand a new one up on the same sockets.
    //
    // Shutdown needs a deadline: a bidirectional stream handler is still
    // parked in Read(), and an unbounded Shutdown() waits for it to finish
    // of its own accord, which it never does. The deadline forces the RPC to
    // be cancelled — which is precisely the abrupt disappearance that a
    // crashing istate presents to its spokes.
    t.server->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(500));
    t.server->Wait();
    t.server.reset();
    t.services.clear();
    ASSERT_TRUE(wait_for([&] { return !t.mon->registered(); }, 8000)) << "the spoke did not notice the hub going away";

    t.server = build_hub_server(*t.core, t.run_dir, &t.services);
    ASSERT_NE(t.server, nullptr);
    EXPECT_TRUE(wait_for([&] { return t.mon->registered(); }, 15000)) << "the spoke did not re-attach after the hub returned";
}

/// @brief Three spokes run concurrently and all register.
TEST(Topology, ThreeSpokesRunTogetherAndAllRegister)
{
    Topology t;
    t.start_monitord();
    t.start_faults();
    t.start_telemetry();
    EXPECT_TRUE(wait_for([&] { return t.mon->registered() && t.flt->registered() && t.tel->registered(); })) << "not every spoke attached to the hub";
    // Each got its own configuration.
    EXPECT_TRUE(wait_for([&] { return t.mon->config_revision() > 0 && t.flt->config_revision() > 0 && t.tel->config_revision() > 0; }));
    EXPECT_NEAR(t.tel_core->config().interval, 0.5, 1e-9);
    EXPECT_NEAR(t.fault_core->config().poll_interval_s, 0.2, 1e-9);
}
