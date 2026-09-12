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
 * @file daemons/faults/test/test_domain.cpp
 * @brief faults domain (REQ §12)
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <fstream>
#include <set>
#include <sstream>

#include <gtest/gtest.h>

#include "instr/common/timestamps.h"
#include "instr/faults/domain.h"

using namespace instr;
using namespace instr::faults;
using namespace instr::faults::domain;

static const char* kRules = R"yaml(
rules:
  - name: bus_error
    pattern: "AER.*Bus Error"
    fault_id: 0x5_05_1_0001
    severity: MAJOR
    x733_type: equipment
    probable_cause: bus-error
    dedup_key: "bus:$device"
    dedup_window_s: 60
    rate_limit_per_s: 100
  - name: oom_kill
    pattern: "Out of memory: Killed process"
    fault_id: 0x5_05_3_0008
    severity: MAJOR
    x733_type: processing-error
    probable_cause: memory-exhaustion
    dedup_key: "oom:$process"
    dedup_window_s: 120
    rate_limit_per_s: 100
)yaml";

/// @brief Rule ids are validated against the §8 identity scheme at load.
TEST(FaultRules, LoadValidatesIds)
{
    auto rules = load_rules(kRules);
    ASSERT_TRUE(rules.ok());
    ASSERT_EQ(rules->size(), 2u);
    EXPECT_EQ((*rules)[0].fault_id, 0x50510001u);
    EXPECT_EQ((*rules)[0].severity, common::SEV_MAJOR);
    EXPECT_EQ((*rules)[1].name, "oom_kill");
}

/// @brief Rules claiming a non-fault domain id are rejected.
TEST(FaultRules, RejectsNonFaultDomainIds)
{
    const char* bad = R"yaml(
rules:
  - name: bad
    pattern: "x"
    fault_id: 0x1_04_1_0007
    severity: MAJOR
)yaml";
    auto r = load_rules(bad);
    EXPECT_FALSE(r.ok()); // fault ids must be in the fault domain (§8)
}

/// @brief A bad regex in the rules file is a loud load error.
TEST(FaultRules, RejectsBadRegex)
{
    const char* bad = R"yaml(
rules:
  - name: bad
    pattern: "([unclosed"
    fault_id: 0x5_05_1_0001
)yaml";
    auto r = load_rules(bad);
    EXPECT_FALSE(r.ok());
}

/// @brief Rules match kernel lines and classify severity/type/cause.
TEST(FaultEngine, MatchesAndClassifies)
{
    auto rules = load_rules(kRules);
    ASSERT_TRUE(rules.ok());
    RuleEngine engine(*rules);
    auto m = engine.match("pcieport 0000:01:00.0: AER: Bus Error detected");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ((*m)->name, "bus_error");
    EXPECT_FALSE(engine.match("normal kernel noise").has_value());
}

/// @brief $device-style dedup templates extract tokens from real lines.
TEST(FaultEngine, DedupKeyExtraction)
{
    auto key = RuleEngine::render_dedup_key("bus:$device", "mmcblk0: I/O error on dev mmcblk0");
    EXPECT_NE(key.find("mmcblk0"), std::string::npos);
    EXPECT_NE(key.find("bus:"), std::string::npos);
    auto key2 = RuleEngine::render_dedup_key("oom:$process", "Out of memory: Killed process 4321 (myapp)");
    EXPECT_NE(key2.find("myapp"), std::string::npos);
}

