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
 * @file daemons/ota/src/main.cpp
 * @brief ota daemon main (thin shell, REQ §7.9)
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <cstdio>

#include "instr/common/config_sink.h"
#include "instr/common/daemon_skeleton.h"
#include "instr/common/log.h"
#include "instr/common/periodic.h"
#include "instr/common/version_gen.h"
#include "instr/ctrl/ctrl.h"
#include "instr/ota/daemon.h"
#include "instr/platform/factory.h"
#include "instr/common/filesystem.h"

/// @brief Daemon entry: parse args, wire domain/transport, serve until stopped.
int main(int argc, char** argv)
{
    auto args = instr::parse_daemon_args(argc, argv, "ota");
    if (args.help)
    {
        std::printf("ota — SWUpdate-based update manager (Arches Instrumentation)\n"
                    "usage: ota [--socket PATH] [--hub PATH] [--root PATH]\n"
                    "       [--platform native|simulated] [--help]\n");
        return 0;
    }
    std::atomic<bool> stop{false};
    instr::install_signal_handlers(stop);
    instr::StderrSink sink;
    instr::SystemTimeSource ts;
    instr::Logger log(sink, ts);
    std::string root = args.root.empty() ? "/" : args.root;
    instr::NativeFileSystem fs;
    // §5: the same binary runs against the simulated platform on a build host.
    bool platform_known = true;
    auto plat_owned = instr::platform::make_platform(args.platform, root, &platform_known);
    if (!platform_known) log.warning(args.name, "unknown --platform '" + args.platform + "', using native");
    instr::platform::IPlatform& plat = *plat_owned;
    instr::ota::OtaConfig cfg;
    cfg.min_version = "0.0.0";
    cfg.confirm_timeout_s = 600;
    instr::ota::OtaCore core(fs, plat, ts, cfg);
    core.set_secret_provider([] { return std::string(); }); // istate injects via relay
    core.set_running_version_provider([] { return std::string(INSTR_VERSION); });
    core.set_clock_provider([&ts] { return ts.mono_ns(); });
    core.set_wall_provider([&ts] { return ts.wall_ns() / 1'000'000'000ll; });
    // The daemon's JSON Schema ships in the image; the configuration itself
    // arrives from istate over the channel (R4), never read from a file here.
    auto cfg_schema = instr::load_daemon_schema(fs, root, "ota");
    if (!cfg_schema.ok()) log.warning("ota", "no JSON Schema installed: " + cfg_schema.error.str() + " — deliveries will be applied unvalidated");

    auto table = instr::ota::build_ota_table(&core);
    std::string run_dir = root + "/run/instrumentation";
    std::string ctrl_path = args.socket_override.empty() ? run_dir + "/ota-ctrl.sock" : args.socket_override;
    instr::ctrl::CtrlServer ctrl(&table, ctrl_path);
    auto e = ctrl.start();
    if (!e.ok())
    {
        std::fprintf(stderr, "ota: ctrl server failed: %s\n", e.str().c_str());
        return 1;
    }
    std::string hub = args.hub_socket.empty() ? run_dir + "/ota.sock" : args.hub_socket;
    instr::ota::OtaSpoke spoke(&core, &log, &table, instr::transport::uds_target(hub), INSTR_VERSION);
    if (cfg_schema.ok()) spoke.set_schema(*cfg_schema);
    spoke.start();

    // The confirmation deadline is only real if something watches it: this is
    // what rolls an unconfirmed update back automatically (§7.9, §11).
    instr::PeriodicWorker ticker("ota-tick", core.tick_interval_s(), [&] {
        auto ev = core.engine().tick(ts.mono_ns());
        if (ev) log.warning("ota", ev->stage + ": " + ev->message);
    });
    core.set_config_listener([&](double interval_s) { ticker.set_interval_s(interval_s); });
    ticker.start();

    instr::sd_notify_ready();
    instr::run_until_stop(stop, {});
    ticker.stop();
    spoke.stop();
    ctrl.stop();
    return 0;
}
