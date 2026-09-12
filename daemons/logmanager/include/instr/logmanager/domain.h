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
 * @file daemons/logmanager/include/instr/logmanager/domain.h
 * @brief collection, rotation, retention, compression, live tail,
 *
 * bundles with handle-based bulk transfer (payload never traverses istate's
 * control path), and crash forensics (REQ §7.7).
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "instr/common/error.h"
#include "instr/common/filesystem.h"
#include "instr/common/timestamps.h"

namespace instr
{
/// @brief LogManager daemon: collection, retention, tailing and support bundles (§7.7).
namespace logmanager
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief One configured log source (journal unit or file) with rotation/retention policy.
struct SourceSpec
{
    std::string name; ///< Logical source name.
    std::string kind; ///< journal | file.
    std::string path; ///< File path (or journal unit).
    std::string level; ///< Runtime level: TRACE..CRITICAL.
    uint64_t rotate_size_bytes = 8 * 1024 * 1024; ///< Rotate when the active file exceeds this.
    uint32_t retention_files = 5; ///< Rotated files kept per source.
    uint64_t max_age_s = 7 * 24 * 3600; ///< Age-based retention cutoff.
    bool compress = true; ///< Compress rotated files.
};

/// @brief Filter parameters for a live tail session.
struct TailFilter
{
    std::vector<std::string> sources; ///< Sources to follow (empty = all).
    std::string min_level; ///< Minimum level; "" = all.
    uint64_t since_mono_ns = 0; ///< Only lines at/after this monotonic time.
    uint32_t max_rate_per_s = 50; ///< Emission rate cap.
    uint32_t timeout_s = 30; ///< Auto-stop after this many seconds.
};

/// @brief One log line as stored or tailed.
struct LogLine
{
    uint64_t mono_ns = 0; ///< Monotonic timestamp.
    std::string source; ///< Emitting source name.
    std::string level; ///< Level name.
    std::string message; ///< Line text.
};

/// @brief Per-source log files with rotation by size, retention by count and age, optional compression.
class LogStore
{
  public:
    /// @brief Bind the store to a filesystem and base directory.
    /// @param fs Storage backend.
    /// @param base_dir Directory holding per-source log files.
    LogStore(IFileSystem& fs, std::string base_dir) : fs_(fs), base_(std::move(base_dir))
    {
    }

    /// @brief Replace the configured source set.
    /// @param sources Source specs to configure.
    void configure(const std::vector<SourceSpec>& sources)
    {
        sources_ = sources;
    }
    /// @brief Currently configured sources.
    /// @return The currently configured sources.
    const std::vector<SourceSpec>& sources() const
    {
        return sources_;
    }
    /// @brief Look up a configured source by name.
    /// @return Pointer to the spec, or NotFound when unknown.
    /// @param name Source name to look up.
    Result<const SourceSpec*> find(const std::string& name) const;

    /// @brief Append one line; rotates when the active file exceeds the size limit.
    /// @param source Source to append to.
    /// @param line Line to store.
    /// @return ok(), or the underlying I/O error.
    Error append(const std::string& source, const LogLine& line);
    /// @brief Retention prune statistics.
    struct PruneStats
    {
        size_t files_removed = 0; ///< Files deleted by this pass.
        uint64_t bytes_freed = 0; ///< Bytes reclaimed by this pass.
    };
    /// @brief Enforce retention (count + age) and the per-class storage budget.
    /// @param now_mono_ns Current monotonic time for age checks.
    /// @return Counts of what this pass removed.
    PruneStats enforce_retention(uint64_t now_mono_ns);
    /// @brief Override the storage budget for this class.
    /// @param b Storage budget in bytes.
    void set_budget_bytes(uint64_t b)
    {
        budget_bytes_ = b;
    }

    /// @brief File names belonging to a source, active first (test helper).
    /// @param source Source to inspect.
    /// @return File names for the source, active first.
    std::vector<std::string> files_for(const std::string& source) const;
    /// @brief Total bytes stored across all sources.
    /// @return Total bytes stored.
    uint64_t total_bytes() const;

