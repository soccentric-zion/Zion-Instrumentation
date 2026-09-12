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
 * @file daemons/monitord/src/main.cpp
 * @brief monitord daemon main (REQ §7.3). Thin shell: wires platform, core, table,
 *
 * ctrl server, spoke channel; all logic lives in the domain classes.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <cstdio>

#include "instr/common/config_sink.h"
#include "instr/common/daemon_skeleton.h"
#include "instr/common/log.h"
#include "instr/ctrl/ctrl.h"
#include "instr/monitord/spoke.h"
#include "instr/monitord/table.h"
#include "instr/platform/factory.h"
#include "instr/common/filesystem.h"
#include "instr/common/version_gen.h"

/// @brief Daemon entry: parse args, wire domain/transport, serve until stopped.
int main(int argc, char** argv)
{
    auto args = instr::parse_daemon_args(argc, argv, "monitord");
    if (args.help)
    {
        std::printf("monitord — system resource monitoring daemon (Arches Instrumentation)\n"
                    "usage: monitord [--socket PATH] [--hub PATH] [--root PATH]\n"
                    "       [--platform native|simulated] [--help]\n");
        return 0;
    }
    std::atomic<bool> stop{false};
    instr::install_signal_handlers(stop);

    instr::StderrSink sink;
    instr::SystemTimeSource ts;
    instr::Logger log(sink, ts);
    log.set_level(instr::LogLevel::Info);

    std::string root = args.root.empty() ? "/" : args.root;
    // §5: the same binary runs against the simulated platform on a build host.
    bool platform_known = true;
    auto plat_owned = instr::platform::make_platform(args.platform, root, &platform_known);
    if (!platform_known) log.warning(args.name, "unknown --platform '" + args.platform + "', using native");
    instr::platform::IPlatform& plat = *plat_owned;
    instr::monitord::MonitorCore core(plat, ts);

    // Resource profile from the platform class (descriptor ships with the
    // image, selected per MACHINE by the recipe). It sets the per-class
    // sampling defaults; the delivered configuration overrides them.
    instr::NativeFileSystem fs;
    {
        auto txt = fs.read(root + "/etc/instrumentation/resource-profile.yaml");
        auto sch = fs.read(root + "/usr/share/instrumentation/schemas/resource-profile.schema.json");
        instr::JValue profile_schema = instr::JValue::object();
        if (sch.ok())
        {
            auto parsed = instr::load_schema_text(*sch);
            if (parsed.ok()) profile_schema = *parsed;
        }
        if (txt.ok())
        {
            auto profile = instr::platform::load_resource_profile(*txt, profile_schema, "resource-profile.yaml");
            if (profile.ok()) { core.configure(*profile); }
            else
            {
                // An invalid profile is loud but not fatal: compiled defaults keep
                // the device sampling rather than wedging it (R10).
                log.error("monitord", "resource profile rejected: " + profile.error.str() + " — using compiled defaults");
            }
        }
    }

    auto cfg_schema = instr::load_daemon_schema(fs, root, "monitord");
    if (!cfg_schema.ok()) log.warning("monitord", "no JSON Schema installed: " + cfg_schema.error.str() + " — deliveries will be applied unvalidated");

    auto table = instr::monitord::build_monitor_table(&core);
    std::string run_dir = root + "/run/instrumentation";
    std::string ctrl_path = args.socket_override.empty() ? run_dir + "/monitord-ctrl.sock" : args.socket_override;
    instr::ctrl::CtrlServer ctrl(&table, ctrl_path);
    auto e = ctrl.start();
    if (!e.ok())
    {
        std::fprintf(stderr, "monitord: ctrl server failed: %s\n", e.str().c_str());
        return 1;
    }

    std::string hub = args.hub_socket.empty() ? run_dir + "/monitord.sock" : args.hub_socket;
    instr::monitord::MonitorSpoke spoke(&core, &log, &table, instr::transport::uds_target(hub), INSTR_VERSION);
    if (cfg_schema.ok()) spoke.set_schema(*cfg_schema);
    spoke.start();

    instr::sd_notify_ready();
    instr::run_until_stop(stop, [&] { return spoke.registered() || true; });
    spoke.stop();
    ctrl.stop();
    return 0;
}