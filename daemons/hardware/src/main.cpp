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
 * @file daemons/hardware/src/main.cpp
 * @brief hardware daemon main (thin shell, REQ §7.4)
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <cstdio>
#include <fstream>
#include <sstream>

#include "instr/common/config_sink.h"
#include "instr/common/daemon_skeleton.h"
#include "instr/common/config.h"
#include "instr/common/log.h"
#include "instr/common/periodic.h"
#include "instr/common/version_gen.h"
#include "instr/ctrl/ctrl.h"
#include "instr/hardware/daemon.h"
#include "instr/platform/factory.h"
#include "instr/common/filesystem.h"

/// @brief Daemon entry: parse args, wire domain/transport, serve until stopped.
int main(int argc, char** argv)
{
    auto args = instr::parse_daemon_args(argc, argv, "hardware");
    if (args.help)
    {
        std::printf("hardware — board device control + quick health checks (Arches Instrumentation)\n"
                    "usage: hardware [--socket PATH] [--hub PATH] [--root PATH]\n"
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
    // Board knowledge lives in devices.yaml (data, not code).
    instr::JValue schema = instr::JValue::object();
    {
        instr::NativeFileSystem fs;
        auto txt = fs.read(root + "/etc/instrumentation/devices.yaml");
        auto sch = fs.read(root + "/usr/share/instrumentation/schemas/devices.schema.json");
        if (txt.ok() && sch.ok())
        {
            auto parsed_schema = instr::load_schema_text(*sch);
            if (parsed_schema.ok()) schema = *parsed_schema;
            auto desc = instr::platform::load_device_descriptor(*txt, schema, "devices.yaml");
            if (desc.ok())
            {
                instr::hardware::HardwareCore core(plat, ts, *desc);
                auto table = instr::hardware::build_hardware_table(&core);
                std::string run_dir = root + "/run/instrumentation";
                std::string ctrl_path = args.socket_override.empty() ? run_dir + "/hardware-ctrl.sock" : args.socket_override;
                instr::ctrl::CtrlServer ctrl(&table, ctrl_path);
                auto e = ctrl.start();
                if (!e.ok())
                {
                    std::fprintf(stderr, "hardware: ctrl server failed: %s\n", e.str().c_str());
                    return 1;
                }
                std::string hub = args.hub_socket.empty() ? run_dir + "/hardware.sock" : args.hub_socket;
                instr::hardware::HardwareSpoke spoke(&core, &log, &table, instr::transport::uds_target(hub), INSTR_VERSION);
                auto cfg_schema = instr::load_daemon_schema(fs, root, "hardware");
                if (!cfg_schema.ok()) log.warning("hardware", "no JSON Schema installed: " + cfg_schema.error.str() + " — deliveries will be applied unvalidated");
                if (cfg_schema.ok()) spoke.set_schema(*cfg_schema);
                spoke.start();

                // Periodic scheduled checks alongside on-demand ones (§7.4). Without
                // this the device only ever reports the health someone asked for.
                instr::PeriodicWorker checks("hardware-checks", core.config().periodic_check_interval_s, [&] {
                    for (const auto& r : core.manager().due_checks(ts.mono_ns())) core.emit(r);
                });
                core.set_config_listener([&](const instr::hardware::HardwareCore::Config& c) {
                    core.manager().set_periodic_interval_s(c.periodic_check_interval_s);
                    checks.set_interval_s(c.enabled ? c.periodic_check_interval_s : 0.0);
                });
                checks.start();

                instr::sd_notify_ready();
                instr::run_until_stop(stop, {});
                checks.stop();
                spoke.stop();
                ctrl.stop();
                return 0;
            }
            std::fprintf(stderr, "hardware: devices.yaml rejected: %s\n", desc.error.str().c_str());
            return 1;
        }
    }
    std::fprintf(stderr, "hardware: devices.yaml not found under %s\n", root.c_str());
    return 1;
}