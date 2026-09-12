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
 * @file test/unit/test_catalog_codegen.cpp
 * @brief alarm catalog code generator (REQ §8)
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include "instr/catalog/codegen.h"

using namespace instr::catalog;

static const char* kFixture = R"yaml(
version: 3
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
  - id: 0x5_05_3_0008
    name: flt_oom_kill
    severity: MAJOR
    probable_cause: memory-exhaustion
    specific_problem: oom-killer-active
    description: Kernel OOM kill detected.
    escalation: promote-after-15m
    auto_clear: manual
    hq_reportable: true
)yaml";

/// @brief The catalog fixture parses into entries with severities and policies.
TEST(CatalogCodegen, ParsesFixture)
{
    auto cat = parse_catalog(kFixture);
    ASSERT_TRUE(cat.ok());
    EXPECT_EQ(cat->version, 3u);
    ASSERT_EQ(cat->entries.size(), 2u);
    EXPECT_EQ(cat->entries[0].id, 0x10410007u);
    EXPECT_EQ(cat->entries[0].severity, Severity::Critical);
    EXPECT_TRUE(cat->entries[0].hq_reportable);
    EXPECT_EQ(cat->entries[1].id, 0x50530008u); // module 05, type 3, index 0008
}

/// @brief render_header emits INSTR_ALARM_* constants for every entry.
TEST(CatalogCodegen, RendersHeaderWithConstants)
{
    auto cat = parse_catalog(kFixture);
    ASSERT_TRUE(cat.ok());
    auto hdr = render_header(*cat, "9.9.9");
    ASSERT_TRUE(hdr.ok());
    EXPECT_NE(hdr->find("constexpr uint32_t INSTR_ALARM_HW_DEVICE_FAIL = 0x10410007u;"), std::string::npos);
    EXPECT_NE(hdr->find("INSTR_ALARM_CATALOG_SIZE"), std::string::npos);
    EXPECT_NE(hdr->find("AlarmSeverity::CRITICAL"), std::string::npos);
    EXPECT_NE(hdr->find("Generated from catalog/alarms.yaml"), std::string::npos);
    EXPECT_NE(hdr->find("9.9.9"), std::string::npos);
}

/// @brief A duplicate alarm id is a loud generation error, never a silent overwrite.
TEST(CatalogCodegen, DuplicateIdRejected)
{
    const char* dup = R"yaml(
entries:
  - id: 0x1_04_1_0001
    name: a
    severity: INFO
  - id: 0x1_04_1_0001
    name: b
    severity: INFO
)yaml";
    auto r = parse_catalog(dup);
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.error.message.find("duplicate alarm id"), std::string::npos);
}

/// @brief Malformed id text is rejected with validated field widths.
TEST(CatalogCodegen, MalformedIdRejected)
{
    auto r = parse_alarm_id_text("0x12_345");
    EXPECT_FALSE(r.ok());
    auto ok = parse_alarm_id_text("0x1_04_1_0007");
    ASSERT_TRUE(ok.ok());
    EXPECT_EQ(*ok, 0x10410007u);
}
