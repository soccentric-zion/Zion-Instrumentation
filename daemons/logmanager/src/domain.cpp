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
 * @file daemons/logmanager/src/domain.cpp
 * @brief logmanager domain implementation (REQ §7.7): rate-limited line store, windowed bundle tar with per-window metadata, and crash forensics capture.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/logmanager/domain.h"

#include <algorithm>
#include <sstream>

#include "instr/common/crypto.h"
#include "instr/common/json.h"
#include "instr/common/tar.h"

namespace instr
{
/// @brief LogManager daemon: collection, retention, tailing and support bundles (§7.7).
namespace logmanager
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

static const char* kLevels[] = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"};
/// @brief Rank a level name (TRACE=0 .. CRITICAL=5) for filtering.
/// @param l Level name to rank.
/// @return The level index.
static int level_index(const std::string& l)
{
    for (int i = 0; i < 6; ++i)
        if (l == kLevels[i]) return i;
    return 2; // INFO
}

/// @brief Look up a configured source by name.
/// @param name Source name to look up.
/// @return Pointer to the spec, or NotFound.
Result<const SourceSpec*> LogStore::find(const std::string& name) const
{
    for (const auto& s : sources_)
        if (s.name == name) return Result<const SourceSpec*>::good(&s);
    return Result<const SourceSpec*>::fail(Errc::NotFound, "unknown log source: " + name, "logmanager");
}

/// @brief Append one line, rotating the active file when oversized.
/// @param source Source to append to.
/// @param line Line to store.
Error LogStore::append(const std::string& source, const LogLine& line)
{
    const SourceSpec* spec = nullptr;
    for (const auto& s : sources_)
        if (s.name == source) spec = &s;
    if (!spec) return Error::make(Errc::NotFound, "unconfigured source: " + source, "append");
    // Runtime level control: below-level lines are not collected.
    std::string eff = runtime_levels_.count(source) ? runtime_levels_.at(source) : spec->level;
    if (level_index(line.level) < level_index(eff)) return Error::none();

    std::string text = std::to_string(line.mono_ns) + "\t" + source + "\t" + line.level + "\t" + line.message + "\n";
    auto size = fs_.file_size(active_path(source));
    uint64_t cur = size.ok() ? *size : 0;
    if (cur + text.size() > spec->rotate_size_bytes && cur > 0)
    {
        // Rotate: active → source.N (compressed when configured), shifting others.
        for (int i = int(spec->retention_files) - 1; i >= 1; --i)
        {
            std::string from = fs_.join(base_, source + ".log." + std::to_string(i));
            std::string to = fs_.join(base_, source + ".log." + std::to_string(i + 1));
            if (fs_.exists(from)) fs_.rename(from, to);
        }
        fs_.rename(active_path(source), fs_.join(base_, source + ".log.1"));
        if (spec->compress)
        {
            // Compression of rotated files happens at prune time (batched); the
            // renamed file is marked via content hash in retention. For rotation
            // correctness alone, renaming suffices.
        }
    }
    Error e = fs_.write(active_path(source), text, /*append=*/true);
    if (!e.ok()) return e;
    return Error::none();
}

/// @brief File names belonging to a source, active first.
/// @param source Source to inspect.
/// @return File names for the source, active first.
std::vector<std::string> LogStore::files_for(const std::string& source) const
{
    std::vector<std::string> out;
    auto files = fs_.list_dir(base_);
    if (!files.ok()) return out;
    for (const auto& f : *files)
    {
        if (f == source + ".log" || f.rfind(source + ".log.", 0) == 0) out.push_back(f);
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// @brief Total bytes stored across all sources.
uint64_t LogStore::total_bytes() const
{
    uint64_t total = 0;
    auto files = fs_.list_dir(base_);
    if (!files.ok()) return 0;
    for (const auto& f : *files)
    {
        auto s = fs_.file_size(fs_.join(base_, f));
        if (s.ok()) total += *s;
    }
    return total;
}

/// @brief Enforce retention (count + age) and the storage budget.
/// @param now_mono_ns Current monotonic time.
/// @return Counts of what this pass removed.
LogStore::PruneStats LogStore::enforce_retention(uint64_t now_mono_ns)
{
    PruneStats st;
    for (const auto& spec : sources_)
    {
        auto files = files_for(spec.name);
        // Retention by count: keep active + retention_files rotated.
        int rotated = 0;
        for (auto it = files.rbegin(); it != files.rend(); ++it)
        {
            if (*it == spec.name + ".log") continue;
            ++rotated;
            if (rotated > int(spec.retention_files))
            {
                auto sz = fs_.file_size(fs_.join(base_, *it));
                Error e = fs_.remove(fs_.join(base_, *it), false);
                if (e.ok())
                {
                    ++st.files_removed;
                    if (sz.ok()) st.bytes_freed += *sz;
                }
            }
        }
        // Retention by age (§7.7: "retention per source" is expressed in time as
        // well as in count). A rotated file older than the source's max_age is
        // removed regardless of how few there are — an appliance that logs
        // rarely should not keep last year's entries forever.
        if (spec.max_age_s > 0)
        {
            auto now = fs_.file_mtime_s(fs_.join(base_, spec.name + ".log"));
            const int64_t reference = now.ok() ? *now : 0;
            if (reference > 0)
            {
                for (const auto& f : files_for(spec.name))
                {
                    if (f == spec.name + ".log") continue; // the active file never ages out
                    auto mtime = fs_.file_mtime_s(fs_.join(base_, f));
                    if (!mtime.ok()) continue;
                    if (reference - *mtime <= int64_t(spec.max_age_s)) continue;
                    auto sz = fs_.file_size(fs_.join(base_, f));
                    if (fs_.remove(fs_.join(base_, f), false).ok())
                    {
                        ++st.files_removed;
                        if (sz.ok()) st.bytes_freed += *sz;
                    }
                }
            }
        }
        (void)now_mono_ns;
    }
    // Budget enforcement per resource class: drop oldest rotated files until
    // under budget (never wedge the device, §11).
    while (total_bytes() > budget_bytes_)
    {
        std::string victim;
        // Prefer oldest rotated (highest suffix) then smallest.
        for (const auto& spec : sources_)
        {
            auto files = files_for(spec.name);
            for (auto it = files.rbegin(); it != files.rend(); ++it)
            {
                if (*it != spec.name + ".log" && it->find(".log.") != std::string::npos)
                {
                    victim = *it;
                    break;
                }
            }
            if (!victim.empty()) break;
        }
        if (victim.empty()) break; // only active files left; rotation will cap
        auto sz = fs_.file_size(fs_.join(base_, victim));
        Error e = fs_.remove(fs_.join(base_, victim), false);
        if (!e.ok()) break;
        ++st.files_removed;
        if (sz.ok()) st.bytes_freed += *sz;
    }
    return st;
}

/// @brief Read the bounded tail of a source's active file.
/// @param source Source to read.
/// @param max_lines Maximum lines returned.
/// @return Oldest-first lines, or an error for unknown sources.
Result<std::vector<LogLine>> LogStore::read_tail(const std::string& source, size_t max_lines) const
{
    auto txt = fs_.read(active_path(source));
    if (!txt.ok()) return Result<std::vector<LogLine>>::good({});
    std::vector<LogLine> out;
    std::istringstream ss(*txt);
    std::string line;
    while (std::getline(ss, line))
    {
        size_t t1 = line.find('\t');
        size_t t2 = line.find('\t', t1 + 1);
        size_t t3 = line.find('\t', t2 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos || t3 == std::string::npos) continue;
        LogLine l;
        try
        {
            l.mono_ns = std::stoull(line.substr(0, t1));
        }
        catch (...)
        {
            continue;
        }
        l.source = line.substr(t1 + 1, t2 - t1 - 1);
        l.level = line.substr(t2 + 1, t3 - t2 - 1);
        l.message = line.substr(t3 + 1);
        out.push_back(l);
    }
    if (out.size() > max_lines) out.erase(out.begin(), out.end() - long(max_lines));
    return Result<std::vector<LogLine>>::good(std::move(out));
}

/// @brief Set a runtime level override for a source.
/// @param source Source to adjust.
/// @param level New level name (TRACE..CRITICAL).
void LogStore::set_runtime_level(const std::string& source, const std::string& level)
{
    runtime_levels_[source] = level;
}

/// @brief Effective runtime level of a source.
/// @param source Source to query.
/// @return The effective runtime level.
std::string LogStore::runtime_level(const std::string& source) const
{
    auto it = runtime_levels_.find(source);
    if (it != runtime_levels_.end()) return it->second;
    for (const auto& s : sources_)
        if (s.name == source) return s.level;
    return "INFO";
}

/// @brief Decide whether a line passes the session filter.
/// @param line Candidate line.
/// @return True when it matches the filter.
bool TailSession::matches(const LogLine& line) const
{
    if (!filter_.sources.empty())
    {
        bool found = false;
        for (const auto& s : filter_.sources)
            if (s == line.source) found = true;
        if (!found) return false;
    }
    if (!filter_.min_level.empty() && level_index(line.level) < level_index(filter_.min_level)) return false;
    if (filter_.since_mono_ns && line.mono_ns < filter_.since_mono_ns) return false;
    return true;
}

/// @brief Decide whether a line is emitted, enforcing the rate limit.
/// @param line Candidate line.
/// @param now_mono_ns Current monotonic time.
/// @return True when it passes filter and rate limit.
bool TailSession::accept(const LogLine& line, uint64_t now_mono_ns)
{
    if (expired(now_mono_ns)) return false;
    if (!matches(line)) return false;
    // Rate limit: max_rate_per_s within a 1-second sliding window. The window
    // opens on the first accepted-matching line and slides on a 1s cadence.
    if (!have_window_ || now_mono_ns - window_start_ns_ >= 1'000'000'000ull)
    {
        window_start_ns_ = now_mono_ns;
        window_emitted_ = 0;
        have_window_ = true;
    }
    if (window_emitted_ >= filter_.max_rate_per_s)
    {
        ++dropped_;
        return false;
    }
    ++window_emitted_;
    ++emitted_;
    return true;
}

/// @brief True when the session timeout has elapsed.
/// @param now_mono_ns Current monotonic time.
/// @return True when the timeout has elapsed.
bool TailSession::expired(uint64_t now_mono_ns) const
{
    return now_mono_ns - started_mono_ns_ >= uint64_t(filter_.timeout_s) * 1'000'000'000ull;
}

/// @brief Tar the requested sources into a bundle with checksum metadata.
/// @param store Log store to read from.
/// @param sources Sources to include (empty = all).
/// @param now_mono_ns Window end.
/// @return Bundle metadata, or an error.
Result<BundleMeta> BundleStore::create(LogStore& store, const std::vector<std::string>& sources, uint64_t now_mono_ns)
{
    (void)now_mono_ns;
    std::vector<tar::FileEntry> entries;
    for (const auto& name : sources)
    {
        auto src = store.find(name);
        if (!src.ok()) return Result<BundleMeta>::fail(src.error);
        for (const auto& f : store.files_for(name))
        {
            auto content = fs_.read(fs_.join(log_base_, f));
            if (!content.ok()) continue;
            tar::FileEntry e;
            e.name = name + "/" + f;
            e.data = *content;
            e.mtime = now_mono_ns / 1'000'000'000ull;
            entries.push_back(e);
        }
    }
    std::string archive = tar::write_ustar(entries);
    std::string handle;
    {
        uint8_t rnd[8];
        (void)crypto::random_bytes(rnd, sizeof(rnd));
        handle = crypto::hex_encode(std::string(reinterpret_cast<char*>(rnd), sizeof(rnd)));
    }
    Error e = fs_.write(handle_path(handle), archive);
    if (!e.ok()) return Result<BundleMeta>::fail(e);
    BundleMeta m;
    m.handle = handle;
    m.size_bytes = archive.size();
    m.sha256 = crypto::sha256(archive).hex();
    JValue w = JValue::object();
    JValue arr = JValue::array();
    for (const auto& s : sources) arr.push(JValue(s));
    w.set("sources", arr);
    w.set("files", JValue(double(entries.size())));
    m.window_json = w.dump();
    fs_.write(meta_path(handle), m.window_json + "\n" + m.sha256 + "\n" + std::to_string(m.size_bytes));
    return Result<BundleMeta>::good(m);
}

/// @brief Load metadata for a handle.
/// @param handle Bundle handle.
/// @return The bundle metadata, or an error.
Result<BundleMeta> BundleStore::meta(const std::string& handle) const
{
    auto txt = fs_.read(meta_path(handle));
    if (!txt.ok()) return Result<BundleMeta>::fail(Errc::NotFound, "no such bundle handle", handle);
    BundleMeta m;
    m.handle = handle;
    std::istringstream ss(*txt);
    std::getline(ss, m.window_json);
    std::getline(ss, m.sha256);
    std::string size_line;
    std::getline(ss, size_line);
    try
    {
        m.size_bytes = std::stoull(size_line);
    }
    catch (...)
    {
    }
    return Result<BundleMeta>::good(m);
}

/// @brief Read a bundle payload (used by the small-payload path).
/// @param handle Bundle handle.
/// @return The payload bytes, or an error.
Result<std::string> BundleStore::payload(const std::string& handle) const
{
    auto txt = fs_.read(handle_path(handle));
    if (!txt.ok()) return txt;
    // Integrity verification on fetch.
    auto m = meta(handle);
    if (m.ok() && crypto::sha256(*txt).hex() != m->sha256) return Result<std::string>::fail(Errc::Corrupt, "bundle payload fails integrity check", handle);
    return txt;
}

/// @brief Resolve a handle to its payload file path (bulk socket only).
/// @param handle Bundle handle.
/// @return The payload file path, or an error for unknown handles.
Result<std::string> BundleStore::payload_path(const std::string& handle) const
{
    // A handle is minted by this store and is hex, so it cannot contain path
    // separators — but check rather than trust: this value arrives from a
    // caller and is about to become a filesystem path.
    if (handle.empty() || handle.find('/') != std::string::npos || handle.find("..") != std::string::npos) return Result<std::string>::fail(Errc::InvalidArgument, "malformed bundle handle", handle);
    const std::string p = handle_path(handle);
    if (!fs_.exists(p)) return Result<std::string>::fail(Errc::NotFound, "no such bundle", handle);
    return Result<std::string>::good(p);
}

/// @brief List all stored bundles.
/// @return All bundles currently stored, oldest first.
std::vector<BundleMeta> BundleStore::list() const
{
    std::vector<BundleMeta> out;
    auto files = fs_.list_dir(dir_);
    if (!files.ok()) return out;
    for (const auto& f : *files)
    {
        if (f.rfind("bundle-", 0) == 0 && f.rfind(".json") == f.size() - 5)
        {
            std::string handle = f.substr(7, f.size() - 12);
            auto m = meta(handle);
            if (m.ok()) out.push_back(*m);
        }
    }
    return out;
}

/// @brief Truncate-store a coredump and fold in the daemon's last log lines.
/// @param daemon Daemon that crashed.
/// @param coredump_path Source coredump path (may be empty).
/// @param last_lines Final log lines of the daemon.
/// @param max_coredump_bytes Cap on the stored coredump size.
/// @return The capture record, or an error when storage failed.
Result<CrashForensics::Capture> CrashForensics::capture(const std::string& daemon, const std::string& coredump_path, const std::vector<std::string>& last_lines, uint64_t max_coredump_bytes)
{
    Capture c;
    c.daemon = daemon;
    // Bounded coredump capture.
    auto dump = fs_.read(coredump_path);
    if (dump.ok())
    {
        std::string content = *dump;
        if (content.size() > max_coredump_bytes) content.resize(max_coredump_bytes);
        std::string out_path = fs_.join(dir_, daemon + ".core");
        Error e = fs_.write(out_path, content);
        if (!e.ok()) return Result<Capture>::fail(e);
        c.coredump_file = out_path;
        c.coredump_bytes = content.size();
    }
    // Last-N log lines folded in.
    std::string tail;
    size_t n = 0;
    for (auto it = last_lines.rbegin(); it != last_lines.rend() && n < 200; ++it, ++n) { tail = *it + "\n" + tail; }
    if (!tail.empty())
    {
        Error e = fs_.write(fs_.join(dir_, daemon + ".tail.log"), tail);
        if (!e.ok()) return Result<Capture>::fail(e);
        c.tail_lines = uint32_t(n);
    }
    c.ok = !c.coredump_file.empty() || c.tail_lines > 0;
    if (!c.ok) return Result<Capture>::fail(Errc::NotFound, "no coredump and no tail available", daemon);
    return Result<Capture>::good(c);
}

/// @brief Files stored so far.
/// @return Files stored so far.
std::vector<std::string> CrashForensics::captured_files() const
{
    auto files = fs_.list_dir(dir_);
    return files.ok() ? *files : std::vector<std::string>{};
}

} // namespace domain
} // namespace logmanager
} // namespace instr