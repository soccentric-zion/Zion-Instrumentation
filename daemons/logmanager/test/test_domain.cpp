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
 * @file daemons/logmanager/test/test_domain.cpp
 * @brief logmanager domain (REQ §12)
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <algorithm>

#include <gtest/gtest.h>

#include "instr/common/filesystem.h"
#include "instr/logmanager/daemon.h"
#include "instr/platform/simulated.h"

#include "instr/common/crypto.h"
#include "instr/common/tar.h"

using namespace instr;
using namespace instr::logmanager;
using namespace instr::logmanager::domain;

struct Fixture
{
    SimulatedFileSystem fs;
    ManualTimeSource ts;
    std::string base = "/pst/state/logs";
    LogCore core{fs, base, ts}; // NOLINT
    /// @brief Provisions a temp log store, bundle dir and manual clock.
    Fixture()
    {
        fs.mkdirs(base);
        std::vector<SourceSpec> sources;
        SourceSpec s;
        s.name = "app";
        s.kind = "file";
        s.path = "/var/log/app.log";
        s.level = "INFO";
        s.rotate_size_bytes = 100; // tiny for tests
        s.retention_files = 2;
        sources.push_back(s);
        core.store().configure(sources);
    }
};

/// @brief Build a LogLine helper for tail/collector tests.
/// @param mono Monotonic timestamp.
/// @param lvl Level name.
/// @param msg Line text.
/// @return The constructed LogLine.
static LogLine line(uint64_t mono, const std::string& lvl, const std::string& msg)
{
    LogLine l;
    l.mono_ns = mono;
    l.source = "app";
    l.level = lvl;
    l.message = msg;
    return l;
}

/// @brief Appends rotate the active file when the size limit is exceeded.
TEST(LogStore, AppendsAndRotatesBySize)
{
    Fixture f;
    for (int i = 0; i < 20; ++i) EXPECT_TRUE(f.core.store().append("app", line(f.ts.mono_ns(), "INFO", std::to_string(i))).ok());
    auto files = f.core.store().files_for("app");
    // Active + at least one rotated file after exceeding 100 bytes.
    EXPECT_GE(files.size(), 2u);
}

/// @brief Retention keeps only the configured number of rotated files.
TEST(LogStore, RetentionKeepsOnlyConfiguredRotations)
{
    Fixture f;
    for (int round = 0; round < 6; ++round)
    {
        for (int i = 0; i < 20; ++i) f.core.store().append("app", line(f.ts.mono_ns(), "INFO", "r" + std::to_string(round)));
    }
    auto st = f.core.store().enforce_retention(f.ts.mono_ns());
    auto files = f.core.store().files_for("app");
    // active + 2 rotations max
    EXPECT_LE(files.size(), 3u);
    // append() already caps the count as it rotates, so a sweep afterwards
    // finds nothing left to do. That is the invariant worth stating: the
    // budget is never exceeded between sweeps.
    EXPECT_EQ(st.files_removed, 0u);
}

/// @brief Runtime level control filters what gets collected (§7.7).
TEST(LogStore, RuntimeLevelControlFiltersCollection)
{
    Fixture f;
    f.core.store().set_runtime_level("app", "ERROR");
    EXPECT_TRUE(f.core.store().append("app", line(1, "INFO", "hidden")).ok()); // filtered, no error
    auto tail = f.core.store().read_tail("app", 100);
    ASSERT_TRUE(tail.ok());
    EXPECT_TRUE(tail->empty()); // INFO below ERROR level was not collected
    EXPECT_TRUE(f.core.store().append("app", line(2, "ERROR", "visible")).ok());
    tail = f.core.store().read_tail("app", 100);
    ASSERT_EQ(tail->size(), 1u);
    EXPECT_EQ((*tail)[0].message, "visible");
}

/// @brief Tail sessions filter by source, level and time.
TEST(LogTail, FiltersBySourceLevelSince)
{
    TailSession s(
        []() {
            TailFilter f;
            f.sources = {"app"};
            f.min_level = "WARNING";
            f.since_mono_ns = 500;
            return f;
        }(),
        0);
    EXPECT_FALSE(s.accept(line(100, "ERROR", "too old"), 1)); // since filter
    EXPECT_FALSE(s.accept(line(600, "INFO", "too quiet"), 1)); // level filter
    EXPECT_TRUE(s.accept(line(600, "WARNING", "ok"), 1));
    EXPECT_EQ(s.emitted(), 1u);
}

