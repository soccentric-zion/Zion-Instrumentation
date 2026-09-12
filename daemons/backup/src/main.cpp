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
 * @file daemons/backup/src/main.cpp
 * @brief backup daemon main (thin shell, REQ §7.10). All /pst writes happen under
 *
 * istate's direction (R4): the commit function is wired by istate's relay.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <cstdio>

#include "instr/common/config_sink.h"
#include "instr/common/daemon_skeleton.h"
#include "instr/common/crypto.h"
#include "instr/common/log.h"
#include "instr/common/periodic.h"
#include "instr/common/version_gen.h"
#include "instr/ctrl/ctrl.h"
#include "instr/backup/daemon.h"
#include "instr/common/filesystem.h"

/// @brief Daemon entry: parse args, wire domain/transport, serve until stopped.
int main(int argc, char** argv)
{
    auto args = instr::parse_daemon_args(argc, argv, "backup");
    if (args.help)
    {
        std::printf("backup — snapshot /pst/system, archive, restore (Arches Instrumentation)\n"
                    "usage: backup [--socket PATH] [--hub PATH] [--root PATH]\n"
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
    instr::backup::BackupOptions opts;
    // Identity comes from istate via the enrollment state; the daemon never
    // reads factory/ itself at runtime.
    auto ident = fs.read(root + "/run/instrumentation/device-identity.json");
    if (ident.ok())
    {
        auto j = instr::json_parse(*ident);
        if (j.ok() && (*j).is_object())
        {
            if (const instr::JValue* v = (*j).find("serial")) opts.device_serial = v->as_string();
            if (const instr::JValue* v = (*j).find("hw_revision")) opts.hardware_revision = v->as_string();
        }
    }
    opts.schema_version = "1";
    opts.firmware_version = INSTR_VERSION;
    auto secret = fs.read(root + "/run/instrumentation/enrollment-secret");
    opts.identity_secret = secret.ok() ? *secret : "";
    // HQ's public key arrives with the tunnel configuration at enrollment and
    // is published by istate. Without it an archive simply is not uploaded —
    // §7.10 requires HQ uploads be encrypted to HQ's key, so "send it in the
    // clear instead" is not an available fallback.
    auto hq_key = fs.read(root + "/run/instrumentation/hq-public.key");
    if (hq_key.ok())
    {
        auto raw = instr::crypto::base64_decode(*hq_key);
        if (raw.ok() && raw->size() == 32) { opts.hq_public_key = *raw; }
        else { log.error("backup", "HQ public key is malformed; uploads to HQ are disabled"); }
    }
    instr::backup::BackupCore core(fs, opts, ts);
    core.set_archive_dir(root + "/pst/backup");
    auto cfg_schema = instr::load_daemon_schema(fs, root, "backup");
    if (!cfg_schema.ok()) log.warning("backup", "no JSON Schema installed: " + cfg_schema.error.str() + " — deliveries will be applied unvalidated");

    auto table = instr::backup::build_backup_table(&core);
    std::string run_dir = root + "/run/instrumentation";
    std::string ctrl_path = args.socket_override.empty() ? run_dir + "/backup-ctrl.sock" : args.socket_override;
    instr::ctrl::CtrlServer ctrl(&table, ctrl_path);
    auto e = ctrl.start();
    if (!e.ok())
    {
        std::fprintf(stderr, "backup: ctrl server failed: %s\n", e.str().c_str());
        return 1;
    }
    std::string hub = args.hub_socket.empty() ? run_dir + "/backup.sock" : args.hub_socket;
    instr::backup::BackupSpoke spoke(&core, &log, &table, instr::transport::uds_target(hub), INSTR_VERSION);
    if (cfg_schema.ok()) spoke.set_schema(*cfg_schema);
    spoke.start();

    // Scheduled backups (§7.10). schedule_enabled in the config only means
    // something if something acts on it; the worker idles when it is off.
    instr::PeriodicWorker scheduler("backup-schedule", core.config().schedule_enabled ? core.config().schedule_interval_s : 0.0, [&] {
        const auto& c = core.config();
        auto info = core.engine().create(c.paths, c.excludes, core.archive_dir(), ts.wall_ns() / 1'000'000'000ll);
        if (!info.ok())
        {
            log.error("backup", "scheduled backup failed: " + info.error.str());
            core.emit_event("scheduled", "{}", false, info.error.str());
            return;
        }
        log.info("backup", "scheduled archive " + info->name);
        core.emit_event("scheduled", "{\"archive\":\"" + info->name + "\"}", true, "");
    });
    core.set_config_listener([&](const instr::backup::BackupCore::Config& c) { scheduler.set_interval_s(c.enabled && c.schedule_enabled ? c.schedule_interval_s : 0.0); });
    scheduler.start();

    instr::sd_notify_ready();
    instr::run_until_stop(stop, {});
    scheduler.stop();
    spoke.stop();
    ctrl.stop();
    return 0;
}