    /// @brief Read the active file tail for a source (bounded).
    /// @param source Source to read.
    /// @param max_lines Maximum lines returned.
    /// @return Oldest-first lines, or an error for unknown sources.
    Result<std::vector<LogLine>> read_tail(const std::string& source, size_t max_lines) const;

    /// @brief Runtime level control per source (REQ §4/§7.7).
    /// @param source Source to adjust.
    /// @param level New level name (TRACE..CRITICAL).
    void set_runtime_level(const std::string& source, const std::string& level);
    /// @brief Effective runtime level of a source (its spec level unless overridden).
    /// @param source Source to query.
    /// @return The effective runtime level.
    std::string runtime_level(const std::string& source) const;

  private:
    /// @brief Path of the active file for a source.
    /// @param source Source to resolve.
    /// @return The active file path.
    std::string active_path(const std::string& source) const
    {
        return fs_.join(base_, source + ".log");
    }
    IFileSystem& fs_; ///< Storage backend.
    std::string base_; ///< Base directory for log files.
    std::vector<SourceSpec> sources_; ///< Configured sources.
    std::map<std::string, std::string> runtime_levels_; ///< Runtime level overrides.
    uint64_t budget_bytes_ = 128ull * 1024 * 1024; ///< Per-class storage budget.
};

/// @brief Live tail session: bounded, filtered, rate-limited, auto-stop.
class TailSession
{
  public:
    /// @brief Construct a session with a filter and start time.
    /// @param filter Line selection and rate parameters.
    /// @param started_mono_ns Monotonic start for timeout enforcement.
    TailSession(TailFilter filter, uint64_t started_mono_ns) : filter_(std::move(filter)), started_mono_ns_(started_mono_ns)
    {
    }

    /// @brief Decide whether a line should be emitted; enforces the rate limit.
    /// @param line Candidate line.
    /// @param now_mono_ns Current monotonic time.
    /// @return True when the line passes filter and rate limit.
    bool accept(const LogLine& line, uint64_t now_mono_ns);
    /// @brief True when the session timeout has elapsed.
    /// @param now_mono_ns Current monotonic time.
    /// @return True when the session timeout has elapsed.
    bool expired(uint64_t now_mono_ns) const;
    /// @brief True when the line matches the session's source/level/time filter.
    /// @param line Candidate line.
    /// @return True when it matches the filter.
    bool matches(const LogLine& line) const;
    /// @brief Lines emitted so far.
    uint64_t emitted() const
    {
        return emitted_;
    }
    /// @brief Lines suppressed by the rate limit so far.
    uint64_t dropped() const
    {
        return dropped_;
    }

  private:
    TailFilter filter_; ///< Line selection and rate parameters.
    uint64_t started_mono_ns_; ///< Monotonic session start.
    uint64_t window_start_ns_ = 0; ///< Start of the current rate-limit window.
    uint32_t window_emitted_ = 0; ///< Emissions in the current window.
    bool have_window_ = false; ///< True once a window is open.
    uint64_t emitted_ = 0; ///< Total emitted.
    uint64_t dropped_ = 0; ///< Total dropped by rate limiting.
};

/// @brief Metadata of a built support bundle (bulk transfer by handle).
struct BundleMeta
{
    std::string handle; ///< Opaque handle named by the control response.
    uint64_t size_bytes = 0; ///< Payload size.
    std::string sha256; ///< Payload integrity checksum.
    std::string window_json; ///< Window, sources and manifest summary.
};

/// @brief Support-archive store: tars log windows for retrieval on the bulk socket.
class BundleStore
{
  public:
    /// @brief Bind the store to a filesystem and bundle directory.
    /// @param fs Storage backend.
    /// @param dir Bundle directory.
    BundleStore(IFileSystem& fs, std::string dir) : fs_(fs), dir_(std::move(dir))
    {
    }
    /// @brief Where the LogStore keeps its files (needed to read payloads).
    /// @param d LogStore base directory (for reading payloads).
    void set_log_base_dir(const std::string& d)
    {
        log_base_ = d;
    }

