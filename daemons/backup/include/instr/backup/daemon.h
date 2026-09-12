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
 * @file daemons/backup/include/instr/backup/daemon.h
 * @brief backup daemon facade: core + command table + spoke
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <memory>

#include "instr/backup/domain.h"
#include "instr/common/command_table.h"
#include "instr/common/config_sink.h"
#include "instr/common/periodic.h"
#include "instr/transport/transport.h"
#include "backup.grpc.pb.h"

namespace instr
{
/// @brief Backup daemon: encrypted archives, restore and reset (§7.10).
namespace backup
{
using namespace domain;
using namespace instr::transport;

/// @brief Backup domain core: engine wrapper, config, schedule and event routing.
class BackupCore
{
  public:
    /// @brief Bind the core to storage, options and a clock.
    /// @param fs Storage backend.
    /// @param opts Backup options.
    /// @param ts Clock.
    BackupCore(IFileSystem& fs, const BackupOptions& opts, TimeSource& ts) : engine_(fs, opts), ts_(ts)
    {
    }

    /// @brief Configuration delivered by istate (§7.10, §9).
    struct Config
    {
        bool enabled = true;                      ///< Master switch.
        std::vector<std::string> paths{"/pst/system"};   ///< Paths to archive.
        std::vector<std::string> excludes;         ///< Extra exclusions.
        bool schedule_enabled = false;            ///< Periodic backups on/off.
        double schedule_interval_s = 86400;       ///< Periodic cadence.
        std::string destination = "local";         ///< local | hq | both.
        std::string local_dir = "/pst/backup";     ///< Archive directory.
        uint32_t retain_archives = 5;             ///< Retention count.
    };
    /// @brief Current configuration.
    /// @return Current configuration.
    const Config& config() const
    {
        return cfg_;
    }
    /// @brief Apply an istate-delivered config document (validates values).
    /// @param doc Parsed, schema-validated document.
    /// @return ok(), or an error describing why the values were refused.
    Error apply_config(const ConfigDocument& doc)
    {
        Config c;
        c.enabled = doc.get_bool("enabled", true);
        c.paths = doc.get_list("paths");
        if (c.paths.empty()) c.paths = {"/pst/system"};
        c.excludes = doc.get_list("excludes");
        c.schedule_enabled = doc.get_bool("schedule_enabled", false);
        c.schedule_interval_s = double(doc.get_int("schedule_interval_s", 86400));
        c.destination = doc.get_string("destination", "local");
        c.local_dir = doc.get_string("local_dir", "/pst/backup");
        c.retain_archives = uint32_t(doc.get_int("retain_archives", 5));
        if (c.schedule_enabled && c.schedule_interval_s < 60) return Error::make(Errc::InvalidArgument, "schedule_interval_s must be >= 60", "backup config");
        cfg_ = c;
        set_schedule(c.schedule_enabled, c.schedule_interval_s);
        archive_dir_ = c.local_dir;
        if (on_config_) on_config_(cfg_);
        return Error::none();
    }
    /// @brief Install a callback fired when a new config is accepted.
    /// @param f Config-accepted listener.
    void set_config_listener(std::function<void(const Config&)> f)
    {
        on_config_ = std::move(f);
    }

