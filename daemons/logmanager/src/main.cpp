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
 * @file daemons/logmanager/src/main.cpp
 * @brief logmanager daemon main (thin shell, REQ §7.7)
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
#include "instr/logmanager/daemon.h"
#include "instr/platform/factory.h"
#include "instr/platform/native.h"
#include "instr/common/filesystem.h"

/// @brief Daemon entry: parse args, wire domain/transport, serve until stopped.
int main(int argc, char** argv)
{
    auto args = instr::parse_daemon_args(argc, argv, "logmanager");
    if (args.help)
    {
        std::printf("logmanager — collection, rotation, tail, bundles, crash forensics\n"
                    "usage: logmanager [--socket PATH] [--hub PATH] [--root PATH]\n"
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
    instr::NativeFileSystem fs;
    std::string base = root + "/pst/state/logs";
    fs.mkdirs(base);
    instr::logmanager::LogCore core(fs, base, ts);

    // Sources are data: config lists which logs to collect and send.
    std::vector<instr::logmanager::SourceSpec> sources = {
        {"journal-system", "journal", "kernel", "INFO", 8 << 20, 5, 7ull * 24 * 3600, true},
        {"journal-istate", "journal", "instr-istate", "INFO", 8 << 20, 5, 7ull * 24 * 3600, true},
        // Application log sink (§7.7): colocated applications append here and
        // the collector picks the lines up. Shipped by default so an app on a
        // stock image is collected without a config push.
        {"app", "app", root + "/var/log/instrumentation/app.log", "INFO", 8 << 20, 5, 7ull * 24 * 3600, true},
    };
    fs.mkdirs(root + "/var/log/instrumentation");
    core.store().configure(sources);
    core.store().set_budget_bytes(128ull << 20);

    // The daemon's JSON Schema ships in the image; the configuration itself
    // arrives from istate over the channel (R4), never read from a file here.
    auto cfg_schema = instr::load_daemon_schema(fs, root, "logmanager");
    if (!cfg_schema.ok()) log.warning("logmanager", "no JSON Schema installed: " + cfg_schema.error.str() + " — deliveries will be applied unvalidated");

    auto table = instr::logmanager::build_log_table(&core);
    std::string run_dir = root + "/run/instrumentation";
    std::string ctrl_path = args.socket_override.empty() ? run_dir + "/logmanager-ctrl.sock" : args.socket_override;
    instr::ctrl::CtrlServer ctrl(&table, ctrl_path);
    auto e = ctrl.start();
    if (!e.ok())
    {
        std::fprintf(stderr, "logmanager: ctrl server failed: %s\n", e.str().c_str());
        return 1;
    }
    std::string hub = args.hub_socket.empty() ? run_dir + "/logmanager.sock" : args.hub_socket;
    instr::logmanager::LogSpoke spoke(&core, &log, &table, instr::transport::uds_target(hub), INSTR_VERSION);
    if (cfg_schema.ok()) spoke.set_schema(*cfg_schema);

    // Bulk payload socket (§7.7): bundle-fetch hands back a handle and the
    // metadata, and the bytes are streamed from here — never through the
    // control path that also carries alarms and liveness.
    const std::string bulk_path = run_dir + "/logmanager-bulk.sock";
    core.set_bulk_socket_path(bulk_path);
    instr::ctrl::BulkServer bulk(bulk_path, [&core](const std::string& handle) { return core.bundles().payload_path(handle); });
    if (auto be = bulk.start(); !be.ok()) log.error("logmanager", "bulk socket unavailable: " + be.str() + " — bundles can be listed but not fetched");

    // Crash forensics (§7.7): resolve a coredump for a daemon from the
    // platform's coredump directory, newest first. istate asks for the capture;
    // finding the core is platform knowledge and belongs here.
    core.set_coredump_resolver([&](const std::string& daemon) -> std::string {
        const std::string dir = plat.coredump_dir();
        if (dir.empty()) return {};
        auto entries = fs.list_dir(dir);
        if (!entries.ok()) return {};
        std::string best;
        int64_t best_mtime = -1;
        for (const auto& entry : *entries)
        {
            if (entry.find(daemon) == std::string::npos) continue;
            const std::string full = fs.join(dir, entry);
            auto mt = fs.file_mtime_s(full);
            int64_t m = mt.ok() ? *mt : 0;
            if (m > best_mtime)
            {
                best_mtime = m;
                best = full;
            }
        }
        return best;
    });

    spoke.start();

    // Rotation, retention and the storage budget are enforced on a timer, not
    // when someone happens to ask (§7.7): the budget exists to stop logs
    // wedging the device, which only works if it is applied continuously.
    instr::PeriodicWorker prune("logmanager-prune", core.config().prune_interval_s, [&] {
        auto st = core.store().enforce_retention(ts.mono_ns());
        if (st.files_removed) log.info("logmanager", "pruned " + std::to_string(st.files_removed) + " files, " + std::to_string(st.bytes_freed) + " bytes");
    });
    core.set_config_listener([&](const instr::logmanager::LogCore::Config& c) { prune.set_interval_s(c.enabled ? c.prune_interval_s : 0.0); });
    prune.start();

    // Collection (§7.7). Without this the store is never written to and every
    // downstream feature — tail, bundles, retention — operates on an empty
    // set. Sources are re-read each pass, so an istate delivery takes effect
    // without a restart.
    instr::logmanager::LogCollector collector(&core, &plat, &fs, &ts, &log);
    collector.start();

    instr::sd_notify_ready();
    instr::run_until_stop(stop, {});
    collector.stop();
    prune.stop();
    bulk.stop();
    spoke.stop();
    ctrl.stop();
    return 0;
}