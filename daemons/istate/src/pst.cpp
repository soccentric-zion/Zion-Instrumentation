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
 * @file daemons/istate/src/pst.cpp
 * @brief istate /pst persistent store implementation (REQ §6, R4): checksummed layouts, redundant copies, budgets, and corruption fallback.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/istate/pst.h"

#include <algorithm>

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

// ---------------- BudgetManager ----------------

/// @brief Reserve bytes for a consumer; fails loudly when over budget.
/// @param consumer Consumer reserving space.
/// @param bytes Bytes to reserve.
/// @return True when allowed, or an error when over budget.
Result<bool> BudgetManager::reserve(const std::string& consumer, uint64_t bytes)
{
    auto it = budgets_.find(consumer);
    if (it == budgets_.end()) return Result<bool>::good(true); // no budget configured
    uint64_t used = usage(consumer);
    if (used + bytes > it->second.limit_bytes) return Result<bool>::fail(Errc::BudgetExceeded, "budget exceeded for " + consumer + ": " + std::to_string(used + bytes) + " > " + std::to_string(it->second.limit_bytes), "BudgetManager::reserve");
    return Result<bool>::good(true);
}

/// @brief Consumers over their 80% warning line.
/// @return Consumers over their 80% warning line.
std::vector<std::string> BudgetManager::over_warning() const
{
    std::vector<std::string> out;
    for (const auto& kv : budgets_)
    {
        uint64_t used = usage(kv.first);
        if (kv.second.limit_bytes > 0 && double(used) >= double(kv.second.limit_bytes) * kv.second.alarm_at_pct) out.push_back(kv.first);
    }
    return out;
}

/// @brief Consumers over their hard limit.
/// @return Consumers over their hard limit.
std::vector<std::string> BudgetManager::over_limit() const
{
    std::vector<std::string> out;
    for (const auto& kv : budgets_)
    {
        uint64_t used = usage(kv.first);
        if (kv.second.limit_bytes > 0 && used > kv.second.limit_bytes) out.push_back(kv.first);
    }
    return out;
}

// ---------------- PstStore ----------------

/// @brief Mount and validate /pst with checksum fallback to redundant copies.
/// @return The mount-and-validate report.
PstStore::MountReport PstStore::mount()
{
    MountReport rep;
    fs_.mkdirs(root_);
    fs_.mkdirs(path("system"));
    fs_.mkdirs(path("state"));
    fs_.mkdirs(path("state/alarms"));
    fs_.mkdirs(path("state/spool"));
    fs_.mkdirs(path("state/ota"));
    fs_.mkdirs(path("backup"));

    rep.virgin = !fs_.exists(path("system/.schema-version"));

    // Validate every protected file; fall back to the redundant copy on
    // corruption. Unrecoverable files are reported, never fatal (§11).
    auto walk = [&](const std::string& rel) {
        auto entries = fs_.list_dir(path(rel));
        if (!entries.ok()) return;
        for (const auto& name : *entries)
        {
            if (name.rfind(".checksum") != std::string::npos || name.rfind(".bak") != std::string::npos) continue;
            std::string rel_file = rel.empty() ? name : rel + "/" + name;
            auto content = AtomicFile::read_protected(fs_, path(rel_file));
            if (content.ok()) continue;
            auto bak = fs_.read(path(rel_file) + ".bak");
            if (bak.ok()) { rep.recovered_from_backup.push_back(rel_file); }
            else { rep.corrupt.push_back(rel_file); }
        }
    };
    auto sys = fs_.list_dir(path("system"));
    if (sys.ok())
    {
        for (const auto& d : *sys)
        {
            auto sub = fs_.list_dir(path("system/" + d));
            if (sub.ok()) walk("system/" + d);
        }
    }
    rep.ok = true;
    return rep;
}

/// @brief Seed system/ from the shipped default YAML set.
/// @param daemon_default_yaml Image default YAML per daemon.
Error PstStore::seed_defaults(const std::map<std::string, std::string>& daemon_default_yaml)
{
    for (const auto& kv : daemon_default_yaml)
    {
        std::string dir = path("system/" + kv.first);
        fs_.mkdirs(dir);
        Error e = write_protected("system/" + kv.first + "/config.yaml", kv.second);
        if (!e.ok()) return e;
    }
    if (!fs_.exists(path("system/.schema-version"))) return set_schema_version("1");
    return Error::none();
}

