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
 * @file test/integration/application.cpp
 * @brief colocated-application integration (REQ R5, §7.6, §7.8, §6.2)
 *
 * REQ is emphatic that an application — not a daemon — is what declares an
 * alarm (R5, §7.6), that Telemetry owes applications a metric registration
 * API (§7.8), and that after provisioning applications "may then override
 * values as they see fit, with origin recorded" (§6.2).
 *
 * None of that was reachable by an application before ApplicationApi existed:
 * alarm raise/clear and metric registration lived only on the per-daemon ctrl
 * sockets, which §3.2 fixes at 0600 root-only, and ConfigurationApi carries
 * no alarm or metric RPC. R5 was unsatisfiable by anything running on the box.
 *
 * These tests drive the real gRPC surface over a real Unix socket against a
 * live hub with the alarms and telemetry spokes attached — the topology an
 * application actually meets — so "an app can do what REQ says only an app
 * may do" is proven end to end rather than asserted.
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

#include <grpcpp/grpcpp.h>

#include "instr/alarms/daemon.h"
#include "instr/common/filesystem.h"
#include "instr/common/log.h"
#include "instr/istate/core.h"
#include "instr/istate/hubs.h"
#include "instr/platform/simulated.h"
#include "instr/telemetry/daemon.h"
#include "instr/transport/transport.h"

#include "istate.grpc.pb.h"

using namespace instr;
using namespace instr::istate;
using instr::platform::SimulatedPlatform;

namespace
{

// The alarm the tests declare. 0x1_04_1_0007 decodes as hardware / Hardware
// daemon / equipment / alarm 7 (§8) without consulting the catalog.
constexpr uint32_t kAlarmId = 0x10410007u;

static const char* kCatalog = R"yaml(
entries:
  - id: 0x1_04_1_0007
    name: hw_device_fail
    severity: CRITICAL
    probable_cause: equipment-failure
    specific_problem: quick-check-failed
    description: A board device failed its quick health check.
    escalation: promote-after-1h
    auto_clear: manual
    hq_reportable: true
)yaml";

struct AppTopology
{
    SimulatedFileSystem fs;
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

    std::unique_ptr<alarms::AlarmsCore> alarm_core;
    std::unique_ptr<alarms::AlarmsSpoke> alarm_spoke;
    CommandTable alarm_table;

    std::unique_ptr<telemetry::TelemetryCore> tel_core;
    std::unique_ptr<telemetry::TelemetrySpoke> tel_spoke;
    CommandTable tel_table;

    AppTopology()
    {
        // A Unix socket address is limited to ~108 bytes; keep the path short.
        char tpl[] = "/tmp/instr-app-XXXXXX";
        dir = ::mkdtemp(tpl);
        run_dir = dir + "/run";
        ::mkdir(run_dir.c_str(), 0755);

        opts.pst_root = "/pst";
        opts.run_dir = run_dir;
        opts.build_version = "application-test";
        opts.liveness_hang_timeout_s = 60;
        fs.mkdirs("/pst");
        plat.set_hq_reachable(true);

        core = std::make_unique<IstateCore>(opts, fs, plat, ts, log);
        core->set_default_provider([this] { return defaults(); });
        server = build_hub_server(*core, run_dir, &services);
        core->boot(defaults());
    }

    ~AppTopology()
    {
        if (alarm_spoke) alarm_spoke->stop();
        if (tel_spoke) tel_spoke->stop();
        if (server)
        {
            server->Shutdown();
            server->Wait();
        }
    }

    std::map<std::string, std::string> defaults() const
    {
        std::map<std::string, std::string> d;
        for (const char* x : {"istate", "networkd", "monitord", "hardware", "faults", "alarms", "logmanager", "telemetry", "ota", "backup"}) d[x] = "enabled: true\n";
        return d;
    }

    std::string target(const std::string& daemon) const
    {
        return transport::uds_target(run_dir + "/" + daemon + ".sock");
    }

    void start_alarms()
    {
        alarm_core = std::make_unique<alarms::AlarmsCore>(fs, "/pst/state/alarms/active.db", ts);
        auto entries = alarms::domain::load_catalog_entries(kCatalog);
        if (entries.ok()) alarm_core->manager().merge_catalog(*entries);
        alarm_table = alarms::build_alarms_table(alarm_core.get());
        alarm_spoke = std::make_unique<alarms::AlarmsSpoke>(alarm_core.get(), &log, &alarm_table, target("alarms"), "1.0.0");
        alarm_spoke->start();
    }

    void start_telemetry()
    {
        tel_core = std::make_unique<telemetry::TelemetryCore>(ts);
        tel_table = telemetry::build_telemetry_table(tel_core.get());
        tel_spoke = std::make_unique<telemetry::TelemetrySpoke>(tel_core.get(), &log, &tel_table, target("telemetry"), "1.0.0");
        tel_spoke->start();
    }