/// @brief Identical keys inside the window merge occurrence counts.
TEST(FaultProcessor, DedupsWithinWindow)
{
    auto rules = load_rules(kRules);
    ASSERT_TRUE(rules.ok());
    FaultProcessor proc(*rules);
    uint64_t t = 1'000'000'000ull;
    auto r1 = proc.feed({"AER: Bus Error on 0000:01:00.0"}, t, false);
    ASSERT_EQ(r1.size(), 1u);
    EXPECT_EQ(r1[0].occurrences, 1u);
    auto r2 = proc.feed({"AER: Bus Error on 0000:01:00.0"}, t + 1'000'000'000ull, false);
    ASSERT_EQ(r2.size(), 1u);
    EXPECT_EQ(r2[0].occurrences, 2u);
    EXPECT_EQ(proc.stats().duplicates_merged, 1u);
}

/// @brief Records recovered from the boot ring carry the backfill flag.
TEST(FaultProcessor, BackfillFlagged)
{
    auto rules = load_rules(kRules);
    ASSERT_TRUE(rules.ok());
    FaultProcessor proc(*rules);
    auto recs = proc.feed({"AER: Bus Error on 0000:01:00.0"}, 0, true);
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(recs[0].backfill);
    EXPECT_EQ(proc.stats().backfill_lines, 1u);
}

/// @brief A flood is rate-limited with exactly one suppression summary.
TEST(FaultProcessor, RateLimitSuppressesFloodWithSummary)
{
    auto rules = load_rules(kRules);
    ASSERT_TRUE(rules.ok());
    std::vector<FaultRule> limited = *rules;
    limited[0].rate_limit_per_s = 2;
    limited[0].dedup_key = "flood"; // same key each time
    FaultProcessor proc(limited);
    std::vector<std::string> flood;
    for (int i = 0; i < 1000; ++i) flood.push_back("AER: Bus Error on 0000:01:00.0");
    uint64_t t = 1'000'000'000ull;
    auto recs = proc.feed(flood, t, false);
    // 2 pass (burst), 998 suppressed; the summary lands on the next acquire.
    uint64_t real = 0;
    for (const auto& r : recs)
        if (r.occurrences > 0) ++real;
    EXPECT_LE(real, 3u);
    auto later = proc.feed({"AER: Bus Error on 0000:01:00.0"}, t + 2'000'000'000ull, false);
    ASSERT_FALSE(later.empty());
    EXPECT_NE(later[0].raw_line.find("suppressed "), std::string::npos);
    EXPECT_EQ(later[0].raw_line, "suppressed 998 events");
    EXPECT_GT(proc.stats().faults_raised, 0u);
}

/// @brief Ordinary kernel lines never emit faults.
TEST(FaultProcessor, NonMatchingLinesNeverEmit)
{
    auto rules = load_rules(kRules);
    ASSERT_TRUE(rules.ok());
    FaultProcessor proc(*rules);
    auto recs = proc.feed({"boring line", "another boring line", "systemd[1]: fine"}, 0, false);
    EXPECT_TRUE(recs.empty());
    EXPECT_EQ(proc.stats().lines_scanned, 3u);
}
// ---------------------------------------------------------------------------
// The shipped rule set against a real kernel log (§7.5, §12 fixtures).
// The rules are data, so the thing worth testing is whether they match what
// a device actually prints — not whether the regex engine works.
// ---------------------------------------------------------------------------

namespace
{

/// @brief Read a fixture file from the test fixtures directory.
/// @param name Fixture file name.
/// @return The fixture file contents.
std::string slurp_fixture(const std::string& name)
{
    std::ifstream f(std::string(REPO_ROOT) + "/test/fixtures/" + name);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
/// @brief Split a fixture file into lines for feeding the processor.
/// @param name Fixture file name.
/// @return The fixture file split into lines.
std::vector<std::string> fixture_lines(const std::string& name)
{
    std::vector<std::string> out;
    std::istringstream in(slurp_fixture(name));
    std::string line;
    while (std::getline(in, line))
        if (!line.empty()) out.push_back(line);
    return out;
}

} // namespace

/// @brief The shipped rules fire on a recorded real boot log (fixture-driven).
TEST(FaultRulesFixture, ShippedRulesMatchARealBootLog)
{
    auto rules = load_rules(slurp_fixture("../../catalog/fault-rules.yaml"));
    ASSERT_TRUE(rules.ok()) << rules.error.str();
    FaultProcessor proc(*rules);

    const auto lines = fixture_lines("kernel-log-boot.txt");
    ASSERT_GT(lines.size(), 10u) << "fixture missing or empty";
    auto found = proc.feed(lines, 1'000'000'000ull, /*backfill=*/true);

    std::set<std::string> matched;
    for (const auto& r : found) matched.insert(r.rule);

    // §7.5 names the classes the default set must cover. Each of these has a
    // corresponding line in the fixture; a rule that stops matching real
    // kernel output is a silent loss of coverage on every device.
    for (const char* want : {"storage_error", "thermal_throttle", "ecc_event", "device_timeout", "oom_kill", "watchdog_trip"}) { EXPECT_TRUE(matched.count(want) > 0) << "no fault raised for " << want << " despite a matching line in the boot log"; }
    // Everything raised from the boot ring is marked as backfill, so HQ can
    // tell a boot-window fault from one happening now.
    for (const auto& r : found) EXPECT_TRUE(r.backfill);
}

/// @brief An ordinary clean boot log raises nothing.
TEST(FaultRulesFixture, OrdinaryBootLinesRaiseNothing)
{
    auto rules = load_rules(slurp_fixture("../../catalog/fault-rules.yaml"));
    ASSERT_TRUE(rules.ok());
    FaultProcessor proc(*rules);
    // Healthy boot chatter must stay quiet: a rule set that fires on normal
    // lines is worse than no rule set, because the noise hides real faults.
    auto found = proc.feed({"6,0,0,-;Linux version 5.15.148-tegra (buildd@arches) #1 SMP", "6,42,120000,-;Machine model: NVIDIA Jetson Orin Nano", "6,800,3000000,-;systemd[1]: Reached target Multi-User System.", "6,101,340000,-;mmc0: new HS400 MMC card at address 0001"}, 2'000'000'000ull, false);
    EXPECT_TRUE(found.empty()) << "the default rule set fires on healthy boot output";
}
