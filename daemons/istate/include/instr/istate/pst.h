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
 * @file daemons/istate/include/instr/istate/pst.h
 * @brief /pst persistent store (REQ §6, R4). istate is the sole writer
 *
 * Layout: factory/ (read-only after manufacture), system/ (config per
 * daemon, .checksum per folder, redundant copies), state/ (runtime,
 * not backed up), backup/ (local archives). Storage headroom reserved with
 * hard per-consumer budgets enforced and alarms raised before the limit.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <map>
#include <string>
#include <vector>

#include "instr/common/atomic_file.h"
#include "instr/common/config.h"
#include "instr/common/filesystem.h"

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

/// @brief The configuration schema version this build expects in /pst.
///
/// @param consumer Consumer reserving space.
/// @param bytes Bytes to reserve.
/// @return True when allowed, or an error when over budget.
/// Bump it in the same commit that adds keys to system/defaults, so the first
/// boot after an update migrates rather than silently running without them (§6, §7.9).
inline constexpr const char* kConfigSchemaVersion = "1";

/// @brief Per-consumer storage budgets (§6: spool, logs, OTA staging can never fill the partition and wedge the device).
/// @param consumer Consumer recording usage.
/// @param bytes Current usage in bytes.
class BudgetManager
{
  public:
    /// @brief One consumer's budget.
    struct Budget
    {
        std::string consumer;        ///< spool / logs / ota / backup.
        uint64_t limit_bytes = 0;    ///< Hard cap.
        double alarm_at_pct = 0.8;   ///< Alarm before the limit.
    };
    /// @brief Bind the manager to storage and the /pst root.
    /// @return All configured budgets.
    /// @param fs Storage backend.
    /// @param root /pst root.
    explicit BudgetManager(IFileSystem& fs, const std::string& root) : fs_(fs), root_(root)
    {
    }

    /// @brief Set (or replace) a consumer's budget.
    /// @param consumer Consumer to configure (spool/logs/ota/backup).
    /// @param limit_bytes Hard cap in bytes.
    void set_budget(const std::string& consumer, uint64_t limit_bytes)
    {
        budgets_[consumer] = Budget{consumer, limit_bytes};
    }
    /// @brief Reserve bytes for a consumer; fails loudly when over budget.
    /// @param consumer Consumer reserving space.
    /// @param bytes Bytes to reserve.
    /// @return True when allowed, or an error when over budget.
    Result<bool> reserve(const std::string& consumer, uint64_t bytes);
    /// @brief Record a consumer's current usage (registered by the components themselves).
    /// @param consumer Consumer recording usage.
    /// @param bytes Current usage in bytes.
    void record_usage(const std::string& consumer, uint64_t bytes)
    {
        usage_[consumer] = bytes;
    }
    /// @brief A consumer's recorded usage (0 when unregistered).
    /// @param consumer Consumer to query.
    /// @return Recorded usage in bytes (0 when unregistered).
    uint64_t usage(const std::string& consumer) const
    {
        auto it = usage_.find(consumer);
        return it == usage_.end() ? 0 : it->second;
    }
    /// @brief Consumers over their 80% warning line (alarm before the limit).
    /// @param fs Storage backend.
    /// @param root /pst root.
    /// @param budgets Budget enforcement (not owned).
    /// @return Consumers over their 80% warning line.
    std::vector<std::string> over_warning() const;
    /// @brief Consumers over their hard limit.
    /// @return Consumers over their hard limit.
    std::vector<std::string> over_limit() const;
    /// @brief All configured budgets.
    /// @return All configured budgets.
    const std::map<std::string, Budget>& budgets() const
    {
        return budgets_;
    }

  private:
    IFileSystem& fs_;                        ///< Storage backend.
    std::string root_;                       ///< /pst root.
    std::map<std::string, Budget> budgets_;   ///< Configured budgets.
    std::map<std::string, uint64_t> usage_;   ///< Recorded usage per consumer.
};

