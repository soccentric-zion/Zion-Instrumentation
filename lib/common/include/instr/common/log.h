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
 * @file lib/common/include/instr/common/log.h
 * @brief structured, levelled, journal-aware, rate-limited
 *
 * logging with per-source runtime level control and secret redaction
 * (REQ §4 Logging, §10 Secret hygiene: enforced with a test, not a convention).
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <mutex>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "instr/common/timestamps.h"

namespace instr
{

/// @brief Severity levels for structured log records.
enum class LogLevel
{
    Trace = 0, ///< Very verbose flow detail; normally disabled.
    Debug = 1, ///< Diagnostic detail for development.
    Info = 2, ///< Normal operational messages (default level).
    Warning = 3, ///< Recoverable problem worth attention.
    Error = 4, ///< Operation failed but the daemon keeps running.
    Critical = 5 ///< Severe failure threatening service continuity.
};

/// @brief Canonical uppercase name of a level ("INFO", "ERROR", ...).
/// @param l Level to name.
/// @return Static string for the level; "INFO" when out of range.
const char* log_level_name(LogLevel l);

/// @brief Parse a level name (case-insensitive, e.g. "INFO" -> Info).
/// @param n Name as produced by log_level_name().
/// @return The parsed level; Info when the name is unknown.
LogLevel log_level_from_name(const std::string& n);

/// @brief One structured log event with timestamps and arbitrary key=value fields.
struct LogRecord
{
    LogLevel level = LogLevel::Info; ///< Severity of the event.
    std::string source; ///< Emitting component, e.g. "telemetry.metrics".
    std::string message; ///< Human-readable event text (may contain redacted values only).
    std::map<std::string, std::string> fields; ///< Structured key=value pairs (secret keys redacted).
    uint64_t mono_ns = 0; ///< Monotonic timestamp of the event.
    int64_t wall_ns = 0; ///< Wall timestamp of the event.
    bool wall_valid = false; ///< True when wall_ns is trustworthy.
};

/// @brief Interface for log record consumers; Logger writes accepted records here.
class ISink
{
  public:
    /// @brief Default destructor.
    /// @return Default destructor.
    virtual ~ISink() = default;
    /// @brief Consume one accepted log record.
    /// @param r Record to write; the sink must not retain a reference past the call.
    virtual void write(const LogRecord& r) = 0;
};

/// @brief Structured key=value line on stderr; journald picks it up verbatim on targets.
class StderrSink : public ISink
{
  public:
    /// @brief Consume one accepted log record.
    /// @param r Record to write; the sink must not retain a reference past the call.
    void write(const LogRecord&) override;
};

/// @brief Bounded in-memory ring; used by tests and crash forensics.
class MemorySink : public ISink
{
  public:
    /// @brief Construct a ring with the given capacity.
    /// @param capacity Maximum number of records retained; older records drop first.
    explicit MemorySink(size_t capacity = 1024) : cap_(capacity)
    {
    }
    /// @brief Consume one accepted log record.
    /// @param r Record to write; the sink must not retain a reference past the call.
    void write(const LogRecord&) override;
    /// @brief Live view of the retained records, oldest first.
    ///
    /// NOT THREAD-SAFE, and deliberately so: it hands out a reference into the
    /// ring, which cannot be held safely while another thread is writing. Use
    /// it only from single-threaded tests. Everything else wants snapshot().
    ///
    /// @return Live view of the retained records, oldest first.
    const std::deque<LogRecord>& records() const
    {
        return buf_;
    }
    /// @brief Copy of the retained records, oldest first.
    ///
    /// The thread-safe reader: takes a copy under the lock.
    ///
    /// @return Copy of the retained records, oldest first.
    std::vector<LogRecord> snapshot() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return {buf_.begin(), buf_.end()};
    }
    /// @brief Discard all retained records.
    void clear()
    {
        std::lock_guard<std::mutex> lk(mu_);
        buf_.clear();
    }

  private:
    // One logger is shared by every thread in a daemon -- gRPC handler
    // threads, the sampler thread, spoke loops -- and they all reach this sink
    // through Logger::log(). write() mutates a std::deque (push_back plus
    // pop_front to hold the ring at capacity), which is not safe to do
    // concurrently: the integration topology test crashed with a SEGV inside
    // std::_Rb_tree_insert_and_rebalance on a gRPC worker thread.
    //
    // A sink is called from wherever logging happens, so it owns its own
    // synchronisation rather than making every caller remember. Found by the
    // TSan stage (Task.md A-05).
    mutable std::mutex mu_; ///< Guards buf_.
    size_t cap_; ///< Maximum retained records.
    std::deque<LogRecord> buf_; ///< Ring buffer, oldest first.
};

/// @brief Secret hygiene guard (REQ §10, §12).
///
/// Field values whose key looks like a credential are redacted; raw credential
/// material in message bodies is masked. Enforced by the secret-leak test.
class Redactor
{
  public:
    /// @brief The credential-like key names that are always redacted.
    /// @return Static list, e.g. contains password, token, psk and friends.
    static const std::vector<std::string>& secret_key_names();
    /// @brief Decide whether a field key refers to credential material.
    /// @param key Field key to test.
    /// @return True when the key must be redacted.
    static bool is_secret_key(const std::string& key);
    /// @brief Redact a field value under a credential-like key.
    /// @param key Field key (drives redaction shape, e.g. partial reveal).
    /// @param value Raw value that must not reach a sink verbatim.
    /// @return Safe replacement text for the value.
    static std::string redact_value(const std::string& key, const std::string& value);
    /// @brief Mask raw credential material appearing inside free-form text.
    /// @param text Message body to scrub.
    /// @return Text with any recognised credential substrings masked.
    static std::string redact_text(const std::string& text);
};