    // An application's own stub — the only thing a colocated app is given.
    std::unique_ptr<ApplicationApi::Stub> app_stub() const
    {
        auto channel = grpc::CreateChannel(transport::uds_target(run_dir + "/application.sock"), grpc::InsecureChannelCredentials());
        return ApplicationApi::NewStub(channel);
    }

    // Spokes attach asynchronously; wait rather than race.
    template <typename Fn> static bool wait_for(Fn f, int timeout_ms = 3000)
    {
        for (int i = 0; i < timeout_ms / 10; ++i)
        {
            if (f()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return f();
    }
};

} // namespace

/// @brief The application API socket is separate from every _ctrl socket (§7.6).
TEST(Application, HasItsOwnSocketDistinctFromTheCtrlSockets)
{
    // §3.2 fixes the ctrl sockets at 0600 root-only, so they can never be the
    // application surface. The application face is its own socket.
    AppTopology t;
    ASSERT_NE(t.server, nullptr) << "the hub gRPC server did not start";
    struct stat st;
    ASSERT_EQ(::stat((t.run_dir + "/application.sock").c_str(), &st), 0) << "no application socket: colocated applications have no way in";
    EXPECT_TRUE(S_ISSOCK(st.st_mode));
}

/// @brief Applications read system info without ever opening /pst (R4).
TEST(Application, CanReadSystemInfoWithoutTouchingPst)
{
    // §3.3's shape: every read is an istate call, the app holds no device state.
    AppTopology t;
    auto stub = t.app_stub();
    grpc::ClientContext ctx;
    SystemInfoRequest req;
    SystemInfo reply;
    ASSERT_TRUE(stub->GetSystemInfo(&ctx, req, &reply).ok());
    // suite_version is the one version for the whole suite, from VERSION (§14).
    EXPECT_FALSE(reply.suite_version().empty());
    EXPECT_FALSE(reply.boot_id().empty()) << "§4: every record carries a boot id";
}

/// @brief An application-raised alarm reaches the Alarms daemon (R5: applications declare alarms).
TEST(Application, RaisesAnAlarmThatReachesTheAlarmsDaemon)
{
    // R5: "Only an application sets or clears an alarm." This is that path.
    AppTopology t;
    t.start_alarms();
    ASSERT_TRUE(AppTopology::wait_for([&] { return t.alarm_spoke->registered(); })) << "alarms spoke never registered";

    auto stub = t.app_stub();
    grpc::ClientContext ctx;
    RaiseAlarmRequest req;
    req.set_alarm_id(kAlarmId);
    req.set_owner("my-app");
    req.set_additional_text("sensor out of range");
    AlarmAck reply;
    auto status = stub->RaiseAlarm(&ctx, req, &reply);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(reply.accepted());

    // The alarm is in the daemon's active table, owned by the declaring app.
    ASSERT_TRUE(AppTopology::wait_for([&] { return !t.alarm_core->manager().active().empty(); }));
    const auto& active = t.alarm_core->manager().active();
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].alarm_id, kAlarmId);
    EXPECT_EQ(active[0].owner, "my-app") << "the owner must be the declaring application, not istate";
}

/// @brief A CRITICAL application alarm requests the bounded diagnostic snapshot.
TEST(Application, CriticalAlarmAsksForTheDiagnosticSnapshot)
{
    // §7.6 snapshot-on-fault: the evidence must exist at the moment it matters,
    // and the caller is told a capture was triggered.
    AppTopology t;
    t.start_alarms();
    ASSERT_TRUE(AppTopology::wait_for([&] { return t.alarm_spoke->registered(); }));
    auto stub = t.app_stub();
    grpc::ClientContext ctx;
    RaiseAlarmRequest req;
    req.set_alarm_id(kAlarmId); // CRITICAL in the catalog
    req.set_owner("my-app");
    AlarmAck reply;
    ASSERT_TRUE(stub->RaiseAlarm(&ctx, req, &reply).ok());
    EXPECT_TRUE(reply.needs_snapshot());
}

