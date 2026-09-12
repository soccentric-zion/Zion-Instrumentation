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
 * @file daemons/backup/src/daemon.cpp
 * @brief backup daemon facade: core state, shared command table construction, and spoke wiring (REQ §7.10).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/backup/daemon.h"

namespace instr
{
/// @brief Backup daemon: encrypted archives, restore and reset (§7.10).
namespace backup
{

/// @brief Render archive metadata as JSON.
/// @param i Archive to render.
/// @return The archive metadata as JSON.
static JValue info_json(const ArchiveInfo& i)
{
    JValue o = JValue::object();
    o.set("name", JValue(i.name));
    o.set("size_bytes", JValue(double(i.size_bytes)));
    o.set("sha256", JValue(i.sha256));
    o.set("device_serial", JValue(i.manifest.device_serial));
    o.set("schema_version", JValue(i.manifest.schema_version));
    o.set("firmware_version", JValue(i.manifest.firmware_version));
    o.set("files", JValue(double(i.manifest.files.size())));
    return o;
}

/// @brief Build the backup ctrl command table.
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_backup_table(BackupCore* core)
{
    CommandTable t;
    t.add({"create", "snapshot the configured paths into an encrypted archive", {{"excludes", false, "comma-separated path prefixes"}}, [core](const std::string& args) {
               auto a = args_object(args);
               std::vector<std::string> excludes;
               if (a.ok())
               {
                   std::string ex = (*a).get_string("excludes");
                   size_t pos = 0;
                   while (pos < ex.size())
                   {
                       size_t c = ex.find(',', pos);
                       if (c == std::string::npos) c = ex.size();
                       if (c > pos) excludes.push_back(ex.substr(pos, c - pos));
                       pos = c + 1;
                   }
               }
               int64_t now = core->clock().wall_ns() / 1'000'000'000ll;
               auto r = core->engine().create({"/pst/system"}, excludes, core->archive_dir(), now);
               if (!r.ok())
               {
                   core->emit_event("create", "", false, r.error.str());
                   return CommandResult::from_error(r.error);
               }
               core->emit_event("create", r->name, true, "");
               return CommandResult::good(info_json(*r));
           }});
    t.add({"list", "list local archives", {}, [core](const std::string&) {
               JValue arr = JValue::array();
               for (const auto& i : core->engine().list(core->archive_dir())) arr.push(info_json(i));
               return CommandResult::good(arr.dump());
           }});
    t.add({"verify", "verify an archive manifest and integrity", {{"name", true, "archive name"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto m = core->engine().read_manifest(core->archive_dir() + "/" + (*a).get_string("name"));
               if (!m.ok()) return CommandResult::from_error(m.error);
               JValue o = JValue::object();
               o.set("verified", JValue(true));
               o.set("files", JValue(double(m->files.size())));
               o.set("created_unix_s", JValue(double(m->created_unix_s)));
               return CommandResult::good(o.dump());
           }});
    t.add({"preview", "restore preview: what will change before committing", {{"name", true, "archive name"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto p = core->engine().restore_preview(core->archive_dir() + "/" + (*a).get_string("name"), core->restore_roots());
               if (!p.ok()) return CommandResult::from_error(p.error);
               JValue o = JValue::object();
               o.set("compatibility", JValue(p->compatibility == BackupEngine::Compatibility::Ok ? "ok" : p->compatibility == BackupEngine::Compatibility::HardwareMismatch ? "hardware-mismatch" : "schema-mismatch"));
               o.set("replacement_board", JValue(p->replacement_board));
               JValue arr = JValue::array();
               for (const auto& e : p->entries)
               {
                   JValue evo = JValue::object();
                   evo.set("path", JValue(e.path));
                   evo.set("action", JValue(e.action));
                   evo.set("detail", JValue(e.detail));
                   arr.push(evo);
               }
               o.set("entries", arr);
               return CommandResult::good(o.dump());
           }});
    t.add({"restore", "transactional restore: completes fully or leaves prior state", {{"name", true, "archive name"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               std::string name = (*a).get_string("name");
               auto r = core->engine().restore_commit(core->archive_dir() + "/" + name, [core](const std::vector<tar::FileEntry>& files) {
                   if (core->has_commit_fn()) return core->commit(files);
                   // No istate wiring (unit-test mode): refuse rather than
                   // write /pst silently (R4: istate is the sole writer).
                   return Error::make(Errc::Unavailable, "restore commit requires istate direction", "restore");
               });
               if (!r.ok())
               {
                   core->emit_event("restore_failed", name, false, r.error.str());
                   return CommandResult::from_error(r.error);
               }
               core->emit_event("restore_commit", name, true, "");
               JValue o = JValue::object();
               o.set("restored_files", JValue(double(*r)));
               return CommandResult::good(o.dump());
           }});
    t.add({"schedule-get", "show the backup schedule", {}, [core](const std::string&) {
               JValue o = JValue::object();
               o.set("enabled", JValue(core->schedule_enabled()));
               o.set("interval_s", JValue(double(core->schedule_interval_s())));
               return CommandResult::good(o.dump());
           }});
    t.add({"schedule-set", "set the schedule (runtime; config via istate)", {{"enabled", true, "true|false"}, {"interval_s", true, "seconds"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               core->set_schedule((*a).get_bool("enabled"), (*a).get_number("interval_s", 0));
               return CommandResult::good(std::string("{\"set\":true}"));
           }});
    return t;
}

/// @brief Wire the channel, config sink and handlers together.
/// @param core Domain core.
/// @param log Logger.
/// @param table Command table.
/// @param hub_target istate gRPC target.
/// @param build_version Build version to advertise.
BackupSpoke::BackupSpoke(BackupCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version) : core_(core), log_(log), table_(table)
{
    using ChannelT = SpokeChannel<backup::BackupHub, backup::BackupSpokeFrame, backup::BackupHubFrame>;
    ChannelT::Options o;
    o.target = hub_target;
    o.daemon = "backup";
    o.build_version = build_version;
    o.capabilities = {"snapshot", "restore-preview", "transactional-restore", "encryption"};
    channel_ = std::make_unique<ChannelT>(std::move(o), *log);
    channel_->set_hub_handler([this](const backup::BackupHubFrame& hf) {
        if (hf.has_command())
        {
            common::Ack ack = dispatch_to_ack(table_, hf.command());
            channel_->send_ack(ack);
        }
    });
    core_->set_event_sink([this](const std::string& action, const std::string& archive, bool ok, const std::string& err) { send_event(action, archive, "{}", ok, err); });
    // Until a schema is installed, deliveries still apply — just unvalidated.
    // A packaging slip must not leave a daemon running unconfigured (R10).
    set_schema(JValue::object());
}

/// @brief Install the schema every config delivery must satisfy.
/// @param schema Schema every config delivery must satisfy.
void BackupSpoke::set_schema(JValue schema)
{
    config_sink_ = std::make_unique<ConfigSink>("backup", std::move(schema), [this](const ConfigDocument& doc) { return core_->apply_config(doc); });
    channel_->set_config_handler([this](const common::ConfigDelivery& delivery) { return config_sink_->deliver(delivery.yaml(), delivery.revision()); });
}

/// @brief Start the transport channel.
void BackupSpoke::start()
{
    channel_->start();
}
/// @brief Stop the transport channel.
void BackupSpoke::stop()
{
    channel_->stop();
}

/// @brief Queue a backup event to istate.
/// @param action Event kind.
/// @param archive Archive name.
/// @param meta Detail.
/// @param ok Outcome.
/// @param err Error text when failed.
void BackupSpoke::send_event(const std::string& action, const std::string& archive, const std::string& meta, bool ok, const std::string& err)
{
    channel_->enqueue([&](backup::BackupSpokeFrame& f) {
        auto* e = f.mutable_event();
        e->set_action(action);
        e->set_archive(archive);
        e->set_meta_json(meta);
        e->set_ok(ok);
        e->set_error(err);
    });
}

} // namespace backup
} // namespace instr