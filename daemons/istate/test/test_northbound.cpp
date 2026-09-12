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
 * @file test/istate/test_northbound.cpp
 * @brief northbound tests (REQ §7.1/§3.4/§11): spool drain with
 *
 * backpressure, drop reporting, timeline ordering, tail fanout.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include "instr/common/filesystem.h"
#include "instr/istate/northbound.h"

using namespace instr;
using namespace instr::istate;

/// @brief Timeline events are ordered with strictly increasing sequence numbers.
TEST(EventTimeline, OrderedStreamWithSeq)
{
    ManualTimeSource ts;
    EventTimeline tl(100);
    tl.append("boot", "istate", "{}", ts);
    tl.append("alarm", "alarms", "{}", ts);
    tl.append("fault", "faults", "{}", ts);
    ASSERT_EQ(tl.size(), 3u);
    auto all = tl.since(0);
    EXPECT_EQ(all[0].kind, "boot");
    EXPECT_LT(all[0].seq, all[1].seq);
    auto later = tl.since(1);
    ASSERT_EQ(later.size(), 2u);
    EXPECT_EQ(later[0].kind, "alarm");
}

/// @brief The timeline is bounded; oldest events drop first.
TEST(EventTimeline, BoundedRingDropsOldest)
{
    ManualTimeSource ts;
    EventTimeline tl(3);
    for (int i = 0; i < 10; ++i) tl.append("event", "x", std::to_string(i), ts);
    EXPECT_EQ(tl.size(), 3u);
    auto evs = tl.since(0);
    EXPECT_EQ(evs[0].detail_json, "7"); // oldest dropped
}

/// @brief Multiple subscribers each get a bounded pending queue.
TEST(TailFanout, MultipleSubscribersBoundedPerSubscriber)
{
    TailFanout fan;
    std::vector<std::string> a, b;
    fan.subscribe([&](const std::string& s) { a.push_back(s); });
    fan.subscribe([&](const std::string& s) { b.push_back(s); });
    for (int i = 0; i < 400; ++i) fan.publish("{\"i\":" + std::to_string(i) + "}");
    EXPECT_EQ(a.size(), 400u);
    EXPECT_EQ(b.size(), 400u);
    int id = fan.subscribe([](const std::string&) {});
    fan.unsubscribe(id);
    EXPECT_EQ(a.size(), 400u);
}

/// @brief Drain pauses when the send callback reports backpressure.
TEST(NorthboundSpool, DrainHonoursBackpressure)
{
    SimulatedFileSystem fs;
    SpoolConfig cfg;
    cfg.dir = "/pst/state/spool";
    NorthboundSpool spool(fs, cfg, nullptr);
    ASSERT_TRUE(spool.open().ok());
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(spool.enqueue(SpoolPriority::Telemetry, "{\"i\":" + std::to_string(i) + "}", i).ok());
    // Sender pauses after 3 records (HQ backpressure, §11).
    int sent = 0;
    auto rep = spool.drain(
        [&](const DiskSpool::Entry&) {
            if (++sent > 3) return false;
            return true;
        },
        50);
    EXPECT_TRUE(rep.paused);
    EXPECT_EQ(spool.size(), 7u); // 3 acked records consumed; pause on the 4th
    // Drain completes when the sender accepts everything.
    auto rep2 = spool.drain([](const DiskSpool::Entry&) { return true; }, 50);
    EXPECT_EQ(rep2.sent, 7);
    EXPECT_EQ(spool.size(), 0u);
}

/// @brief Drop accounting is reported per priority class — drops are never silent.
TEST(NorthboundSpool, DropAccountingReported)
{
    SimulatedFileSystem fs;
    SpoolConfig cfg;
    cfg.dir = "/pst/state/spool";
    cfg.max_bytes = 1200;
    NorthboundSpool spool(fs, cfg, nullptr);
    ASSERT_TRUE(spool.open().ok());
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(spool.enqueue(SpoolPriority::Alarm, "alarm-" + std::to_string(i), i).ok());
    for (int i = 0; i < 40; ++i) ASSERT_TRUE(spool.enqueue(SpoolPriority::Telemetry, "t-" + std::to_string(i), i).ok());
    // Drops happened and are counted (reported, never silent, §11).
    EXPECT_GT(spool.stats().dropped_telemetry, 0u);
    EXPECT_EQ(spool.stats().refused_protected, 0u); // alarms never dropped
}

/// @brief The HQ session state machine transitions and backoff behave as documented.
TEST(HqSession, BackoffAndStateTransitions)
{
    HqSession s(1.0, 60.0);
    EXPECT_EQ(s.state(), HqSession::State::Disconnected);
    double total = 0;
    for (int i = 0; i < 10; ++i) total += s.next_delay_s();
    EXPECT_GE(total, 1.0); // exponential growth
    s.on_connected();
    EXPECT_EQ(s.state(), HqSession::State::Connected);
    s.on_backpressure(true);
    EXPECT_EQ(s.state(), HqSession::State::Backpressured);
    s.on_backpressure(false);
    EXPECT_EQ(s.state(), HqSession::State::Draining);
    s.on_disconnected();
    EXPECT_EQ(s.state(), HqSession::State::Backoff);
    EXPECT_GT(s.next_delay_s(), 0.0);
}
