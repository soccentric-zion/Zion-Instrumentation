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
 * @file test/unit/test_default_configs.cpp
 * @brief Every shipped default validates against its daemon's JSON Schema.
 *
 * REQ §9: every format has a JSON Schema in schemas/, validated at load and
 * rejected with a clear error. The schemas are only real if the set the
 * image ships actually satisfies them — a default that cannot load would
 * leave a device unconfigured on first boot. This is the CI gate for that.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include "instr/common/config.h"
#include "instr/common/json.h"

using namespace instr;

namespace
{

const char* kDaemons[] = {"istate", "networkd", "monitord", "hardware", "faults", "alarms", "logmanager", "telemetry", "ota", "backup"};

/// @brief Read a shipped schema/default file.
/// @param path File to read.
/// @return The file contents.
std::string slurp(const std::string& path)
{
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
/// @brief Path of a daemon's shipped JSON Schema.
/// @param d Daemon name.
/// @return The daemon's schema path.
std::string schema_path(const std::string& d)
{
    return std::string(REPO_ROOT) + "/schemas/" + d + ".schema.json";
}
/// @brief Path of a daemon's shipped default YAML.
/// @param d Daemon name.
/// @return The daemon's defaults path.
std::string default_path(const std::string& d)
{
    return std::string(REPO_ROOT) + "/system/defaults/" + d + "/config.yaml";
}

} // namespace

/// @brief Every daemon ships a JSON Schema for its config (§9).
TEST(DefaultConfigs, EveryDaemonShipsASchema)
{
    for (const char* d : kDaemons)
    {
        const std::string text = slurp(schema_path(d));
        ASSERT_FALSE(text.empty()) << "missing schemas/" << d << ".schema.json (REQ §9)";
        auto schema = load_schema_text(text);
        ASSERT_TRUE(schema.ok()) << d << ": " << schema.error.str();
    }
}

/// @brief Every daemon ships default YAML for first boot.
TEST(DefaultConfigs, EveryDaemonShipsDefaults)
{
    for (const char* d : kDaemons) { ASSERT_FALSE(slurp(default_path(d)).empty()) << "missing system/defaults/" << d << "/config.yaml (REQ §9)"; }
}

/// @brief Shipped defaults must satisfy their own schema — a packaging typo cannot reach a device.
TEST(DefaultConfigs, ShippedDefaultsValidateAgainstTheirSchema)
{
    for (const char* d : kDaemons)
    {
        auto schema = load_schema_text(slurp(schema_path(d)));
        ASSERT_TRUE(schema.ok()) << d;
        auto doc = ConfigDocument::parse(slurp(default_path(d)), *schema, std::string("system/defaults/") + d + "/config.yaml");
        EXPECT_TRUE(doc.ok()) << d << " default config rejected: " << doc.error.str();
    }
}

/// @brief An unknown key is a loud schema rejection, never a silent behavioural change.
TEST(DefaultConfigs, SchemaRejectsAnUnknownKeyLoudly)
{
    // A typo must be a loud error, never a silent behavioural change (§9).
    auto schema = load_schema_text(slurp(schema_path("monitord")));
    ASSERT_TRUE(schema.ok());
    auto doc = ConfigDocument::parse("enabled: true\nintervl: 5\n", *schema, "typo.yaml");
    ASSERT_FALSE(doc.ok());
    EXPECT_NE(doc.error.str().find("intervl"), std::string::npos) << "the rejection must name the offending key: " << doc.error.str();
}

/// @brief Out-of-range values are rejected at load.
TEST(DefaultConfigs, SchemaRejectsAnOutOfRangeValue)
{
    auto schema = load_schema_text(slurp(schema_path("monitord")));
    ASSERT_TRUE(schema.ok());
    auto doc = ConfigDocument::parse("enabled: true\ninterval: 0\n", *schema, "range.yaml");
    EXPECT_FALSE(doc.ok()) << "interval below the schema minimum must be refused";
}

/// @brief The per-class interval map is constrained to the known resource classes.
TEST(DefaultConfigs, PerClassIntervalMapIsConstrained)
{
    // additionalProperties-as-schema: an open map whose values are still typed.
    auto schema = load_schema_text(slurp(schema_path("monitord")));
    ASSERT_TRUE(schema.ok());
    auto ok = ConfigDocument::parse("enabled: true\nintervals_per_class:\n  cpu: 2.5\n", *schema, "map-ok.yaml");
    EXPECT_TRUE(ok.ok()) << ok.error.str();
    auto bad = ConfigDocument::parse("enabled: true\nintervals_per_class:\n  cpu: nonsense\n", *schema, "map-bad.yaml");
    EXPECT_FALSE(bad.ok()) << "a non-numeric interval must be refused";
}
