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
 * @file test/secret-leak/secret_leak_test.cpp
 * @brief Secret-leak test (REQ §10/§12): credentials never appear in logs,
 *
 * unencrypted backups, or telemetry. Enforced with a test, not a convention.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include "instr/backup/domain.h"
#include "instr/common/filesystem.h"
#include "instr/common/log.h"
#include "telemetry.pb.h"
#include "instr/telemetry/domain.h"

using namespace instr;
using namespace instr::backup::domain;
using namespace instr::telemetry::domain;
using namespace instr::backup;
using namespace instr::telemetry;
namespace telemetry_proto = instr::telemetry; // proto-generated namespace

/// @brief No credential reaches a log sink — fields and inline values redacted (REQ §10, enforced not conventional).
TEST(SecretLeak, LoggerRedactsSecretFieldsAndInlineValues)
{
    MemorySink sink;
    ManualTimeSource ts;
    Logger log(sink, ts);
    log.error("istate", "auth failed password=hunter2 token=eyJhbGc secret=zzz", {{"wg-private.key", "KPRIV"}, {"serial", "SIM-1"}});
    std::string all;
    for (const auto& r : sink.records())
    {
        all += r.message + " ";
        for (const auto& kv : r.fields) all += kv.first + "=" + kv.second + " ";
    }
    EXPECT_EQ(all.find("hunter2"), std::string::npos);
    EXPECT_EQ(all.find("KPRIV"), std::string::npos);
    EXPECT_EQ(all.find("zzz"), std::string::npos);
    EXPECT_NE(all.find("SIM-1"), std::string::npos); // non-secrets pass
}

/// @brief Backup archives exclude key material and are sealed before leaving the device.
TEST(SecretLeak, BackupArchivesExcludeKeysAndEncrypt)
{
    SimulatedFileSystem fs;
    BackupOptions opts;
    opts.device_serial = "SIM-1";
    opts.hardware_revision = "rev-a";
    opts.schema_version = "1";
    opts.firmware_version = "1.0.0";
    opts.identity_secret = "id-secret";
    BackupEngine eng(fs, opts);
    fs.write("/pst/system/networkd/config.yaml", "mode: dhcp\n");
    fs.write("/pst/system/networkd/wg-private.key", "WG-SECRET-KEY-DO-NOT-LEAK");
    fs.write("/pst/factory/identity/.secret", "FACTORY-SECRET");
    auto info = eng.create({"/pst"}, {}, "/pst/backup", 1'700'000'000);
    ASSERT_TRUE(info.ok());
    auto blob = fs.read("/pst/backup/" + info->name);
    ASSERT_TRUE(blob.ok());
    // No secret material in the archive — encrypted AND excluded by policy.
    EXPECT_EQ(blob->find("WG-SECRET-KEY-DO-NOT-LEAK"), std::string::npos);
    EXPECT_EQ(blob->find("FACTORY-SECRET"), std::string::npos);
    EXPECT_EQ(blob->find("mode: dhcp"), std::string::npos); // ciphertext, not plain
    // Decryption recovers exactly the portable config.
    auto man = eng.read_manifest("/pst/backup/" + info->name);
    ASSERT_TRUE(man.ok());
    for (const auto& f : man->files)
    {
        EXPECT_EQ(f.find("wg-private"), std::string::npos);
        EXPECT_EQ(f.find("factory/"), std::string::npos);
    }
}

/// @brief Telemetry batches carry no credential-shaped data.
TEST(SecretLeak, TelemetryRecordsCarryNoCredentials)
{
    // The telemetry surface ships only metric values + reproducibility
    // metadata; there is no credential-bearing field in the record model.
    BatchShaper shaper(10, 1.0);
    shaper.set_repro({"1.0.0", "1", "rev-a", "SIM-1"});
    shaper.ingest("cpu.util", 42.0, "percent", 100, {{"device", "mmcblk0"}});
    auto b = shaper.flush(2000);
    ASSERT_TRUE(b.has_value());
    for (const auto& r : b->records)
    {
        std::string s = r.metric;
        EXPECT_EQ(s.find("secret"), std::string::npos);
        EXPECT_EQ(s.find("password"), std::string::npos);
        EXPECT_EQ(s.find("token"), std::string::npos);
    }
    // The proto record model has no credential fields (compile-time guarantee
    // exercised here): telemetry.proto TelemetryRecord fields are metric,
    // value, unit, agg_fn, window bounds, tags, ts, count.
    telemetry_proto::TelemetryRecord proto_rec; // proto type
    proto_rec.set_metric("cpu.util");
    std::string probe;
    ASSERT_TRUE(proto_rec.AppendToString(&probe));
    EXPECT_EQ(probe.find("password"), std::string::npos);
}

/// @brief The audit trail records actions without leaking secrets in details.
TEST(SecretLeak, AuditTrailRedactsDetailSecrets)
{
    MemorySink sink;
    ManualTimeSource ts;
    Logger log(sink, ts);
    // Audit entries record who/what/when/where — never credentials.
    std::string detail = "{\"actor\":\"hq\",\"wg-private.key\":\"LEAKME\"}";
    log.info("audit", "config change", {{"detail", detail}});
    std::string all;
    for (const auto& r : sink.records())
        for (const auto& kv : r.fields) all += kv.second;
    EXPECT_EQ(all.find("LEAKME"), std::string::npos);
}
