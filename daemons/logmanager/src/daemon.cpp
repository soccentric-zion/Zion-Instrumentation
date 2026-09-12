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
 * @file daemons/logmanager/src/daemon.cpp
 * @brief logmanager daemon facade: core state, shared command table construction, spoke wiring, and the bulk payload data socket (REQ §7.7).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/logmanager/daemon.h"

#include "instr/common/crypto.h"

namespace instr
{
/// @brief LogManager daemon: collection, retention, tailing and support bundles (§7.7).
namespace logmanager
{

/// @brief Render bundle metadata as JSON for control responses.
/// @param m Bundle metadata to render.
/// @return The metadata as JSON.
static JValue bundle_json(const BundleMeta& m)
{
    JValue o = JValue::object();
    o.set("handle", JValue(m.handle));
    o.set("size_bytes", JValue(double(m.size_bytes)));
    o.set("sha256", JValue(m.sha256));
    o.set("meta", JValue(m.window_json));
    return o;
}

/// @brief Apply an istate-delivered config document to the core.
/// @param doc Parsed, schema-validated document.
/// @return ok(), or an error describing why the values were refused.
Error LogCore::apply_config(const ConfigDocument& doc)
{
    Config c;
    c.enabled = doc.get_bool("enabled", true);
    c.budget_bytes = uint64_t(doc.get_int("budget_bytes", 128ll * 1024 * 1024));
    c.prune_interval_s = doc.get_double("prune_interval_s", 300.0);
    c.tail_max_rate_per_s = uint32_t(doc.get_int("tail_max_rate_per_s", 50));
    c.coredump_max_bytes = uint64_t(doc.get_int("coredump_max_bytes", 32ll * 1024 * 1024));
    if (c.prune_interval_s <= 0) return Error::make(Errc::InvalidArgument, "prune_interval_s must be > 0", "logmanager config");

    // sources[] is a list of objects; the flattened document exposes them as
    // "sources.<i>.<key>". Walk until an index has no name.
    for (size_t i = 0;; ++i)
    {
        const std::string base = "sources." + std::to_string(i) + ".";
        if (!doc.has(base + "name")) break;
        SourceSpec sp;
        sp.name = doc.get_string(base + "name");
        sp.kind = doc.get_string(base + "kind", "journal");
        sp.path = doc.get_string(base + "path");
        sp.level = doc.get_string(base + "level", "INFO");
        sp.rotate_size_bytes = uint64_t(doc.get_int(base + "rotate_size_bytes", 8ll * 1024 * 1024));
        sp.retention_files = uint32_t(doc.get_int(base + "retention_files", 5));
        sp.max_age_s = uint64_t(doc.get_int(base + "retention_age_s", 7ll * 24 * 3600));
        sp.compress = doc.get_bool(base + "compress", true);
        if (sp.rotate_size_bytes < 1024) return Error::make(Errc::InvalidArgument, "rotate_size_bytes too small for source " + sp.name, "logmanager config");
        c.sources.push_back(sp);
    }
    cfg_ = c;
    store_.configure(c.sources);
    store_.set_budget_bytes(c.budget_bytes);
    if (on_config_) on_config_(cfg_);
    return Error::none();
}

/// @brief Build the logmanager ctrl command table.
/// @param core Domain core the table reads from.
/// @return The command table.
CommandTable build_log_table(LogCore* core)
{
    CommandTable t;
    t.add({"sources", "list configured log sources and their runtime levels", {}, [core](const std::string&) {
               JValue arr = JValue::array();
               for (const auto& s : core->store().sources())
               {
                   JValue o = JValue::object();
                   o.set("name", JValue(s.name));
                   o.set("kind", JValue(s.kind));
                   o.set("level", JValue(core->store().runtime_level(s.name)));
                   o.set("rotate_size_bytes", JValue(double(s.rotate_size_bytes)));
                   o.set("retention_files", JValue(double(s.retention_files)));
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    t.add({"level-set", "set the runtime level for a source", {{"source", true, "source name"}, {"level", true, "TRACE|DEBUG|INFO|WARNING|ERROR|CRITICAL"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               std::string src = (*a).get_string("source");
               std::string lvl = (*a).get_string("level");
               if (!core->store().find(src).ok()) return CommandResult::err(Errc::NotFound, "unknown source: " + src);
               if (level_index_public(lvl) < 0) return CommandResult::err(Errc::InvalidArgument, "bad level: " + lvl);
               core->store().set_runtime_level(src, lvl);
               return CommandResult::good(std::string("{\"set\":true}"));
           }});
    t.add({"tail",
           "bounded on-demand live tail (stream)",
           {{"sources", false, "comma-separated source names"}, {"min_level", false, "minimum level"}, {"since_mono_ns", false, "start timestamp"}, {"max_rate_per_s", false, "rate limit"}, {"timeout_s", false, "auto-stop"}},
           [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               TailFilter f;
               std::string srcs = (*a).get_string("sources");
               size_t pos = 0;
               while (pos < srcs.size())
               {
                   size_t c = srcs.find(',', pos);
                   if (c == std::string::npos) c = srcs.size();
                   if (c > pos) f.sources.push_back(srcs.substr(pos, c - pos));
                   pos = c + 1;
               }
               f.min_level = (*a).get_string("min_level");
               f.since_mono_ns = uint64_t((*a).get_number("since_mono_ns", 0));
               f.max_rate_per_s = uint32_t((*a).get_number("max_rate_per_s", 50));
               f.timeout_s = uint32_t((*a).get_number("timeout_s", 30));
               // Emit matching recent lines now (bounded); the ctrl stream layer
               // stops when the response completes — live push continues via the
               // gRPC tail fanout for HQ/Configuration.
               auto lines = core->store().read_tail(f.sources.empty() ? first_source(core) : f.sources[0], 1000);
               if (!lines.ok()) return CommandResult::from_error(lines.error);
               JValue arr = JValue::array();
               for (const auto& l : *lines)
               {
                   if (!f.sources.empty())
                   {
                       bool ok = false;
                       for (const auto& s : f.sources) ok |= (s == l.source);
                       if (!ok) continue;
                   }
                   JValue o = JValue::object();
                   o.set("source", JValue(l.source));
                   o.set("level", JValue(l.level));
                   o.set("message", JValue(l.message));
                   o.set("mono_ns", JValue(double(l.mono_ns)));
                   arr.push(o);
               }
               return CommandResult::good(arr.dump());
           }});
    t.add({"bundle-create", "create a support bundle for a time window", {{"sources", false, "comma-separated sources (all)"}, {"timeout_s", false, "ignored"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               std::vector<std::string> sources;
               std::string srcs = (*a).get_string("sources");
               if (srcs.empty())
               {
                   for (const auto& s : core->store().sources()) sources.push_back(s.name);
               }
               else
               {
                   size_t pos = 0;
                   while (pos < srcs.size())
                   {
                       size_t c = srcs.find(',', pos);
                       if (c == std::string::npos) c = srcs.size();
                       if (c > pos) sources.push_back(srcs.substr(pos, c - pos));
                       pos = c + 1;
                   }
               }
               auto m = core->bundles().create(core->store(), sources, core->clock().mono_ns());
               if (!m.ok()) return CommandResult::from_error(m.error);
               JValue o = JValue::object();
               o.set("handle", JValue(m->handle));
               o.set("size_bytes", JValue(double(m->size_bytes)));
               o.set("sha256", JValue(m->sha256));
               return CommandResult::good(o.dump());
           }});
    t.add({"bundle-list", "list bundle handles", {}, [core](const std::string&) {
               JValue arr = JValue::array();
               for (const auto& m : core->bundles().list()) arr.push(bundle_json(m));
               return CommandResult::good(arr.dump());
           }});
    t.add({"bundle-meta", "bundle metadata by handle", {{"handle", true, "bundle handle"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               auto m = core->bundles().meta((*a).get_string("handle"));
               if (!m.ok()) return CommandResult::from_error(m.error);
               return CommandResult::good(bundle_json(*m));
           }});
    t.add({"bundle-fetch", "locate a bundle payload for bulk transfer by handle", {{"handle", true, "bundle handle"}}, [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               const std::string handle = (*a).get_string("handle");
               auto m = core->bundles().meta(handle);
               if (!m.ok()) return CommandResult::from_error(m.error);
               // §7.7: the control API carries a handle and metadata; the
               // payload never traverses it. The caller reads the bytes from the
               // bulk socket, so a 40 MB support archive does not get base64'd
               // into a JSON envelope and pushed through the path that also
               // carries alarms.
               JValue o = JValue::object();
               o.set("handle", JValue(m->handle));
               o.set("size_bytes", JValue(double(m->size_bytes)));
               o.set("sha256", JValue(m->sha256));
               o.set("bulk_socket", JValue(core->bulk_socket_path()));
               o.set("transfer", JValue(std::string("bulk")));
               return CommandResult::good(o.dump());
           }});
    t.add({"crash-report",
           "capture crash forensics for an abnormal exit",
           {{"daemon", true, "daemon name"}, {"coredump", false, "coredump path (resolved from the platform when omitted)"}, {"last_lines", false, "JSON array of last log lines"}, {"tail_lines", false, "how many stored lines to fold in (default 200)"}},
           [core](const std::string& args) {
               auto a = args_object(args);
               if (!a.ok()) return CommandResult::from_error(a.error);
               std::string daemon = (*a).get_string("daemon");
               std::vector<std::string> lines;
               auto j = json_parse((*a).get_string("last_lines"));
               if (j.ok() && (*j).is_array())
                   for (const auto& item : (*j).items()) lines.push_back(item.as_string());
               // Caller named no coredump: find one. istate knows a daemon died,
               // not where the kernel put the core.
               std::string coredump = (*a).get_string("coredump");
               if (coredump.empty()) coredump = core->resolve_coredump(daemon);
               // Caller supplied no tail either: take it from the store, which is
               // the whole point of "last-N log lines on abnormal exit".
               if (lines.empty())
               {
                   uint32_t want = uint32_t((*a).get_number("tail_lines", 200));
                   for (const auto& src : core->store().sources())
                   {
                       auto tail = core->store().read_tail(src.name, want);
                       if (!tail.ok()) continue;
                       for (const auto& l : *tail)
                           if (l.message.find(daemon) != std::string::npos) lines.push_back(l.message);
                   }
               }
               auto c = core->forensics().capture(daemon, coredump, lines, 16 * 1024 * 1024);
               if (!c.ok()) return CommandResult::from_error(c.error);
               JValue o = JValue::object();
               o.set("daemon", JValue(c->daemon));
               o.set("coredump_bytes", JValue(double(c->coredump_bytes)));
               o.set("tail_lines", JValue(double(c->tail_lines)));
               return CommandResult::good(o.dump());
           }});
    t.add({"stats", "storage usage and retention stats", {}, [core](const std::string&) {
               JValue o = JValue::object();
               o.set("total_bytes", JValue(double(core->store().total_bytes())));
               return CommandResult::good(o.dump());
           }});
    t.add({"prune", "enforce retention and budget now", {}, [core](const std::string&) {
               auto st = core->store().enforce_retention(core->clock().mono_ns());
               JValue o = JValue::object();
               o.set("files_removed", JValue(double(st.files_removed)));
               o.set("bytes_freed", JValue(double(st.bytes_freed)));
               return CommandResult::good(o.dump());
           }});
    return t;
}

/// @brief Public wrapper around the internal level ranking.
/// @param l Level name to rank.
/// @return The level index (TRACE=0 .. CRITICAL=5).
int level_index_public(const std::string& l)
{
    static const char* kLevels[] = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"};
    for (int i = 0; i < 6; ++i)
        if (l == kLevels[i]) return i;
    return -1;
}

/// @brief First configured source name, or a default when none are configured.
/// @param core Domain core.
/// @return The first configured source name, or a default.
std::string first_source(LogCore* core)
{
    return core->store().sources().empty() ? std::string() : core->store().sources()[0].name;
}

/// @brief Wire the channel, config sink and handlers together.
/// @param core Domain core.
/// @param log Logger.
/// @param table Command table.
/// @param hub_target istate gRPC target.
/// @param build_version Build version to advertise.
LogSpoke::LogSpoke(LogCore* core, Logger* log, const CommandTable* table, const std::string& hub_target, const std::string& build_version) : core_(core), log_(log), table_(table)
{
    using ChannelT = SpokeChannel<logmanager::LogHub, logmanager::LogSpokeFrame, logmanager::LogHubFrame>;
    ChannelT::Options o;
    o.target = hub_target;
    o.daemon = "logmanager";
    o.build_version = build_version;
    o.capabilities = {"tail", "bundles", "rotation", "crash-forensics"};
    channel_ = std::make_unique<ChannelT>(std::move(o), *log);
    channel_->set_hub_handler([this](const logmanager::LogHubFrame& hf) {
        if (hf.has_command())
        {
            common::Ack ack = dispatch_to_ack(table_, hf.command());
            channel_->send_ack(ack);
        }
    });
    core_->set_sink([this](const LogLine& l) { send_line(l); });
    // Until a schema is installed, deliveries still apply — just unvalidated.
    // A packaging slip must not leave a daemon running unconfigured (R10).
    set_schema(JValue::object());
}

/// @brief Install the schema every config delivery must satisfy.
/// @param schema Schema every config delivery must satisfy.
void LogSpoke::set_schema(JValue schema)
{
    config_sink_ = std::make_unique<ConfigSink>("logmanager", std::move(schema), [this](const ConfigDocument& doc) { return core_->apply_config(doc); });
    channel_->set_config_handler([this](const common::ConfigDelivery& delivery) { return config_sink_->deliver(delivery.yaml(), delivery.revision()); });
}

/// @brief Start the transport channel.
void LogSpoke::start()
{
    channel_->start();
}
/// @brief Stop the transport channel.
void LogSpoke::stop()
{
    channel_->stop();
}

/// @brief Queue a collected log line to istate.
/// @param l Log line to queue.
void LogSpoke::send_line(const LogLine& l)
{
    channel_->enqueue([&l](logmanager::LogSpokeFrame& f) {
        auto* t = f.mutable_line();
        t->mutable_ts()->set_mono_ns(l.mono_ns);
        t->set_source(l.source);
        t->set_message(l.message);
        if (l.level == "CRITICAL")
            t->set_level(common::SEV_CRITICAL);
        else if (l.level == "ERROR")
            t->set_level(common::SEV_MAJOR);
        else if (l.level == "WARNING")
            t->set_level(common::SEV_WARNING);
        else
            t->set_level(common::SEV_INFO);
    });
}

/// @brief Queue a bundle announcement to istate.
/// @param m Bundle metadata to queue.
void LogSpoke::send_bundle(const BundleMeta& m)
{
    channel_->enqueue([&m](logmanager::LogSpokeFrame& f) {
        auto* b = f.mutable_bundle();
        b->set_handle(m.handle);
        b->set_meta_json(m.window_json);
        b->set_size_bytes(m.size_bytes);
        b->set_sha256(m.sha256);
    });
}

/// @brief Queue a crash-forensics report to istate.
/// @param daemon Daemon that crashed.
/// @param coredump Stored coredump path.
/// @param tail Log-tail lines folded in.
void LogSpoke::send_crash(const std::string& daemon, const std::string& coredump, uint32_t tail)
{
    channel_->enqueue([&](logmanager::LogSpokeFrame& f) {
        auto* c = f.mutable_crash();
        c->set_daemon(daemon);
        c->set_coredump_handle(coredump);
        c->set_tail_lines(tail);
        c->mutable_ts()->set_mono_ns(core_->clock().mono_ns());
    });
}

// ---- LogCollector (§7.7) --------------------------------------------------

/// @brief Construct the collector and its periodic worker.
LogCollector::LogCollector(LogCore* core, platform::IPlatform* plat, IFileSystem* fs, TimeSource* ts, Logger* log) : core_(core), plat_(plat), fs_(fs), ts_(ts), log_(log), worker_("logmanager-collect", 2.0, [this] { (void)poll(); })
{
}

/// @brief Start the periodic worker.
void LogCollector::start()
{
    worker_.start();
}
/// @brief Stop the periodic worker.
void LogCollector::stop()
{
    worker_.stop();
}

// Parses "<LEVEL> message" where the line carries one, else falls back to the
// source's configured level. Journal "short" output and most application
// sinks put the level in the first bracketed or bare token.
/// @brief Best-effort level extraction from a raw log line.
/// @param line Raw log line.
/// @param dflt Level to use when none is detected.
/// @return The detected level name.
static std::string parse_level(const std::string& line, const std::string& dflt)
{
    static const char* kLevels[] = {"CRITICAL", "ERROR", "WARNING", "WARN", "INFO", "DEBUG", "TRACE"};
    // Look only in the first 64 characters: a level token appears in the
    // prefix, and scanning the whole line would match the word "error"
    // occurring anywhere in a message body.
    const std::string head = line.substr(0, 64);
    for (const char* lv : kLevels)
    {
        auto pos = head.find(lv);
        if (pos != std::string::npos) return std::string(lv) == "WARN" ? "WARNING" : lv;
    }
    return dflt;
}

/// @brief Split raw text into lines, filter and store them.
/// @param spec Source the lines belong to.
/// @param raw Raw collected text.
/// @return Lines emitted and stored.
size_t LogCollector::emit(const SourceSpec& spec, const std::string& raw)
{
    if (raw.empty()) return 0;
    LogLine l;
    l.mono_ns = ts_->mono_ns();
    l.source = spec.name;
    l.level = parse_level(raw, spec.level.empty() ? "INFO" : spec.level);
    l.message = raw;
    Error e = core_->store().append(spec.name, l);
    if (!e.ok())
    {
        if (log_) log_->warning("logmanager", "append to '" + spec.name + "' failed: " + e.str());
        return 0;
    }
    // Feed the live-tail fan-out too, so a tail session sees collected lines
    // and not only what a test injected (§7.7: tail streams to istate).
    core_->emit(l);
    ++lines_;
    return 1;
}

/// @brief Collect one pass from a journal source.
/// @param spec Journal source to collect.
/// @return Lines appended.
size_t LogCollector::collect_journal(const SourceSpec& spec)
{
    if (!plat_) return 0;
    // "path" carries the unit for a journal source; the source name is the
    // fallback so a minimal config still works.
    const std::string unit = spec.path.empty() ? spec.name : spec.path;
    auto lines = plat_->journal_lines(unit, 500);
    if (!lines.ok() || lines->empty()) return 0;

    Cursor& cur = cursors_[spec.name];
    size_t start = 0;
    if (!cur.last_line.empty())
    {
        // Resume after the last line we handed on. Not found means the window
        // moved past it (a burst, or a journal rotation) — take the whole batch
        // rather than silently dropping the gap.
        for (size_t i = lines->size(); i-- > 0;)
        {
            if ((*lines)[i] == cur.last_line)
            {
                start = i + 1;
                break;
            }
        }
    }
    size_t n = 0;
    for (size_t i = start; i < lines->size(); ++i) n += emit(spec, (*lines)[i]);
    cur.last_line = lines->back();
    return n;
}

/// @brief Collect one pass from a file source.
/// @param spec File source to collect.
/// @return Lines appended.
size_t LogCollector::collect_file(const SourceSpec& spec)
{
    if (!fs_ || spec.path.empty()) return 0;
    auto size = fs_->file_size(spec.path);
    if (!size.ok()) return 0; // sink not created yet: normal, not an error
    Cursor& cur = cursors_[spec.name];
    if (*size < cur.offset) cur.offset = 0; // rotated or truncated by the app
    if (*size == cur.offset) return 0;
    auto text = fs_->read(spec.path);
    if (!text.ok()) return 0;
    if (cur.offset > text->size()) cur.offset = 0;

    std::string chunk = text->substr(cur.offset);
    // Only consume up to the last newline: a partial trailing line is a write
    // still in flight, and collecting half of it would corrupt the record.
    auto last_nl = chunk.rfind('\n');
    if (last_nl == std::string::npos) return 0;
    chunk.resize(last_nl + 1);
    cur.offset += chunk.size();

    size_t n = 0;
    size_t pos = 0;
    while (pos < chunk.size())
    {
        size_t nl = chunk.find('\n', pos);
        if (nl == std::string::npos) break;
        if (nl > pos) n += emit(spec, chunk.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return n;
}

/// @brief One collection pass over every configured source.
size_t LogCollector::poll()
{
    if (!core_->config().enabled) return 0;
    size_t n = 0;
    for (const auto& spec : core_->store().sources())
    {
        if (spec.kind == "journal") { n += collect_journal(spec); }
        else if (spec.kind == "file" || spec.kind == "app") { n += collect_file(spec); }
        // An unknown kind is a config error the schema already rejects; nothing
        // to do here beyond leaving it uncollected.
    }
    return n;
}
} // namespace logmanager
} // namespace instr