/// @brief Load a daemon's configuration document.
/// @param daemon Daemon whose config to read.
/// @return The config document, or an error.
Result<ConfigDocument> PstStore::load_config(const std::string& daemon) const
{
    auto text = read_protected("system/" + daemon + "/config.yaml");
    if (!text.ok()) return Result<ConfigDocument>::fail(text.error);
    // Origins sidecar.
    auto origins_txt = fs_.read(path("system/" + daemon + "/origins.yaml"));
    // Schema validation happens in the config service (daemon-specific schema);
    // here we parse raw.
    auto j = json_from_yaml_text(*text);
    if (!j.ok()) return Result<ConfigDocument>::fail(j.error);
    ConfigDocument doc;
    auto parsed = ConfigDocument::parse(*text, JValue::object(), "system/" + daemon + "/config.yaml");
    if (!parsed.ok()) return parsed;
    if (origins_txt.ok()) (*parsed).load_origins_yaml(*origins_txt);
    return parsed;
}

/// @brief Persist a daemon's configuration atomically (checksum + .bak).
/// @param daemon Daemon whose config to write.
/// @param doc Document to persist.
Error PstStore::save_config(const std::string& daemon, const ConfigDocument& doc)
{
    fs_.mkdirs(path("system/" + daemon));
    Error e = write_protected("system/" + daemon + "/config.yaml", doc.to_yaml());
    if (!e.ok()) return e;
    return write_protected("system/" + daemon + "/origins.yaml", doc.origins_to_yaml());
}

/// @brief Read a protected file with .bak fallback.
/// @param rel_path Protected file to read.
/// @return Verified contents (with .bak fallback), or an error.
Result<std::string> PstStore::read_protected(const std::string& rel_path) const
{
    return AtomicFile::read_protected(fs_, path(rel_path));
}

/// @brief Write a protected file atomically.
/// @param rel_path Protected file to write.
/// @param data Complete contents to write.
Error PstStore::write_protected(const std::string& rel_path, const std::string& data)
{
    size_t pos = rel_path.find_last_of('/');
    if (pos != std::string::npos) fs_.mkdirs(path(rel_path.substr(0, pos)));
    return AtomicFile::write_protected(fs_, path(rel_path), data);
}

/// @brief Current on-disk schema version.
/// @return The schema version recorded on disk.
std::string PstStore::schema_version() const
{
    auto txt = read_protected("system/.schema-version");
    if (!txt.ok()) return "0";
    std::string s = *txt;
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

/// @brief Record a new schema version.
/// @param v Schema version to record.
Error PstStore::set_schema_version(const std::string& v)
{
    return write_protected("system/.schema-version", v + "\n");
}

/// @brief Merge a release's defaults, preserving existing values by origin.
/// @param new_defaults Release default YAML per daemon.
/// @return The added paths.
std::vector<std::string> PstStore::migrate(const std::map<std::string, std::string>& new_defaults)
{
    std::vector<std::string> added;
    for (const auto& kv : new_defaults)
    {
        std::string cfg_path = "system/" + kv.first + "/config.yaml";
        ConfigDocument existing;
        if (fs_.exists(path(cfg_path)))
        {
            // load_config, not a bare parse: it also reads the origins sidecar.
            // Parsing the YAML alone drops every origin to default, so after one
            // update an operator's edits would look like shipped defaults and the
            // next HQ push would be free to overwrite them (§9).
            auto cur = load_config(kv.first);
            if (cur.ok()) existing = *cur;
        }
        auto defaults = ConfigDocument::parse(kv.second, JValue::object(), "defaults/" + kv.first);
        if (!defaults.ok()) continue;
        auto added_paths = existing.merge_release(*defaults);
        for (const auto& p : added_paths) added.push_back(kv.first + "/" + p);
        if (!added_paths.empty()) save_config(kv.first, existing);
    }
    return added;
}

/// @brief System reset: wipe system/ and state/, keep factory/ and backup/.
/// @param defaults Image defaults for reseeding system/.
Error PstStore::system_reset(const std::map<std::string, std::string>& defaults)
{
    // Wipes system/ and state/; keeps factory/ and backup/ (§6.1).
    Error e = fs_.remove(path("system"), true);
    if (!e.ok()) return e;
    e = fs_.remove(path("state"), true);
    if (!e.ok()) return e;
    fs_.mkdirs(path("system"));
    fs_.mkdirs(path("state"));
    fs_.mkdirs(path("state/alarms"));
    fs_.mkdirs(path("state/spool"));
    fs_.mkdirs(path("state/ota"));
    return seed_defaults(defaults);
}

/// @brief Factory reset: wipe everything (explicitly gated).
Error PstStore::factory_reset()
{
    // Everything; manufacturing operation only, must be explicitly gated by
    // the caller (ResetRequest.confirmation == "FACTORY-RESET").
    Error e = fs_.remove(path("system"), true);
    if (!e.ok()) return e;
    e = fs_.remove(path("state"), true);
    if (!e.ok()) return e;
    e = fs_.remove(path("factory"), true);
    if (!e.ok()) return e;
    e = fs_.remove(path("backup"), true);
    if (!e.ok()) return e;
    fs_.mkdirs(path("system"));
    fs_.mkdirs(path("state"));
    return Error::none();
}

} // namespace istate
} // namespace instr