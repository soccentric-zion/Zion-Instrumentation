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
 * @file daemons/telemetry/test/test_domain.cpp
 * @brief telemetry domain (REQ §12)
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include "instr/common/timestamps.h"
#include "instr/telemetry/domain.h"

using namespace instr;
using namespace instr::telemetry;
using namespace instr::telemetry::domain;

/// @brief Registration accepts new metrics and rejects duplicates loudly (§7.8).
TEST(TelemetryRegistry, RegistersAndRejectsDuplicates)
{
    MetricRegistry r;
    EXPECT_EQ(r.register_metric("cpu.util", "percent", "gauge", "monitord"), MetricRegistry::Reg::Ok);
    EXPECT_EQ(r.register_metric("cpu.util", "percent", "gauge", "other-app"),
              MetricRegistry::Reg::Duplicate); // alarm-visible, never silent overwrite
    EXPECT_EQ(r.register_metric("", "percent", "gauge", "app"), MetricRegistry::Reg::Invalid);
    EXPECT_EQ(r.register_metric("x", "", "histogram", "app"), MetricRegistry::Reg::Invalid);
}

/// @brief Aggregation computes min/max/mean/count/last per window.
TEST(TelemetryAggregator, FunctionsMinMaxMeanCountLast)
{
    Aggregator agg;
    uint64_t t = 1000;
    agg.add("cpu.util", 10, t);
    agg.add("cpu.util", 90, t + 1);
    agg.add("cpu.util", 50, t + 2);
    auto wins = agg.close(t + 3);
    std::map<std::string, double> vals;
    for (const auto& w : wins) vals[w.fn] = w.value;
    EXPECT_NEAR(vals["min"], 10, 1e-9);
    EXPECT_NEAR(vals["max"], 90, 1e-9);
    EXPECT_NEAR(vals["mean"], 50, 1e-9);
    EXPECT_NEAR(vals["count"], 3, 1e-9);
    EXPECT_NEAR(vals["last"], 50, 1e-9);
}

/// @brief Batches shape at the record bound.
TEST(TelemetryBatchShaper, ShapesBatchesAtRecordLimit)
{
    BatchShaper shaper(5, 30.0);
    uint64_t t = 1'000'000'000;
    std::optional<MetricBatch> b;
    for (int i = 0; i < 5; ++i)
    {
        auto maybe = shaper.ingest("m", double(i), "", t + i, {});
        if (maybe) b = maybe;
    }
    ASSERT_TRUE(b.has_value());
    EXPECT_GE(b->records.size(), 1u);
    EXPECT_EQ(shaper.pending_samples(), 0u);
}

/// @brief Batches flush when the interval elapses.
TEST(TelemetryBatchShaper, FlushOnInterval)
{
    BatchShaper shaper(1000, 30.0);
    uint64_t t = 1'000'000'000;
    EXPECT_FALSE(shaper.ingest("m", 1, "", t, {}).has_value());
    auto b = shaper.flush(t + uint64_t(31 * 1e9));
    ASSERT_TRUE(b.has_value());
    EXPECT_FALSE(shaper.flush(t + uint64_t(32 * 1e9)).has_value());
}

/// @brief Unsent retention drops oldest telemetry first; protected batches stay.
TEST(TelemetryRetention, DropsOldestTelemetryFirst)
{
    RetentionBuffer buf(3);
    MetricBatch b;
    b.records.push_back(MetricRecord{});
    for (int i = 0; i < 5; ++i) buf.push(b, RetentionBuffer::Class::Telemetry);
    EXPECT_EQ(buf.dropped(), 2u); // oldest-first drop
    // Protected batches are NEVER dropped (§7.8/§11): they may exceed the
    // bound temporarily — alarms and faults must not be shed.
    MetricBatch prot;
    for (int i = 0; i < 4; ++i) buf.push(prot, RetentionBuffer::Class::Protected);
    // Each protected push sheds one more old telemetry record; once only
    // protected remain the buffer may exceed the bound (never drop them).
    EXPECT_EQ(buf.dropped(), 5u); // telemetry drops only (2 + 3)
    EXPECT_EQ(buf.size(), 4u); // 4 protected retained, telemetry shed
}

/// @brief The health score penalises monotonically with factors exposed.
TEST(TelemetryHealthScore, MonotonicPenaltiesWithFactors)
{
    HealthInputs base;
    auto clean = compute_health_score(base);
    EXPECT_DOUBLE_EQ(clean.score, 100.0);
    EXPECT_TRUE(clean.factors.count("cpu"));

    HealthInputs bad = base;
    bad.cpu_util_pct = 100;
    bad.mem_used_pct = 100;
    bad.max_thermal_c = 95;
    bad.alarms_critical = 2;
    auto worse = compute_health_score(bad);
    EXPECT_LT(worse.score, 60.0);
    EXPECT_GT(worse.factors["alarms"], 40.0);

    EXPECT_DOUBLE_EQ(compute_health_score(bad).score, worse.score); // deterministic
}

/// @brief The health score floors at zero.
TEST(TelemetryHealthScore, FloorAtZero)
{
    HealthInputs extreme;
    extreme.alarms_critical = 100;
    EXPECT_DOUBLE_EQ(compute_health_score(extreme).score, 0.0);
}

/// @brief Reproducibility metadata rides on every batch (§9).
TEST(TelemetryRepro, MetadataOnEveryRecord)
{
    BatchShaper shaper(2, 30.0);
    shaper.set_repro({"9.9.9", "3", "rev-c", "SIM-1"});
    shaper.ingest("m", 1, "", 1000, {});
    auto b = shaper.flush(2000);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->repro.firmware_version, "9.9.9");
    EXPECT_EQ(b->repro.config_schema_version, "3");
    EXPECT_EQ(b->repro.hardware_revision, "rev-c");
}