/// @brief The /pst store: sole reader and writer of device-persistent state (R4).
class PstStore
{
  public:
    /// @brief Bind the store to a filesystem, root and budget manager.
    /// @param fs Storage backend.
    /// @param root /pst root.
    /// @param budgets Budget enforcement (not owned).
    PstStore(IFileSystem& fs, const std::string& root, BudgetManager& budgets) : fs_(fs), root_(root), budgets_(budgets)
    {
    }

    /// @brief Mount-and-validate report.
    struct MountReport
    {
        bool ok = false;                            ///< Store usable.
        bool virgin = true;                         ///< No system/ yet.
        std::vector<std::string> recovered_from_backup;   ///< Files restored from .bak copies.
        std::vector<std::string> corrupt;            ///< Unrecoverable files.
    };
    /// @brief Mount and validate: verify checksums with fallback to redundant copies (REQ §11).
    /// @return The mount-and-validate report.
    MountReport mount();
    /// @brief Seed system/ from the shipped default YAML set (first boot / system reset).
    /// @param daemon_default_yaml Image default YAML per daemon.
    /// @return ok(), or the write error.
    Error seed_defaults(const std::map<std::string, std::string>& daemon_default_yaml);

    /// @brief Load a daemon's configuration (sole reader).
    /// @param new_defaults Release default YAML per daemon.
    /// @param daemon Daemon whose config to read.
    /// @return The added paths.
    Result<ConfigDocument> load_config(const std::string& daemon) const;
    /// @brief Persist a daemon's configuration atomically with checksum+backup.
    /// @param daemon Daemon whose config to write.
    /// @param doc Document to persist.
    /// @return ok(), or the write error.
    Error save_config(const std::string& daemon, const ConfigDocument& doc);
    /// @brief Read a checksum-protected file with fallback to its .bak copy.
    /// @param rel_path Protected file to read.
    /// @return Verified contents (with .bak fallback), or an error.
    Result<std::string> read_protected(const std::string& rel_path) const;
    /// @brief Write a checksum-protected file atomically.
    /// @param rel_path Protected file to write.
    /// @param data Complete contents to write.
    /// @return ok(), or the write error.
    Error write_protected(const std::string& rel_path, const std::string& data);

    /// @brief Test whether a path exists under the root.
    /// @param rel_path Path to test under the root.
    /// @return True when it exists.
    bool exists(const std::string& rel_path) const
    {
        return fs_.exists(fs_.join(root_, rel_path));
    }

    /// @brief Current schema version on disk (§6: .schema-version drives migration).
    /// @return The schema version recorded on disk.
    std::string schema_version() const;
    /// @brief Record a new schema version.
    /// @param v Schema version to record.
    /// @return ok(), or the write error.
    Error set_schema_version(const std::string& v);
    /// @brief Merge a new release's defaults: adds missing keys, preserves existing values by origin.
    ///
    /// @param new_defaults Release default YAML per daemon.
    /// @return Added paths.
    std::vector<std::string> migrate(const std::map<std::string, std::string>& new_defaults);

    /// @brief System reset (§6.1): wipes system/ and state/; keeps factory/ and backup/; reseeds system/ from image defaults after.
    /// @param defaults Image defaults for reseeding system/.
    /// @return ok(), or the reset error.
    Error system_reset(const std::map<std::string, std::string>& defaults);
    /// @brief Factory reset: everything; manufacturing operation only, explicitly gated.
    /// @return ok(), or the reset error.
    Error factory_reset();

    /// @brief Absolute path of a relative path under the root.
    /// @param rel Path relative to the root.
    /// @return The absolute path under the root.
    std::string path(const std::string& rel) const
    {
        return fs_.join(root_, rel);
    }

  private:
    IFileSystem& fs_;            ///< Storage backend.
    std::string root_;           ///< /pst root.
    BudgetManager& budgets_;     ///< Budget enforcement (not owned).
};

} // namespace istate
} // namespace instr