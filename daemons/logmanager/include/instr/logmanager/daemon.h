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
 * @file daemons/logmanager/include/instr/logmanager/daemon.h
 * @brief logmanager daemon facade: core + command table + spoke + data socket for
 *
 * bulk payload transfer (handle-based; never via istate's control path).
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <memory>
#include <mutex>

#include "instr/common/command_table.h"
#include "instr/common/config_sink.h"
#include "instr/common/periodic.h"
#include "instr/logmanager/domain.h"
#include "instr/transport/transport.h"
#include "instr/platform/platform.h"
#include "logmanager.grpc.pb.h"

namespace instr
{
/// @brief LogManager daemon: collection, retention, tailing and support bundles (§7.7).
namespace logmanager
{
using namespace domain;
using namespace instr::transport;

/// @brief Logmanager domain core: store, bundles, forensics, config and the log sink.
class LogCore
{
  public:
    /// @brief Bind the core to storage, a clock and its sub-stores.
    /// @param fs Storage backend.
    /// @param base_dir Base directory (store, bundles and crash dirs derive from it).
    /// @param ts Clock for line timestamps.
    LogCore(IFileSystem& fs, const std::string& base_dir, TimeSource& ts) : store_(fs, base_dir), bundles_(fs, base_dir + "-bundles"), forensics_(fs, base_dir + "-crash"), ts_(ts)
    {
        bundles_.set_log_base_dir(base_dir);
    }

    /// @brief Configuration delivered by istate (§7.7, §9).
    struct Config
    {
        bool enabled = true; ///< Master switch for collection.
        uint64_t budget_bytes = 128ull * 1024 * 1024; ///< Storage budget for this resource class.
        double prune_interval_s = 300.0; ///< Retention pass cadence.
        uint32_t tail_max_rate_per_s = 50; ///< Live-tail rate cap.
        uint64_t coredump_max_bytes = 32ull * 1024 * 1024; ///< Coredump capture cap.
        std::vector<SourceSpec> sources; ///< Configured log sources.
    };
    /// @brief Current configuration.
    /// @return Current configuration.
    const Config& config() const
    {
        return cfg_;
    }
    /// @brief Apply an istate-delivered config document (validated by the spoke's sink).
    /// @param doc Parsed, schema-validated document.
    /// @return ok(), or an error describing why the values were refused.
    Error apply_config(const ConfigDocument& doc);
    /// @brief Install a callback fired whenever a new config is accepted.
    /// @param f Config-accepted listener.
    void set_config_listener(std::function<void(const Config&)> f)
    {
        on_config_ = std::move(f);
    }

    /// @brief Where the bulk payload socket lives, reported to callers by bundle-fetch.
    /// @param p Bulk socket path to advertise.
    void set_bulk_socket_path(std::string p)
    {
        bulk_socket_path_ = std::move(p);
    }
    /// @brief Current bulk socket path.
    /// @return The current bulk socket path.
    const std::string& bulk_socket_path() const
    {
        return bulk_socket_path_;
    }

    /// @brief Log store access.
    /// @return Log store access.
    LogStore& store()
    {
        return store_;
    }
    /// @brief Bundle store access.
    /// @return Bundle store access.
    BundleStore& bundles()
    {
        return bundles_;
    }
    /// @brief Crash-forensics access.
    /// @return Crash-forensics access.
    CrashForensics& forensics()
    {
        return forensics_;
    }
    /// @brief Clock access.
    /// @return Clock access.
    TimeSource& clock()
    {
        return ts_;
    }

    /// @brief Inject the platform's coredump resolver (§7.7).
    ///
    /// istate notices an abnormal exit and asks for a capture without knowing
    /// where coredumps land — that is platform knowledge. The resolver is
    /// injected from main so LogCore stays free of the platform layer and the
    /// domain stays testable.
    /// @param f Platform coredump resolver (daemon name → path).
    void set_coredump_resolver(std::function<std::string(const std::string& daemon)> f)
    {
        std::lock_guard<std::mutex> lk(mu_);
        coredump_resolver_ = std::move(f);
    }
    /// @brief Resolve a daemon's coredump location via the injected resolver.
    /// @return Path, or empty when unknown.
    /// @param daemon Daemon whose coredump to locate.
    std::string resolve_coredump(const std::string& daemon)
    {
        std::lock_guard<std::mutex> lk(mu_);
        return coredump_resolver_ ? coredump_resolver_(daemon) : std::string();
    }

    /// @brief Install the live log-line sink (feeds the collector's tail stream).
    /// @param f Live log-line sink.
    void set_sink(std::function<void(const LogLine&)> f)
    {
        std::lock_guard<std::mutex> lk(mu_);
        sink_ = std::move(f);
    }
    /// @brief Emit one line to the installed sink.
    /// @param l Line to emit.
    void emit(const LogLine& l)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (sink_) sink_(l);
    }

  private:
    LogStore store_; ///< Per-source log files.
    BundleStore bundles_; ///< Support bundles.
    CrashForensics forensics_; ///< Crash captures.
    TimeSource& ts_; ///< Clock for timestamps.
    std::function<void(const LogLine&)> sink_; ///< Live line sink (guarded by mu_).
    std::function<std::string(const std::string&)> coredump_resolver_; ///< Platform resolver (guarded by mu_).
    std::mutex mu_; ///< Guards sink_ and coredump_resolver_.
    Config cfg_; ///< Current configuration.
    std::function<void(const Config&)> on_config_; ///< Config-accepted listener.
    std::string bulk_socket_path_ = "/run/instrumentation/logmanager-bulk.sock"; ///< Bulk socket advertised by bundle-fetch.
};

/// @brief Periodic collection pass over every configured source (§7.7).
///
/// Same shape as the Faults daemon's FaultFollower: a periodic poll that
/// feeds the store, kept out of the transport so it is testable against a
/// simulated platform and an in-memory filesystem with no sockets. Without
/// this the daemon rotates, prunes, tails and bundles a store that nothing
/// ever writes to.
class LogCollector
{
  public:
    /// @brief Construct a collector over a core, platform, filesystem, clock and logger.
    /// @param core Domain core.
    /// @param plat Journal/kmsg access.
    /// @param fs File-sink access.
    /// @param ts Clock.
    /// @param log Logger.
    LogCollector(LogCore* core, platform::IPlatform* plat, IFileSystem* fs, TimeSource* ts, Logger* log);

    /// @brief One collection pass over every configured source.
    ///
    /// Sources are re-read from the core's config each pass, so a delivery
    /// from istate takes effect without a restart.
    ///
    /// @return Number of lines appended.
    size_t poll();
    /// @brief Start the periodic worker.
    void start();
    /// @brief Stop the periodic worker.
    void stop();
    /// @brief Override the pass cadence.
    /// @param s Pass cadence in seconds.
    void set_interval_s(double s)
    {
        worker_.set_interval_s(s);
    }
    /// @brief Passes completed so far.
    uint64_t polls() const
    {
        return worker_.ticks();
    }
    /// @brief Lines collected since construction.
    uint64_t lines_collected() const
    {
        return lines_.load();
    }

  private:
    /// @brief Collect one pass from a journal source.
    /// @param spec Journal source to collect.
    /// @return Lines appended.
    size_t collect_journal(const SourceSpec& spec);
    /// @brief Collect one pass from a file source.
    /// @param spec File source to collect.
    /// @return Lines appended.
    size_t collect_file(const SourceSpec& spec);
    /// @brief Split raw text into lines, emit and store them.
    /// @param spec Source the lines belong to.
    /// @param raw Raw collected text.
    /// @return Lines emitted and stored.
    size_t emit(const SourceSpec& spec, const std::string& raw);

    // Per-source follow position. The journal has no cursor API, so dedup is
    // by remembering the last line handed back; a file sink tracks a byte
    // offset and restarts at 0 when the file shrinks (rotation or truncation).
    struct Cursor
    {
        std::string last_line; ///< Journal: last line seen (dedup key).
        uint64_t offset = 0; ///< File: byte offset (restarts at 0 on shrink).
    };
    std::map<std::string, Cursor> cursors_; ///< Follow positions per source.

    LogCore* core_; ///< Domain core (not owned).
    platform::IPlatform* plat_; ///< Journal/kmsg access (not owned).
    IFileSystem* fs_; ///< File-sink access (not owned).
    TimeSource* ts_; ///< Clock (not owned).
    Logger* log_; ///< Logger (not owned).
    std::atomic<uint64_t> lines_{0}; ///< Lifetime lines collected.
    PeriodicWorker worker_; ///< Polling scheduler.
};

/// @brief Build the logmanager ctrl command table (sources, tail, bundles, forensics).
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_log_table(LogCore* core);
/// @brief Public helper ranking a level name (used by table handlers and tests).
/// @param level Level name to rank.
/// @return The level index (TRACE=0 .. CRITICAL=5).
int level_index_public(const std::string& level);
/// @brief First configured source name, or a default when none are configured.
/// @param core Domain core.
/// @return The first configured source name, or a default.
std::string first_source(LogCore* core);

/// @brief Spoke: transport to istate plus the schema-validated config sink.
class LogSpoke
{
  public:
    /// @brief Construct the spoke over the core's command table and hub target.
    /// @param core Domain core.
    /// @param log Logger.
    /// @param table Command table.
    /// @param hub_target istate gRPC target.
    /// @param build_version Build version to advertise.
    LogSpoke(LogCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version);
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
    /// @brief Queue a collected log line to istate.
    /// @param l Log line to queue.
    void send_line(const LogLine& l);
    /// @brief Queue a bundle announcement to istate.
    /// @param m Bundle metadata to queue.
    void send_bundle(const BundleMeta& m);
    /// @brief Queue a crash-forensics report to istate.
    /// @param daemon Daemon that crashed.
    /// @param coredump Stored coredump path.
    /// @param tail_lines Log-tail lines folded in.
    void send_crash(const std::string& daemon, const std::string& coredump, uint32_t tail_lines);

  private:
    LogCore* core_; ///< Domain core (not owned).
    Logger* log_; ///< Logger (not owned).
    const CommandTable* table_; ///< Command table (not owned).
    std::unique_ptr<ConfigSink> config_sink_; ///< Validates and applies config deliveries.
    std::unique_ptr<SpokeChannel<logmanager::LogHub, logmanager::LogSpokeFrame, logmanager::LogHubFrame>> channel_; ///< gRPC channel to istate.
};

} // namespace logmanager
} // namespace instr