/// @brief Structured, levelled, rate-limited logger with per-source controls and secret redaction.
class Logger
{
  public:
    /// @brief Construct a logger writing to @p sink and stamping with @p ts.
    /// @param sink Destination for accepted records.
    /// @param ts Time source for record timestamps.
    Logger(ISink& sink, TimeSource& ts) : sink_(&sink), ts_(&ts)
    {
    }

    /// @brief Set the global level; records below it are dropped.
    /// @param l New global level.
    void set_level(LogLevel l)
    {
        level_ = l;
    }
    /// @brief Current global level.
    LogLevel level() const
    {
        return level_;
    }
    /// @brief Override the level for one source (per-source runtime control, REQ §4).
    /// @param source Source name, e.g. "telemetry.metrics".
    /// @param l New level for that source.
    void set_source_level(const std::string& source, LogLevel l)
    {
        source_levels_[source] = l;
    }
    /// @brief Remove all per-source level overrides.
    void clear_source_levels()
    {
        source_levels_.clear();
    }

    /// @brief Rate-limit one source: max N messages/s, surplus dropped and summarised.
    /// @param source Source name to limit.
    /// @param max_per_s Allowed message rate; surplus is suppressed.
    void set_rate_limit(const std::string& source, double max_per_s);
    /// @brief Remove all per-source rate limits.
    void clear_rate_limits()
    {
        rate_limits_.clear();
    }

    /// @brief Emit a record: level-filtered, rate-limited, secret-redacted, then written to the sink.
    /// @param lvl Severity of the record.
    /// @param source Emitting component name.
    /// @param msg Event text (credential material is masked before writing).
    /// @param fields Optional structured key=value pairs (secret keys redacted).
    void log(LogLevel lvl, const std::string& source, const std::string& msg, const std::map<std::string, std::string>& fields = {});

    /// @brief Convenience wrappers delegating to log() with a fixed level.
    /// @{
    /// @brief Log at Trace level.
    /// @param s Source name.
    /// @param msg Event text.
    /// @param f Optional structured fields.
    void trace(const std::string& s, const std::string& msg, const std::map<std::string, std::string>& f = {})
    {
        log(LogLevel::Trace, s, msg, f);
    }
    /// @brief Log at Debug level.
    /// @param s Source name.
    /// @param msg Event text.
    /// @param f Optional structured fields.
    void debug(const std::string& s, const std::string& msg, const std::map<std::string, std::string>& f = {})
    {
        log(LogLevel::Debug, s, msg, f);
    }
    /// @brief Log at Info level.
    /// @param s Source name.
    /// @param msg Event text.
    /// @param f Optional structured fields.
    void info(const std::string& s, const std::string& msg, const std::map<std::string, std::string>& f = {})
    {
        log(LogLevel::Info, s, msg, f);
    }
    /// @brief Log at Warning level.
    /// @param s Source name.
    /// @param msg Event text.
    /// @param f Optional structured fields.
    void warning(const std::string& s, const std::string& msg, const std::map<std::string, std::string>& f = {})
    {
        log(LogLevel::Warning, s, msg, f);
    }
    /// @brief Log at Error level.
    /// @param s Source name.
    /// @param msg Event text.
    /// @param f Optional structured fields.
    void error(const std::string& s, const std::string& msg, const std::map<std::string, std::string>& f = {})
    {
        log(LogLevel::Error, s, msg, f);
    }
    /// @brief Log at Critical level.
    /// @param s Source name.
    /// @param msg Event text.
    /// @param f Optional structured fields.
    void critical(const std::string& s, const std::string& msg, const std::map<std::string, std::string>& f = {})
    {
        log(LogLevel::Critical, s, msg, f);
    }

    /// @brief Number of messages dropped by rate limiting (exposed for tests).
    uint64_t dropped_by_rate_limit() const
    {
        return dropped_;
    }

  private:
    ISink* sink_; ///< Destination for accepted records.
    TimeSource* ts_; ///< Clock for record timestamps.
    LogLevel level_ = LogLevel::Info; ///< Global level threshold.
    std::map<std::string, LogLevel> source_levels_; ///< Per-source level overrides.
    /// @brief Per-source token-bucket state backing set_rate_limit().
    struct RateBucket
    {
        double tokens; ///< Current token count.
        double last_s; ///< Monotonic time of the last refill, seconds.
        double rate; ///< Allowed messages per second.
    };
    std::map<std::string, RateBucket> rate_limits_; ///< Active per-source rate limits.
    std::map<std::string, uint64_t> suppressed_; ///< Suppressed-count summary state per source.
    uint64_t dropped_ = 0; ///< Lifetime count of messages dropped by rate limiting.
    /// @brief Monotonic seconds used for rate-limit bookkeeping.
    /// @return Monotonic seconds.
    double now_s();
};

} // namespace instr