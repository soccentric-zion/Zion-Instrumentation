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
 * @file daemons/alarms/src/main.cpp
 * @brief alarms daemon main (thin shell, REQ §7.6). Active table persists across
 *
 * restart in state/alarms/active.db (via istate-owned state paths).
 *
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
#include "instr/alarms/daemon.h"
#include "instr/platform/native.h"
#include "instr/common/filesystem.h"

/// @brief Daemon entry: parse args, wire domain/transport, serve until stopped.
int main(int argc, char** argv)
{
    auto args = instr::parse_daemon_args(argc, argv, "alarms");
    if (args.help)
    {
        std::printf("alarms — telecom-style alarm manager (Arches Instrumentation)\n"
                    "usage: alarms [--socket PATH] [--hub PATH] [--root PATH]\n"
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
    std::string db_path = root + "/pst/state/alarms/active.db";
    instr::alarms::AlarmsCore core(fs, db_path, ts);
    // Load persisted active table + merged catalog, then regenerate from the
    // shipped catalog preserving operator edits.
    core.manager().load();
    core.manager().load_catalog(root + "/pst/state/alarms/catalog.yaml");
    auto shipped = fs.read(root + "/usr/share/instrumentation/catalog/alarms.yaml");
    if (shipped.ok())
    {
        auto entries = instr::alarms::load_catalog_entries(*shipped);
        if (entries.ok())
        {
            auto st = core.manager().merge_catalog(*entries);
            (void)st;
            core.manager().save_catalog(root + "/pst/state/alarms/catalog.yaml");
        }
    }
    core.manager().save();

    // The daemon's JSON Schema ships in the image; the configuration itself
    // arrives from istate over the channel (R4), never read from a file here.
    auto cfg_schema = instr::load_daemon_schema(fs, root, "alarms");
    if (!cfg_schema.ok()) log.warning("alarms", "no JSON Schema installed: " + cfg_schema.error.str() + " — deliveries will be applied unvalidated");

    auto table = instr::alarms::build_alarms_table(&core);
    std::string run_dir = root + "/run/instrumentation";
    std::string ctrl_path = args.socket_override.empty() ? run_dir + "/alarms-ctrl.sock" : args.socket_override;
    instr::ctrl::CtrlServer ctrl(&table, ctrl_path);
    auto e = ctrl.start();
    if (!e.ok())
    {
        std::fprintf(stderr, "alarms: ctrl server failed: %s\n", e.str().c_str());
        return 1;
    }
    std::string hub = args.hub_socket.empty() ? run_dir + "/alarms.sock" : args.hub_socket;
    instr::alarms::AlarmsSpoke spoke(&core, &log, &table, instr::transport::uds_target(hub), INSTR_VERSION);
    if (cfg_schema.ok()) spoke.set_schema(*cfg_schema);
    spoke.start();

    // Escalation sweep + persistence, on the configured interval (§7.6). An
    // alarm left uncleared must be promoted whether or not anyone is asking.
    instr::PeriodicWorker escalation("alarms-escalation", core.config().escalation_sweep_interval_s, [&] {
        core.manager().escalation_sweep(ts.mono_ns());
        core.manager().save();
    });
    core.set_config_listener([&](const instr::alarms::AlarmsCore::Config& c) { escalation.set_interval_s(c.enabled ? c.escalation_sweep_interval_s : 0.0); });
    escalation.start();

    instr::sd_notify_ready();
    instr::run_until_stop(stop, {});
    escalation.stop();
    core.manager().save();
    spoke.stop();
    ctrl.stop();
    return 0;
}