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
 * @file daemons/faults/src/main.cpp
 * @brief faults daemon main (thin shell, REQ §7.5). Purely passive: reads the
 *
 * kernel ring from the start on launch (backfill) and follows it.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <cstdio>

#include "instr/common/config_sink.h"
#include "instr/common/daemon_skeleton.h"
#include "instr/common/log.h"
#include "instr/common/version_gen.h"
#include "instr/ctrl/ctrl.h"
#include "instr/faults/daemon.h"
#include "instr/platform/factory.h"
#include "instr/common/filesystem.h"

/// @brief Daemon entry: parse args, wire domain/transport, serve until stopped.
int main(int argc, char** argv)
{
    auto args = instr::parse_daemon_args(argc, argv, "faults");
    if (args.help)
    {
        std::printf("faults — passive kernel-log fault detection (Arches Instrumentation)\n"
                    "usage: faults [--socket PATH] [--hub PATH] [--root PATH]\n"
                    "       [--platform native|simulated] [--help]\n");
        return 0;
    }
    std::atomic<bool> stop{false};
    instr::install_signal_handlers(stop);
    instr::StderrSink sink;
    instr::SystemTimeSource ts;
    instr::Logger log(sink, ts);

    std::string root = args.root.empty() ? "/" : args.root;
    // §5: the same binary runs against the simulated platform on a build host.
    bool platform_known = true;
    auto plat_owned = instr::platform::make_platform(args.platform, root, &platform_known);
    if (!platform_known) log.warning(args.name, "unknown --platform '" + args.platform + "', using native");
    instr::platform::IPlatform& plat = *plat_owned;
    instr::faults::FaultsCore core;

    // Rules are data: default set ships with the image; HQ may push new
    // signatures as configuration (written by istate).
    instr::NativeFileSystem fs;
    auto rules_txt = fs.read(root + "/etc/instrumentation/fault-rules.yaml");
    if (rules_txt.ok())
    {
        instr::Error e = core.load_rules(*rules_txt);
        if (!e.ok()) std::fprintf(stderr, "faults: %s\n", e.str().c_str());
    }

    // The daemon's JSON Schema ships in the image; configuration itself comes
    // from istate over the channel (R4), never from a file read here.
    auto schema = instr::load_daemon_schema(fs, root, "faults");
    if (!schema.ok()) log.warning("faults", "no JSON Schema installed: " + schema.error.str() + " — deliveries will be applied unvalidated");

    auto table = instr::faults::build_faults_table(&core);
    std::string run_dir = root + "/run/instrumentation";
    std::string ctrl_path = args.socket_override.empty() ? run_dir + "/faults-ctrl.sock" : args.socket_override;
    instr::ctrl::CtrlServer ctrl(&table, ctrl_path);
    auto e = ctrl.start();
    if (!e.ok())
    {
        std::fprintf(stderr, "faults: ctrl server failed: %s\n", e.str().c_str());
        return 1;
    }
    std::string hub = args.hub_socket.empty() ? run_dir + "/faults.sock" : args.hub_socket;
    instr::faults::FaultsSpoke spoke(&core, &log, &table, instr::transport::uds_target(hub), INSTR_VERSION);
    if (schema.ok()) spoke.set_schema(*schema);
    spoke.start();

    // Backfill first: the kernel boot log is the most diagnostic window there
    // is, and faults starts after userspace. Then follow it continuously —
    // §7.5 is real-time monitoring, not a single read at startup.
    instr::faults::FaultFollower follower(&core, &plat, &ts, &log);
    follower.backfill();
    follower.start();

    instr::sd_notify_ready();
    instr::run_until_stop(stop, {});
    follower.stop();
    spoke.stop();
    ctrl.stop();
    return 0;
}