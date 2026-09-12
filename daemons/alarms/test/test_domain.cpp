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
 * @file daemons/alarms/test/test_domain.cpp
 * @brief alarms domain (REQ §12). Covers the state machine, severity
 *
 * change, escalation, persistence, snapshot-on-fault, catalog merge with
 * operator-edit preservation, and the "alarms are declared, never inferred"
 * rule (R5).
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include "instr/alarms/domain.h"
#include "instr/common/filesystem.h"

using namespace instr;
using namespace instr::alarms;
using namespace instr::alarms::domain;

namespace
{
class CountingTs : public TimeSource
{
  public:
    uint64_t mono = 1'000'000'000;
    uint64_t seq = 0;
    uint64_t mono_ns() override
    {
        return mono;
    }
    int64_t wall_ns() override
    {
        return 1'700'000'000'000'000'000;
    }
    std::string boot_id() override
    {
        return "boot-1";
    }
    uint64_t next_seq() override
    {
        return ++seq;
    }
};

static const char* kCatalog = R"yaml(
entries:
  - id: 0x1_04_1_0007
    name: hw_device_fail
    severity: CRITICAL
    probable_cause: equipment-failure
    specific_problem: quick-check-failed
    description: A board device failed its quick health check.
    escalation: promote-after-1h
    auto_clear: manual
    hq_reportable: true
  - id: 0x3_02_2_0002
    name: netd_tunnel_down
    severity: MAJOR
    probable_cause: connectivity-issue
    specific_problem: wireguard-tunnel-down
    description: The HQ tunnel is down.
    escalation: promote-after-1h
    auto_clear: auto
    hq_reportable: true
)yaml";

struct Fixture
{
    SimulatedFileSystem fs;
    CountingTs ts;
    AlarmManager mgr{fs, "/pst/state/alarms/active.db", ts};
    /// @brief Provisions a temp /pst-like store and a manual clock for alarm tests.
    Fixture()
    {
        auto entries = load_catalog_entries(kCatalog);
        assert(entries.ok());
        mgr.merge_catalog(*entries);
    }
};
} // namespace

/// @brief Raise → active, acknowledge, clear transitions with events emitted at each step.
TEST(AlarmsStateMachine, RaiseActiveAckClear)
{
    Fixture f;
    uint32_t id = 0x30220002u;
    auto ev = f.mgr.raise(id, "networkd-app", "tunnel down");
    ASSERT_TRUE(ev.ok());
    EXPECT_EQ(ev->action, "raise");
    ASSERT_EQ(f.mgr.active().size(), 1u);
    EXPECT_EQ(f.mgr.active()[0].count, 1u);
    EXPECT_FALSE(f.mgr.active()[0].acknowledged);
    EXPECT_EQ(f.mgr.active()[0].probable_cause, "connectivity-issue"); // X.733 from catalog

    auto ack = f.mgr.acknowledge(id);
    ASSERT_TRUE(ack.ok());
    EXPECT_TRUE((*f.mgr.find(id))->acknowledged);

    auto clr = f.mgr.clear(id, "networkd-app");
    ASSERT_TRUE(clr.ok());
    EXPECT_TRUE(f.mgr.active().empty());
}

/// @brief Re-raising an active alarm merges occurrences and refreshes last_seen.
TEST(AlarmsStateMachine, ReRaiseCountsAndUpdatesLastSeen)
{
    Fixture f;
    uint32_t id = 0x30220002u;
    f.mgr.raise(id, "app", "first");
    f.ts.mono += 1'000'000'000ull;
    auto ev = f.mgr.raise(id, "app", "again");
    ASSERT_TRUE(ev.ok());
    EXPECT_EQ(f.mgr.active()[0].count, 2u);
    EXPECT_EQ(ev->record.last_seen.seq, f.mgr.active()[0].first_seen.seq + 1);
}

/// @brief A severity change on an active alarm emits severity_change, not a duplicate raise.
TEST(AlarmsStateMachine, SeverityChangeOnActiveAlarm)
{
    Fixture f;
    uint32_t id = 0x30220002u; // MAJOR in catalog
    f.mgr.raise(id, "app", "v1");
    // Catalog entry changed to CRITICAL (e.g. new release) then re-raise.
    CatalogEntry updated;
    updated.id = id;
    updated.name = "netd_tunnel_down";
    updated.severity = common::SEV_CRITICAL;
    f.mgr.merge_catalog({updated});
    auto ev = f.mgr.raise(id, "app", "v2");
    ASSERT_TRUE(ev.ok());
    EXPECT_EQ(ev->action, "severity_change");
    EXPECT_EQ(f.mgr.active()[0].severity, common::SEV_CRITICAL);
    EXPECT_EQ(f.mgr.active()[0].count, 2u); // still one alarm, not two
}

/// @brief An undecodable alarm id is refused loudly, never guessed.
TEST(AlarmsR5, UnknownIdRefusedLoudly)
{
    Fixture f;
    auto r = f.mgr.raise(0xdeadbeefu, "app", "mystery");
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error.code, Errc::NotFound);
    EXPECT_NE(r.error.message.find("never invented"), std::string::npos);
}