/// @brief An application can clear the alarm it raised.
TEST(Application, ClearsAnAlarmItRaised)
{
    AppTopology t;
    t.start_alarms();
    ASSERT_TRUE(AppTopology::wait_for([&] { return t.alarm_spoke->registered(); }));
    auto stub = t.app_stub();
    {
        grpc::ClientContext ctx;
        RaiseAlarmRequest req;
        req.set_alarm_id(kAlarmId);
        req.set_owner("my-app");
        AlarmAck reply;
        ASSERT_TRUE(stub->RaiseAlarm(&ctx, req, &reply).ok());
    }
    ASSERT_TRUE(AppTopology::wait_for([&] { return !t.alarm_core->manager().active().empty(); }));
    {
        grpc::ClientContext ctx;
        ClearAlarmRequest req;
        req.set_alarm_id(kAlarmId);
        req.set_owner("my-app");
        AlarmAck reply;
        auto status = stub->ClearAlarm(&ctx, req, &reply);
        ASSERT_TRUE(status.ok()) << status.error_message();
        EXPECT_TRUE(reply.accepted());
    }
    EXPECT_TRUE(AppTopology::wait_for([&] { return t.alarm_core->manager().active().empty(); })) << "the alarm the application cleared is still active";
}

/// @brief An alarm without an owner is refused — attribution is mandatory.
TEST(Application, AnAlarmWithoutAnOwnerIsRefused)
{
    // R5 makes the owner the whole point: an unattributed alarm is not a
    // declaration, and guessing one would put istate's name on an app's fault.
    AppTopology t;
    t.start_alarms();
    auto stub = t.app_stub();
    grpc::ClientContext ctx;
    RaiseAlarmRequest req;
    req.set_alarm_id(kAlarmId); // no owner
    AlarmAck reply;
    auto status = stub->RaiseAlarm(&ctx, req, &reply);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

/// @brief When Alarms is unreachable the failure is reported, never swallowed.
TEST(Application, AlarmWithTheAlarmsDaemonDownIsReportedNotSwallowed)
{
    // R10: the device keeps working. But an application that declared an alarm
    // must learn it did not land, rather than believe it did.
    AppTopology t; // alarms spoke deliberately not started
    auto stub = t.app_stub();
    grpc::ClientContext ctx;
    RaiseAlarmRequest req;
    req.set_alarm_id(kAlarmId);
    req.set_owner("my-app");
    AlarmAck reply;
    auto status = stub->RaiseAlarm(&ctx, req, &reply);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE);
}

/// @brief An application registers a metric with Telemetry (§7.8 registration API).
TEST(Application, RegistersAMetricWithTelemetry)
{
    // §7.8: "application metric registration API".
    AppTopology t;
    t.start_telemetry();
    ASSERT_TRUE(AppTopology::wait_for([&] { return t.tel_spoke->registered(); })) << "telemetry spoke never registered";
    auto stub = t.app_stub();
    grpc::ClientContext ctx;
    RegisterMetricRequest req;
    req.set_name("myapp.latency");
    req.set_unit("ms");
    req.set_type("gauge");
    req.set_app("my-app");
    common::Ack reply;
    auto status = stub->RegisterMetric(&ctx, req, &reply);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(reply.ok());
}

/// @brief Registered metrics accept samples and flow into batches.
TEST(Application, ReportsMetricSamples)
{
    AppTopology t;
    t.start_telemetry();
    ASSERT_TRUE(AppTopology::wait_for([&] { return t.tel_spoke->registered(); }));
    auto stub = t.app_stub();
    {
        grpc::ClientContext ctx;
        RegisterMetricRequest req;
        req.set_name("myapp.latency");
        req.set_unit("ms");
        req.set_type("gauge");
        req.set_app("my-app");
        common::Ack reply;
        ASSERT_TRUE(stub->RegisterMetric(&ctx, req, &reply).ok());
    }
    grpc::ClientContext ctx;
    ReportMetricsRequest req;
    for (double v : {11.0, 12.0, 13.0})
    {
        auto* s = req.add_samples();
        s->set_name("myapp.latency");
        s->set_value(v);
        s->set_app("my-app");
    }
    ReportMetricsReply reply;
    auto status = stub->ReportMetrics(&ctx, req, &reply);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(reply.accepted(), 3u);
    EXPECT_EQ(reply.rejected(), 0u);
}

/// @brief A malformed sample is rejected without failing the rest of the batch.
TEST(Application, ASampleWithNoMetricNameIsRejectedWithoutFailingTheBatch)
{
    // §14: no silent failures. A bad sample is counted and reported; the good
    // ones in the same batch still land.
    AppTopology t;
    t.start_telemetry();
    ASSERT_TRUE(AppTopology::wait_for([&] { return t.tel_spoke->registered(); }));
    auto stub = t.app_stub();
    grpc::ClientContext ctx;
    ReportMetricsRequest req;
    req.add_samples()->set_name(""); // bad
    auto* good = req.add_samples();
    good->set_name("myapp.other");
    good->set_value(1.0);
    ReportMetricsReply reply;
    ASSERT_TRUE(stub->ReportMetrics(&ctx, req, &reply).ok());
    EXPECT_EQ(reply.rejected(), 1u);
    EXPECT_EQ(reply.accepted(), 1u);
    EXPECT_FALSE(reply.error().empty()) << "a rejected sample must be diagnosable";
}