    /// @brief Engine access.
    /// @return Engine access.
    BackupEngine& engine()
    {
        return engine_;
    }
    /// @brief Override the archive directory.
    /// @param d Directory to store archives in.
    void set_archive_dir(const std::string& d)
    {
        archive_dir_ = d;
    }
    /// @brief Current archive directory.
    /// @return The archive directory.
    const std::string& archive_dir() const
    {
        return archive_dir_;
    }
    /// @brief Roots restores may touch (istate direction).
    /// @param roots Roots restores may touch (istate direction).
    void set_restore_roots(const std::vector<std::string>& roots)
    {
        restore_roots_ = roots;
    }
    const std::vector<std::string>& restore_roots() const
    {
        return restore_roots_;
    }
    /// @brief istate-directed commit: writes staged files under istate's supervision.
    /// @param f istate-directed commit callback.
    void set_commit_fn(BackupEngine::CommitFn f)
    {
        commit_fn_ = std::move(f);
    }
    /// @brief Clock access.
    /// @return Clock access.
    TimeSource& clock()
    {
        return ts_;
    }
    /// @brief Install the event sink (backup/restore results).
    /// @param f Event sink (backup/restore results).
    void set_event_sink(std::function<void(const std::string& action, const std::string& meta, bool ok, const std::string& err)> f)
    {
        sink_ = std::move(f);
    }
    /// @brief Emit one backup/restore event to the sink.
    /// @param action Event kind.
    /// @param meta Archive or detail.
    /// @param ok Outcome.
    /// @param err Error text when failed.
    void emit_event(const std::string& action, const std::string& meta, bool ok, const std::string& err)
    {
        if (sink_) sink_(action, meta, ok, err);
    }
    /// @brief Schedule (runtime state; persisted config lives in /pst via istate).
    bool schedule_enabled() const
    {
        return schedule_enabled_;
    }
    /// @brief Configured schedule interval.
    double schedule_interval_s() const
    {
        return schedule_interval_s_;
    }
    /// @brief Set the schedule state.
    /// @param enabled True to enable periodic backups.
    /// @param interval_s Cadence in seconds.
    void set_schedule(bool enabled, double interval_s)
    {
        schedule_enabled_ = enabled;
        schedule_interval_s_ = interval_s;
    }
    /// @brief True when istate directed a commit callback.
    bool has_commit_fn() const
    {
        return bool(commit_fn_);
    }
    /// @brief Run the istate-directed commit for staged files.
    /// @param files Staged files to commit.
    /// @return ok(), or the commit error.
    Error commit(const std::vector<tar::FileEntry>& files)
    {
        return commit_fn_(files);
    }

  private:
    bool schedule_enabled_ = false;         ///< Periodic backups on/off (runtime).
    double schedule_interval_s_ = 86400;    ///< Periodic cadence (runtime).
    BackupEngine engine_;                   ///< Archive engine.
    TimeSource& ts_;                         ///< Clock (not owned).
    std::string archive_dir_ = "/pst/backup";         ///< Archive directory.
    std::vector<std::string> restore_roots_ = {"/pst/system"};   ///< Restorable roots.
    BackupEngine::CommitFn commit_fn_;      ///< istate-directed commit.
    std::function<void(const std::string&, const std::string&, bool, const std::string&)> sink_;   ///< Event sink.
    Config cfg_;                            ///< Current configuration.
    std::function<void(const Config&)> on_config_;   ///< Config-accepted listener.
};

/// @brief Build the backup ctrl command table (create, list, verify, restore).
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_backup_table(BackupCore* core);

/// @brief Spoke: transport to istate plus the schema-validated config sink.
class BackupSpoke
{
  public:
    /// @brief Wire the channel, config sink and handlers together.
    /// @param core Domain core.
    /// @param log Logger.
    /// @param table Command table.
    /// @param hub_target istate gRPC target.
    /// @param build_version Build version to advertise.
    BackupSpoke(BackupCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version);
    /// @brief Start the channel.
    void start();
    /// @brief Stop the channel.
    void stop();
    /// @brief True while registered with istate.
    bool registered() const
    {
        return channel_->registered();
    }
    /// @brief Install the schema every config delivery must satisfy (R4, §9).
    /// @param schema Schema every config delivery must satisfy.
    void set_schema(JValue schema);
    /// @brief Revision of the last config applied.
    uint64_t config_revision() const
    {
        return channel_->config_revision();
    }
    /// @brief Queue a backup event to istate.
    /// @param action Event kind.
    /// @param archive Archive name.
    /// @param meta Detail.
    /// @param ok Outcome.
    /// @param err Error text when failed.
    void send_event(const std::string& action, const std::string& archive, const std::string& meta, bool ok, const std::string& err);

  private:
    BackupCore* core_;                        ///< Domain core (not owned).
    Logger* log_;                             ///< Logger (not owned).
    const CommandTable* table_;               ///< Command table (not owned).
    std::unique_ptr<ConfigSink> config_sink_;      ///< Validates and applies config deliveries.
    std::unique_ptr<SpokeChannel<backup::BackupHub, backup::BackupSpokeFrame, backup::BackupHubFrame>> channel_;   ///< gRPC channel to istate.
};

} // namespace backup
} // namespace instr