    /// @brief Build a bundle from the log store for the given sources.
    /// @param store Log store to read from.
    /// @param sources Sources to include (empty = all).
    /// @param now_mono_ns Window end.
    /// @return Bundle metadata, or an error.
    Result<BundleMeta> create(LogStore& store, const std::vector<std::string>& sources, uint64_t now_mono_ns);
    /// @brief Metadata for a handle.
    /// @param handle Bundle handle.
    /// @return The bundle metadata, or an error.
    Result<BundleMeta> meta(const std::string& handle) const;
    /// @brief Payload fetch — served on the daemon's data socket, never via istate.
    /// @param handle Bundle handle.
    /// @return The payload bytes, or an error.
    Result<std::string> payload(const std::string& handle) const;
    /// @brief Absolute path of a bundle's payload, for the bulk socket to stream from.
    ///
    /// §7.7: the payload never travels the control path. Errors for an unknown
    /// handle, so an unknown handle cannot be turned into a read of an
    /// arbitrary file.
    /// @param handle Bundle handle.
    /// @return The payload file path, or an error for unknown handles.
    Result<std::string> payload_path(const std::string& handle) const;
    /// @brief All bundles currently stored, oldest first.
    /// @return All bundles currently stored, oldest first.
    std::vector<BundleMeta> list() const;

  private:
    /// @brief Payload file path for a handle.
    /// @param handle Bundle handle.
    /// @return The payload file path.
    std::string handle_path(const std::string& handle) const
    {
        return fs_.join(dir_, "bundle-" + handle + ".tar.gz");
    }
    /// @brief Metadata file path for a handle.
    /// @param handle Bundle handle.
    /// @return The metadata file path.
    std::string meta_path(const std::string& handle) const
    {
        return fs_.join(dir_, "bundle-" + handle + ".json");
    }
    IFileSystem& fs_; ///< Storage backend.
    std::string dir_; ///< Bundle directory.
    std::string log_base_; ///< LogStore base directory.
};

/// @brief Crash forensics: bounded coredump capture + last-N lines on abnormal daemon exit.
class CrashForensics
{
  public:
    /// @brief Bind forensics to a filesystem and storage directory.
    /// @param fs Storage backend.
    /// @param dir Forensics directory.
    CrashForensics(IFileSystem& fs, std::string dir) : fs_(fs), dir_(std::move(dir))
    {
    }
    /// @brief Record of one capture.
    struct Capture
    {
        std::string daemon; ///< Daemon that crashed.
        std::string coredump_file; ///< Stored (truncated) coredump path.
        uint32_t tail_lines = 0; ///< Log-tail lines folded in.
        uint64_t coredump_bytes = 0; ///< Stored coredump size.
        bool ok = false; ///< False when nothing could be captured.
    };
    /// @brief Capture a crash.
    ///
    /// Coredump is truncated to @p max_coredump_bytes; @p last_lines come from
    /// the daemon's log tail. Stored under the forensics dir for bundling.
    ///
    /// @param daemon Crashed daemon name.
    /// @param coredump_path Source coredump path (may be empty).
    /// @param last_lines Final log lines of the daemon.
    /// @param max_coredump_bytes Cap on the stored coredump size.
    /// @return Capture record, or an error when storage failed.
    Result<Capture> capture(const std::string& daemon, const std::string& coredump_path, const std::vector<std::string>& last_lines, uint64_t max_coredump_bytes);
    /// @brief Files stored so far (for bundling).
    /// @return Files stored so far (for bundling).
    std::vector<std::string> captured_files() const;

  private:
    IFileSystem& fs_; ///< Storage backend.
    std::string dir_; ///< Forensics directory.
};

} // namespace domain
} // namespace logmanager
} // namespace instr