/// @brief Application config writes land with origin=App (§6.2).
TEST(Application, ConfigWritesLandWithAppOrigin)
{
    // §6.2: applications "may then override values as they see fit, with origin
    // recorded". The origin is what a reset and an HQ push consult later, so it
    // must be "app" and the caller must not get to choose it.
    AppTopology t;
    auto stub = t.app_stub();
    {
        grpc::ClientContext ctx;
        SetAppConfigRequest req;
        req.set_daemon("telemetry");
        req.set_key("app_interval_s");
        req.set_value("30");
        req.set_app("my-app");
        SetConfigReply reply;
        auto status = stub->SetAppConfig(&ctx, req, &reply);
        ASSERT_TRUE(status.ok()) << status.error_message();
        ASSERT_TRUE(reply.ok()) << reply.error();
    }
    grpc::ClientContext ctx;
    GetConfigRequest req;
    req.set_daemon("telemetry");
    ConfigSnapshot reply;
    ASSERT_TRUE(stub->GetConfig(&ctx, req, &reply).ok());
    auto it = reply.origins().find("app_interval_s");
    ASSERT_NE(it, reply.origins().end()) << "the key the application set is not in the snapshot";
    EXPECT_EQ(it->second, "app");
}

/// @brief Application config writes are attributed in the audit trail.
TEST(Application, ConfigWriteIsAttributedInTheAuditTrail)
{
    // §10: "who changed what, when, from where".
    AppTopology t;
    auto stub = t.app_stub();
    grpc::ClientContext ctx;
    SetAppConfigRequest req;
    req.set_daemon("telemetry");
    req.set_key("app_interval_s");
    req.set_value("45");
    req.set_app("my-app");
    SetConfigReply reply;
    ASSERT_TRUE(stub->SetAppConfig(&ctx, req, &reply).ok());

    bool found = false;
    for (const auto& e : t.core->audit().entries())
        if (e.actor == "my-app" && e.action == "config_set") found = true;
    EXPECT_TRUE(found) << "an application's config write left no audit trail";
}

/// @brief A config write without an app name is refused — attribution is mandatory.
TEST(Application, ConfigWriteWithoutAnAppNameIsRefused)
{
    AppTopology t;
    auto stub = t.app_stub();
    grpc::ClientContext ctx;
    SetAppConfigRequest req;
    req.set_daemon("telemetry");
    req.set_key("app_interval_s");
    req.set_value("30"); // no app
    SetConfigReply reply;
    auto status = stub->SetAppConfig(&ctx, req, &reply);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

/// @brief Dotted application keys expand into the nested document shape.
TEST(Application, NestedConfigKeysAreExpanded)
{
    // Config paths are dotted; an application sets one key at a time and the
    // API turns "a.b" into the nested document ConfigService expects.
    AppTopology t;
    auto stub = t.app_stub();
    {
        grpc::ClientContext ctx;
        SetAppConfigRequest req;
        req.set_daemon("telemetry");
        req.set_key("app.window_s");
        req.set_value("60");
        req.set_app("my-app");
        SetConfigReply reply;
        ASSERT_TRUE(stub->SetAppConfig(&ctx, req, &reply).ok());
        ASSERT_TRUE(reply.ok()) << reply.error();
    }
    grpc::ClientContext ctx;
    GetConfigRequest req;
    req.set_daemon("telemetry");
    ConfigSnapshot reply;
    ASSERT_TRUE(stub->GetConfig(&ctx, req, &reply).ok());
    EXPECT_NE(reply.origins().find("app.window_s"), reply.origins().end()) << "a dotted key did not become a nested config path";
}

/// @brief Origin rules: an application cannot overwrite an operator's local edit.
TEST(Application, CannotOverwriteAnOperatorsLocalEdit)
{
    // §4 merge semantics: an application adding keys must never clobber what an
    // operator set by hand. force is not the application's to pass.
    AppTopology t;
    auto set = t.core->config_service().set("telemetry", "app_interval_s: 5\n", Origin::Local,
                                            /*force=*/false);
    ASSERT_TRUE(set.ok);

    auto stub = t.app_stub();
    grpc::ClientContext ctx;
    SetAppConfigRequest req;
    req.set_daemon("telemetry");
    req.set_key("app_interval_s");
    req.set_value("999");
    req.set_app("my-app");
    SetConfigReply reply;
    ASSERT_TRUE(stub->SetAppConfig(&ctx, req, &reply).ok());

    auto doc = t.core->config_service().get("telemetry");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ(doc->get_string("app_interval_s"), "5") << "the application overwrote an operator's local edit";
}