/// @brief Tail sessions rate-limit and auto-stop at the timeout.
TEST(LogTail, RateLimitsAndAutoStops)
{
    TailFilter f;
    f.max_rate_per_s = 3;
    f.timeout_s = 10;
    TailSession s(f, 0);
    uint64_t now = 0;
    for (int i = 0; i < 10; ++i) s.accept(line(1, "INFO", "x"), now);
    EXPECT_EQ(s.emitted(), 3u);
    EXPECT_EQ(s.dropped(), 7u);
    // Auto-stop after timeout.
    EXPECT_TRUE(s.expired(11ull * 1'000'000'000ull));
    EXPECT_FALSE(s.accept(line(1, "INFO", "after"), 11ull * 1'000'000'000ull));
}

/// @brief Bundles tar a window with checksum metadata; fetch verifies integrity.
TEST(LogBundles, CreateMetaFetchIntegrity)
{
    Fixture f;
    for (int i = 0; i < 5; ++i) f.core.store().append("app", line(i, "INFO", "hello " + std::to_string(i)));
    auto m = f.core.bundles().create(f.core.store(), {"app"}, f.ts.mono_ns());
    ASSERT_TRUE(m.ok());
    EXPECT_NE(m->handle.size(), 0);
    EXPECT_GT(m->size_bytes, 0u);
    auto meta = f.core.bundles().meta(m->handle);
    ASSERT_TRUE(meta.ok());
    EXPECT_EQ(meta->sha256, m->sha256);
    auto payload = f.core.bundles().payload(m->handle);
    ASSERT_TRUE(payload.ok());
    // Payload parses as ustar and contains the app log.
    auto entries = tar::read_ustar(*payload);
    ASSERT_TRUE(entries.ok());
    ASSERT_FALSE(entries->empty());
    EXPECT_NE((*entries)[0].data.find("hello 4"), std::string::npos);
    EXPECT_EQ(meta->sha256, crypto::sha256(*payload).hex());
}

/// @brief An unknown bundle handle is a loud error, never an arbitrary-file read.
TEST(LogBundles, UnknownHandleIsLoud)
{
    Fixture f;
    auto m = f.core.bundles().meta("nope");
    EXPECT_FALSE(m.ok());
    EXPECT_EQ(m.error.code, Errc::NotFound);
}

/// @brief Crash forensics captures a bounded coredump plus the daemon's last lines.
TEST(LogForensics, CapturesBoundedCoredumpAndTail)
{
    Fixture f;
    f.fs.write("/var/lib/coredumps/istate.core", std::string(1000, 'x'));
    std::vector<std::string> lines;
    for (int i = 0; i < 300; ++i) lines.push_back("log line " + std::to_string(i));
    auto c = f.core.forensics().capture("istate", "/var/lib/coredumps/istate.core", lines, 100);
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c->coredump_bytes, 100u); // bounded
    EXPECT_GT(c->tail_lines, 0u);
    bool saw_tail = false;
    for (const auto& fl : f.core.forensics().captured_files())
        if (fl.find("istate.tail.log") != std::string::npos) saw_tail = true;
    EXPECT_TRUE(saw_tail);
}
// §7.7: "rotation by size and age, retention per source". Retention by count
// was implemented; retention by age was a comment saying the simulated
// filesystem had no mtime. It has one now, so the policy is real.
/// @brief Age-based retention removes files past max_age (§7.7).
TEST(LogStore, RetainsByAgeNotOnlyByCount)
{
    SimulatedFileSystem fs;
    LogStore store(fs, "/logs");
    SourceSpec s;
    s.name = "app";
    s.kind = "file";
    s.rotate_size_bytes = 64; // rotate often so we accumulate files
    s.retention_files = 100; // count retention must not be what prunes
    s.max_age_s = 7 * 24 * 3600; // a week
    store.configure({s});

    auto append = [&](const std::string& text) {
        LogLine l;
        l.source = "app";
        l.level = "INFO";
        l.message = text;
        (void)store.append("app", l);
    };

    // Write enough to rotate a few times, at a point in time eight days ago.
    fs.set_now_unix_s(1'700'000'000);
    for (int i = 0; i < 12; ++i) append("old entry number " + std::to_string(i));
    const size_t after_old = store.files_for("app").size();
    ASSERT_GT(after_old, 1u) << "expected rotation to have produced archived files";

    // Move the clock forward eight days and write again, so the active file is
    // recent while the rotated ones are stale.
    fs.set_now_unix_s(1'700'000'000 + 8 * 24 * 3600);
    for (int i = 0; i < 12; ++i) append("new entry number " + std::to_string(i));

    auto st = store.enforce_retention(0);
    EXPECT_GT(st.files_removed, 0u) << "nothing was pruned by age";

    // Everything left is within the window, and the active file survives.
    const auto remaining = store.files_for("app");
    EXPECT_NE(std::find(remaining.begin(), remaining.end(), std::string("app.log")), remaining.end()) << "the active file must never be aged out";
    for (const auto& f : remaining)
    {
        if (f == "app.log") continue;
        auto m = fs.file_mtime_s(fs.join("/logs", f));
        ASSERT_TRUE(m.ok());
        EXPECT_LE(1'700'000'000 + 8 * 24 * 3600 - *m, int64_t(s.max_age_s)) << f << " is older than the retention window and survived";
    }
}

/// @brief max_age=0 disables age retention (count-only).
TEST(LogStore, AgeRetentionIsDisabledWhenMaxAgeIsZero)
{
    SimulatedFileSystem fs;
    LogStore store(fs, "/logs");
    SourceSpec s;
    s.name = "app";
    s.kind = "file";
    s.rotate_size_bytes = 64;
    s.retention_files = 100;
    s.max_age_s = 0; // keep regardless of age
    store.configure({s});
    fs.set_now_unix_s(1'700'000'000);
    for (int i = 0; i < 12; ++i)
    {
        LogLine l;
        l.source = "app";
        l.level = "INFO";
        l.message = "entry " + std::to_string(i);
        (void)store.append("app", l);
    }
    const size_t before = store.files_for("app").size();
    fs.set_now_unix_s(1'700'000'000 + 3650 * 24 * 3600); // ten years later
    store.enforce_retention(0);
    EXPECT_EQ(store.files_for("app").size(), before);
}

// ---- collection (§7.7) ------------------------------------------------------
//
// "Collects from the journal and from application log sinks" is the daemon's
// primary job. Before LogCollector existed, LogStore::append() had no caller
// outside this file: the daemon rotated, pruned, tailed and bundled a store
// that nothing ever wrote to. These tests exist so that cannot recur.

struct CollectFixture
{
    SimulatedFileSystem fs;
    ManualTimeSource ts;
    platform::SimulatedPlatform plat;
    StderrSink sink;
    Logger log{sink, ts};
    std::string base = "/pst/state/logs";
    LogCore core{fs, base, ts};

    /// @brief Provisions collector fixtures: store, sink file and platform.
    CollectFixture()
    {
        fs.mkdirs(base);
    }

    void configure(std::vector<SourceSpec> sources)
    {
        core.store().configure(std::move(sources));
    }

    LogCollector make()
    {
        return LogCollector(&core, &plat, &fs, &ts, &log);
    }

    static SourceSpec file_source(const std::string& name, const std::string& path, const std::string& level = "INFO")
    {
        SourceSpec s;
        s.name = name;
        s.kind = "app";
        s.path = path;
        s.level = level;
        s.rotate_size_bytes = 1 << 20;
        s.retention_files = 3;
        return s;
    }
    static SourceSpec journal_source(const std::string& name, const std::string& unit)
    {
        SourceSpec s;
        s.name = name;
        s.kind = "journal";
        s.path = unit;
        s.level = "INFO";
        s.rotate_size_bytes = 1 << 20;
        s.retention_files = 3;
        return s;
    }
};

/// @brief Application log sinks are collected into the store (§7.7).
TEST(LogCollect, ApplicationSinkIsCollectedIntoTheStore)
{
    CollectFixture f;
    f.configure({CollectFixture::file_source("app", "/var/log/app.log")});
    f.fs.write("/var/log/app.log", "INFO first\nINFO second\n");

    auto c = f.make();
    EXPECT_EQ(c.poll(), 2u);
    auto tail = f.core.store().read_tail("app", 10);
    ASSERT_TRUE(tail.ok());
    ASSERT_EQ(tail->size(), 2u);
    EXPECT_NE((*tail)[0].message.find("first"), std::string::npos);
    EXPECT_NE((*tail)[1].message.find("second"), std::string::npos);
}

/// @brief Dedup: already-collected lines are not collected twice.
TEST(LogCollect, AlreadyCollectedLinesAreNotCollectedTwice)
{
    CollectFixture f;
    f.configure({CollectFixture::file_source("app", "/var/log/app.log")});
    f.fs.write("/var/log/app.log", "INFO one\n");
    auto c = f.make();
    EXPECT_EQ(c.poll(), 1u);
    EXPECT_EQ(c.poll(), 0u) << "a second pass over unchanged input must collect nothing";

    f.fs.write("/var/log/app.log", "INFO two\n", /*append=*/true);
    EXPECT_EQ(c.poll(), 1u) << "only the appended line";
    EXPECT_EQ(c.lines_collected(), 2u);
}

/// @brief A partial trailing line is left for the next pass (no torn lines).
TEST(LogCollect, PartialTrailingLineIsLeftForTheNextPass)
{
    // A write still in flight must not be collected half-formed.
    CollectFixture f;
    f.configure({CollectFixture::file_source("app", "/var/log/app.log")});
    f.fs.write("/var/log/app.log", "INFO complete\nINFO half");
    auto c = f.make();
    EXPECT_EQ(c.poll(), 1u);

    f.fs.write("/var/log/app.log", "-written\n", /*append=*/true);
    EXPECT_EQ(c.poll(), 1u);
    auto tail = f.core.store().read_tail("app", 10);
    ASSERT_TRUE(tail.ok());
    ASSERT_EQ(tail->size(), 2u);
    EXPECT_NE((*tail)[1].message.find("half-written"), std::string::npos) << "the partial line must arrive whole, not split across two records";
}

/// @brief A truncated sink restarts collection from the beginning.
TEST(LogCollect, SinkTruncationRestartsFromTheBeginning)
{
    // An application rotating its own log shrinks the file. Holding the old
    // offset would silently stop collecting for good.
    CollectFixture f;
    f.configure({CollectFixture::file_source("app", "/var/log/app.log")});
    f.fs.write("/var/log/app.log", "INFO aaaaaaaaaaaaaaaaaaaa\nINFO bbbbbbbbbbbbbbbbbbbb\n");
    auto c = f.make();
    EXPECT_EQ(c.poll(), 2u);

    f.fs.write("/var/log/app.log", "INFO c\n"); // truncating rewrite
    EXPECT_EQ(c.poll(), 1u);
}

/// @brief Lines below the source's runtime level are not collected.
TEST(LogCollect, BelowLevelLinesAreNotCollected)
{
    CollectFixture f;
    f.configure({CollectFixture::file_source("app", "/var/log/app.log", "ERROR")});
    f.fs.write("/var/log/app.log", "DEBUG noise\nERROR real\n");
    auto c = f.make();
    c.poll();
    auto tail = f.core.store().read_tail("app", 10);
    ASSERT_TRUE(tail.ok());
    ASSERT_EQ(tail->size(), 1u);
    EXPECT_NE((*tail)[0].message.find("real"), std::string::npos);
}

/// @brief Journal sources are collected with dedup by last line.
TEST(LogCollect, JournalIsCollectedAndDeduplicated)
{
    CollectFixture f;
    f.configure({CollectFixture::journal_source("journal-istate", "instr-istate")});
    f.plat.set_journal({{"instr-istate", {"INFO boot", "INFO ready"}}});
    auto c = f.make();
    EXPECT_EQ(c.poll(), 2u);
    EXPECT_EQ(c.poll(), 0u) << "the journal window has no new lines";

    f.plat.set_journal({{"instr-istate", {"INFO boot", "INFO ready", "WARNING late"}}});
    EXPECT_EQ(c.poll(), 1u) << "only the line past the cursor";
}

/// @brief A journal gap collects the whole window rather than dropping it.
TEST(LogCollect, JournalGapCollectsTheWholeWindowRatherThanDroppingIt)
{
    // The journal has no cursor API. If the window has moved past the last
    // line we saw — a burst, or a rotation — taking the whole batch is right;
    // silently collecting nothing is not.
    CollectFixture f;
    f.configure({CollectFixture::journal_source("journal-istate", "instr-istate")});
    f.plat.set_journal({{"instr-istate", {"INFO a"}}});
    auto c = f.make();
    EXPECT_EQ(c.poll(), 1u);

    f.plat.set_journal({{"instr-istate", {"INFO x", "INFO y", "INFO z"}}});
    EXPECT_EQ(c.poll(), 3u);
}

/// @brief Collection stops while the daemon is disabled by config.
TEST(LogCollect, CollectionStopsWhileDisabled)
{
    CollectFixture f;
    f.configure({CollectFixture::file_source("app", "/var/log/app.log")});
    f.fs.write("/var/log/app.log", "INFO one\n");
    ConfigDocument doc;
    doc.set("enabled", "false", Origin::Local);
    (void)f.core.apply_config(doc);
    auto c = f.make();
    EXPECT_EQ(c.poll(), 0u);
}

/// @brief A missing sink file is tolerated (no crash, no error spam).
TEST(LogCollect, MissingSinkFileIsNotAnError)
{
    // An application that has not started yet is normal, not a fault.
    CollectFixture f;
    f.configure({CollectFixture::file_source("app", "/var/log/not-created-yet.log")});
    auto c = f.make();
    EXPECT_EQ(c.poll(), 0u);
}

/// @brief Collected lines reach the live tail fanout.
TEST(LogCollect, CollectedLinesReachTheLiveTailFanout)
{
    // §7.7: tail server-streams to istate. A tail that only ever sees injected
    // test lines is not a tail.
    CollectFixture f;
    f.configure({CollectFixture::file_source("app", "/var/log/app.log")});
    std::vector<LogLine> seen;
    f.core.set_sink([&](const LogLine& l) { seen.push_back(l); });
    f.fs.write("/var/log/app.log", "INFO streamed\n");
    auto c = f.make();
    c.poll();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].source, "app");
}
