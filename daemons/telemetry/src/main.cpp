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
 * @file daemons/telemetry/src/main.cpp
 * @brief telemetry daemon main (thin shell, REQ §7.8)
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <cstdio>

#include "instr/common/config_sink.h"
#include "instr/common/daemon_skeleton.h"
#include "instr/common/filesystem.h"
#include "instr/common/log.h"
#include "instr/common/periodic.h"
#include "instr/common/version_gen.h"
#include "instr/ctrl/ctrl.h"
#include "instr/telemetry/daemon.h"

/// @brief Daemon entry: parse args, wire domain/transport, serve until stopped.
int main(int argc, char** argv)
{
    auto args = instr::parse_daemon_args(argc, argv, "telemetry");
    if (args.help)
    {
        std::printf("telemetry — aggregation, batch shaping, health score\n"
                    "usage: telemetry [--socket PATH] [--hub PATH] [--root PATH]\n"
                    "       [--platform native|simulated] [--help]\n");
        return 0;
    }
    std::atomic<bool> stop{false};
    instr::install_signal_handlers(stop);
    instr::StderrSink sink;
    instr::SystemTimeSource ts;
    instr::Logger log(sink, ts);
    instr::telemetry::TelemetryCore core(ts);
    core.set_repro({INSTR_VERSION, "1", "", ""});

    std::string root = args.root.empty() ? "/" : args.root;
    instr::NativeFileSystem fs;
    auto cfg_schema = instr::load_daemon_schema(fs, root, "telemetry");
    if (!cfg_schema.ok()) log.warning("telemetry", "no JSON Schema installed: " + cfg_schema.error.str() + " — deliveries will be applied unvalidated");

    auto table = instr::telemetry::build_telemetry_table(&core);
    std::string run_dir = args.root.empty() ? "/run/instrumentation" : args.root + "/run/instrumentation";
    std::string ctrl_path = args.socket_override.empty() ? run_dir + "/telemetry-ctrl.sock" : args.socket_override;
    instr::ctrl::CtrlServer ctrl(&table, ctrl_path);
    auto e = ctrl.start();
    if (!e.ok())
    {
        std::fprintf(stderr, "telemetry: ctrl server failed: %s\n", e.str().c_str());
        return 1;
    }
    std::string hub = args.hub_socket.empty() ? run_dir + "/telemetry.sock" : args.hub_socket;
    instr::telemetry::TelemetrySpoke spoke(&core, &log, &table, instr::transport::uds_target(hub), INSTR_VERSION);
    if (cfg_schema.ok()) spoke.set_schema(*cfg_schema);
    spoke.start();

    // §7.8: "at a configured interval sends it to HQ". Without a timer the
    // batch only leaves when an operator types `telemetry_ctrl flush`.
    instr::PeriodicWorker flusher("telemetry-flush", core.config().interval, [&] {
        auto batch = core.shaper().flush(core.clock().mono_ns());
        if (batch) spoke.send_batch(*batch);
    });
    core.set_config_listener([&](const instr::telemetry::TelemetryCore::Config& c) { flusher.set_interval_s(c.enabled ? c.interval : 0.0); });
    flusher.start();

    instr::sd_notify_ready();
    instr::run_until_stop(stop, {});
    flusher.stop();
    spoke.stop();
    ctrl.stop();
    return 0;
}
