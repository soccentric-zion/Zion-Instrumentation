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
 * @file daemons/networkd/src/main.cpp
 * @brief networkd daemon main (thin shell, REQ §7.2)
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
#include "instr/networkd/daemon.h"
#include "instr/platform/factory.h"

/// @brief Daemon entry: parse args, wire domain/transport, serve until stopped.
int main(int argc, char** argv)
{
    auto args = instr::parse_daemon_args(argc, argv, "networkd");
    if (args.help)
    {
        std::printf("networkd — network/firewall/tunnel/time daemon (Arches Instrumentation)\n"
                    "usage: networkd [--socket PATH] [--hub PATH] [--root PATH]\n"
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
    instr::networkd::NetworkdCore core(plat, ts);

    instr::NativeFileSystem fs;
    auto cfg_schema = instr::load_daemon_schema(fs, root, "networkd");
    if (!cfg_schema.ok()) log.warning("networkd", "no JSON Schema installed: " + cfg_schema.error.str() + " — deliveries will be applied unvalidated");

    auto table = instr::networkd::build_networkd_table(&core);

    std::string run_dir = root + "/run/instrumentation";
    std::string ctrl_path = args.socket_override.empty() ? run_dir + "/networkd-ctrl.sock" : args.socket_override;
    instr::ctrl::CtrlServer ctrl(&table, ctrl_path);
    auto e = ctrl.start();
    if (!e.ok())
    {
        std::fprintf(stderr, "networkd: ctrl server failed: %s\n", e.str().c_str());
        return 1;
    }
    std::string hub = args.hub_socket.empty() ? run_dir + "/networkd.sock" : args.hub_socket;
    instr::networkd::NetworkdSpoke spoke(&core, &log, &table, instr::transport::uds_target(hub), INSTR_VERSION);
    if (cfg_schema.ok()) spoke.set_schema(*cfg_schema);
    core.set_event_sink([&](const std::string& kind, const std::string& detail) {
        instr::networkd::NetworkEvent ev;
        ev.set_kind(kind);
        ev.set_detail_json(detail);
        spoke.send_event(ev);
    });
    spoke.start();

    // A time-boxed SSH window only actually expires if something sweeps it
    // (§7.2, §10), and failover only re-ranks if reachability is probed.
    instr::PeriodicWorker ssh_sweep("networkd-ssh-sweep", core.config().ssh_sweep_interval_s, [&] { core.sweep_remote_access(); });
    instr::PeriodicWorker probe("networkd-probe", core.config().connectivity_probe_interval_s, [&] { core.probe_connectivity(); });
    core.set_config_listener([&](const instr::networkd::NetworkdCore::Config& c) {
        ssh_sweep.set_interval_s(c.enabled ? c.ssh_sweep_interval_s : 0.0);
        probe.set_interval_s(c.enabled ? c.connectivity_probe_interval_s : 0.0);
    });
    ssh_sweep.start();
    probe.start();

    instr::sd_notify_ready();
    instr::run_until_stop(stop, {});
    probe.stop();
    ssh_sweep.stop();
    spoke.stop();
    ctrl.stop();
    return 0;
}