/// @brief Only the owning component may clear its alarm.
TEST(AlarmsR5, ClearRefusesForeignOwner)
{
    Fixture f;
    uint32_t id = 0x30220002u;
    f.mgr.raise(id, "app-a", "x");
    auto r = f.mgr.clear(id, "app-b");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(f.mgr.find(id).ok());
}

/// @brief A CRITICAL alarm requests the bounded diagnostic capture (snapshot-on-fault).
TEST(AlarmsSnapshotOnFault, CriticalRequestsBoundedCapture)
{
    Fixture f;
    uint32_t captured = 0;
    f.mgr.set_snapshot_sink([&](uint32_t id) { captured = id; });
    std::vector<AlarmStateEvent> events;
    f.mgr.set_event_sink([&](const AlarmStateEvent& ev) { events.push_back(ev); });
    f.mgr.raise(0x10410007u, "hardware", "device failed"); // CRITICAL in catalog
    EXPECT_EQ(captured, 0x10410007u);
    bool saw_snapshot_request = false;
    for (const auto& ev : events)
        if (ev.action == "snapshot_request") saw_snapshot_request = true;
    EXPECT_TRUE(saw_snapshot_request);
}

/// @brief Unacknowledged alarms promote to the configured severity after the threshold.
TEST(AlarmsEscalation, PromotesUnacknowledgedAfterThreshold)
{
    Fixture f;
    f.mgr.set_escalation({{0, 60, common::SEV_CRITICAL}}); // default policy
    f.mgr.raise(0x30220002u, "app", "x"); // MAJOR
    // Before threshold: no change.
    auto none = f.mgr.escalation_sweep(f.ts.mono + 10ull * 1'000'000'000ull);
    EXPECT_TRUE(none.empty());
    // Past threshold: promoted to CRITICAL.
    f.ts.mono += 60ull * 1'000'000'000ull;
    auto promoted = f.mgr.escalation_sweep(f.ts.mono);
    ASSERT_EQ(promoted.size(), 1u);
    EXPECT_EQ(promoted[0].action, "severity_change");
    EXPECT_EQ(f.mgr.active()[0].severity, common::SEV_CRITICAL);
}

/// @brief Acknowledged alarms are exempt from escalation.
TEST(AlarmsEscalation, AcknowledgedAlarmsDoNotEscalate)
{
    Fixture f;
    f.mgr.set_escalation({{0, 60, common::SEV_CRITICAL}});
    f.mgr.raise(0x30220002u, "app", "x");
    f.mgr.acknowledge(0x30220002u);
    f.ts.mono += 120ull * 1'000'000'000ull;
    EXPECT_TRUE(f.mgr.escalation_sweep(f.ts.mono).empty());
}

/// @brief The active table persists to active.db and survives restart.
TEST(AlarmsPersistence, ActiveTableSurvivesRestart)
{
    SimulatedFileSystem fs;
    CountingTs ts;
    {
        AlarmManager mgr(fs, "/pst/state/alarms/active.db", ts);
        auto entries = load_catalog_entries(kCatalog);
        mgr.merge_catalog(*entries);
        mgr.raise(0x30220002u, "app", "persist me");
        EXPECT_TRUE(mgr.save().ok());
    }
    {
        AlarmManager mgr2(fs, "/pst/state/alarms/active.db", ts);
        ASSERT_TRUE(mgr2.load().ok());
        ASSERT_EQ(mgr2.active().size(), 1u);
        EXPECT_EQ(mgr2.active()[0].owner, "app");
        EXPECT_EQ(mgr2.active()[0].count, 1u);
        EXPECT_EQ(mgr2.active()[0].probable_cause, "connectivity-issue");
    }
}

/// @brief Catalog regeneration preserves operator-edited severities (§7.6).
TEST(AlarmsCatalogMerge, PreservesOperatorEdits)
{
    Fixture f;
    // Operator edits the shipped severity.
    for (auto& c : f.mgr.mutable_catalog())
        if (c.id == 0x30220002u)
        {
            c.severity = common::SEV_CRITICAL;
            c.operator_edited = true;
        }
    // Regeneration with the shipped (MAJOR) entry.
    auto shipped = load_catalog_entries(kCatalog);
    auto st = f.mgr.merge_catalog(*shipped);
    EXPECT_EQ(st.preserved_operator_edits, 1u);
    for (const auto& c : f.mgr.catalog())
    {
        // Braces around the EXPECT too: the macro expands to an if/else, which
        // otherwise dangles against the if above it.
        if (c.id == 0x30220002u) { EXPECT_EQ(c.severity, common::SEV_CRITICAL); }
    }
    // A new entry gets added.
    EXPECT_EQ(st.added, 0u);
    CatalogEntry fresh;
    fresh.id = 0x20803001u;
    fresh.name = "tlm_registration_conflict";
    st = f.mgr.merge_catalog({fresh});
    EXPECT_EQ(st.added, 1u);
}

/// @brief X.733 correlated notifications ride along with the event.
TEST(AlarmsCorrelation, X733CorrelatedNotificationsCarried)
{
    Fixture f;
    auto ev = f.mgr.raise(0x30220002u, "app", "x", {0x50510001u});
    ASSERT_TRUE(ev.ok());
    ASSERT_EQ(f.mgr.active()[0].correlated.size(), 1u);
    EXPECT_EQ(f.mgr.active()[0].correlated[0], 0x50510001u); // kernel fault correlated
}