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
 * @file test/istate/test_pst.cpp
 * @brief /pst store tests (REQ §6, §11): virgin detection, seeding,
 *
 * corrupt fallback, budgets, migration, reset semantics.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include "instr/common/filesystem.h"
#include "instr/istate/core.h"
#include "instr/istate/pst.h"
#include "instr/platform/simulated.h"

using namespace instr;
using namespace instr::istate;

struct PstFixture
{
    SimulatedFileSystem fs;
    BudgetManager budgets{fs, "/pst"};
    PstStore pst{fs, "/pst", budgets};
    std::map<std::string, std::string> defaults;
    /// @brief Provisions a temp /pst root on an in-memory filesystem.
    PstFixture()
    {
        defaults["istate"] = "a: 1\n";
        defaults["networkd"] = "mode: dhcp\n";
        defaults["monitord"] = "interval: 5\n";
    }
};

/// @brief A virgin /pst is detected and seeded from image defaults.
TEST(PstStore, MountDetectsVirginAndSeeds)
{
    PstFixture f;
    auto rep = f.pst.mount();
    EXPECT_TRUE(rep.ok);
    EXPECT_TRUE(rep.virgin);
    EXPECT_TRUE(f.pst.seed_defaults(f.defaults).ok());
    auto rep2 = f.pst.mount();
    EXPECT_FALSE(rep2.virgin);
    auto doc = f.pst.load_config("monitord");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ((*doc).get_int("interval"), 5);
}

/// @brief A corrupt file falls back to its redundant copy at mount (§11).
TEST(PstStore, CorruptFileFallsBackToRedundantCopy)
{
    PstFixture f;
    f.pst.mount();
    f.pst.seed_defaults(f.defaults);
    // Rewrite (creates .bak), then corrupt the primary.
    ConfigDocument d;
    d.set("interval", "10", Origin::Local);
    f.pst.save_config("monitord", d);
    f.fs.corrupt_reads_of("/pst/system/monitord/config.yaml", 1);
    auto doc = f.pst.load_config("monitord");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ((*doc).get_int("interval"), 10); // recovered from .bak
}

/// @brief Schema version drives migration of defaults across updates.
TEST(PstStore, SchemaVersionAndMigration)
{
    PstFixture f;
    f.pst.mount();
    f.pst.seed_defaults(f.defaults);
    EXPECT_EQ(f.pst.schema_version(), "1");
    std::map<std::string, std::string> new_release = {{"monitord", "interval: 5\ndisk_warn_pct: 90\n"}};
    auto added = f.pst.migrate(new_release);
    ASSERT_EQ(added.size(), 1u);
    EXPECT_EQ(added[0], "monitord/disk_warn_pct"); // new keys added...
    auto doc = f.pst.load_config("monitord");
    ASSERT_TRUE(doc.ok());
    // ...existing operator values preserved (load-modify-save keeps the keys
    // the release added)...
    auto op = f.pst.load_config("monitord");
    ASSERT_TRUE(op.ok());
    (*op).set("interval", "42", Origin::Local);
    f.pst.save_config("monitord", *op);
    auto added2 = f.pst.migrate(new_release);
    EXPECT_TRUE(added2.empty());
    doc = f.pst.load_config("monitord");
    EXPECT_EQ((*doc).get_int("interval"), 42);
}

/// @brief System reset wipes system/state and keeps factory/backup (§6.1).
TEST(PstStore, SystemResetWipesSystemAndStateKeepsFactoryBackup)
{
    PstFixture f;
    f.pst.mount();
    f.pst.seed_defaults(f.defaults);
    f.fs.write("/pst/factory/identity/identity.yaml", "serial: X\n");
    f.fs.write("/pst/backup/backup-1.abk", "archivedata");
    f.fs.write("/pst/state/spool/spool-1.seg", "spooldata");
    // Operator edit that must not survive the reset.
    ConfigDocument d;
    d.set("interval", "99", Origin::Local);
    f.pst.save_config("monitord", d);

    EXPECT_TRUE(f.pst.system_reset(f.defaults).ok());
    EXPECT_TRUE(f.fs.exists("/pst/factory/identity/identity.yaml"));
    EXPECT_TRUE(f.fs.exists("/pst/backup/backup-1.abk"));
    EXPECT_FALSE(f.fs.exists("/pst/state/spool/spool-1.seg"));
    auto doc = f.pst.load_config("monitord");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ((*doc).get_int("interval"), 5); // reseeded from image defaults
}

/// @brief Factory reset wipes everything.
TEST(PstStore, FactoryResetWipesEverything)
{
    PstFixture f;
    f.pst.mount();
    f.pst.seed_defaults(f.defaults);
    f.fs.write("/pst/factory/identity/identity.yaml", "serial: X\n");
    EXPECT_TRUE(f.pst.factory_reset().ok());
    EXPECT_FALSE(f.fs.exists("/pst/factory/identity/identity.yaml"));
    EXPECT_FALSE(f.fs.exists("/pst/system/.schema-version")); // reseeded on next boot
    EXPECT_FALSE(f.fs.exists("/pst/system/istate"));
}

/// @brief Budgets are enforced with the alarm line before the hard limit.
TEST(BudgetManager, EnforcesWithAlarmBeforeLimit)
{
    PstFixture f;
    f.budgets.set_budget("logs", 1000);
    auto r = f.budgets.reserve("logs", 700);
    EXPECT_TRUE(r.ok());
    f.budgets.record_usage("logs", 700);
    EXPECT_TRUE(f.budgets.over_warning().empty()); // 70% < 80%
    f.budgets.record_usage("logs", 850);
    auto warned = f.budgets.over_warning();
    ASSERT_EQ(warned.size(), 1u);
    EXPECT_EQ(warned[0], "logs"); // alarm raised BEFORE the limit
    EXPECT_TRUE(f.budgets.over_limit().empty());
    f.budgets.record_usage("logs", 1001);
    EXPECT_FALSE(f.budgets.over_limit().empty());
    EXPECT_FALSE(f.budgets.reserve("logs", 100).ok()); // hard enforcement
}

// §6/§7.9: migration must actually run on the first boot after an update.
// PstStore::migrate existed and was correct, but nothing called it from the
// boot path, so a release that added keys left devices running without them.

namespace
{
struct BootFixture
{
    SimulatedFileSystem fs;
    platform::SimulatedPlatform plat;
    ManualTimeSource ts;
    MemorySink log_sink;
    Logger log{log_sink, ts};
    IstateOptions opts;
    /// @brief Provisions a store at the current schema version for boot-migration tests.
    BootFixture()
    {
        opts.pst_root = "/pst";
        opts.run_dir = "/run/instrumentation";
        opts.build_version = "test";
        fs.mkdirs(opts.run_dir);
    }
    BudgetManager budgets{fs, "/pst"};
    PstStore pst{fs, "/pst", budgets};
};
} // namespace

/// @brief First boot after an update migrates and stamps the new schema version.
TEST(PstStore, BootAfterUpdateMigratesAndStampsTheSchemaVersion)
{
    BootFixture f;
    // A device provisioned by the previous release: seeded, operator-edited,
    // and stamped with an older schema version.
    std::map<std::string, std::string> old_defaults{{"monitord", "enabled: true\ninterval: 5\n"}};
    ASSERT_TRUE(f.pst.seed_defaults(old_defaults).ok());
    {
        auto doc = f.pst.load_config("monitord");
        ASSERT_TRUE(doc.ok());
        (*doc).set("interval", "17", Origin::Local); // operator edit
        ASSERT_TRUE(f.pst.save_config("monitord", *doc).ok());
    }
    ASSERT_TRUE(f.pst.set_schema_version("0").ok());

    // The new release adds a key and keeps the old one.
    std::map<std::string, std::string> new_defaults{{"monitord", "enabled: true\ninterval: 5\nhighres_window_s: 120\n"}};

    IstateCore core(f.opts, f.fs, f.plat, f.ts, f.log);
    ASSERT_TRUE(core.boot(new_defaults));

    auto doc = core.pst().load_config("monitord");
    ASSERT_TRUE(doc.ok());
    // The new key arrived...
    EXPECT_EQ((*doc).get_int("highres_window_s"), 120);
    // ...and the operator's edit survived it, which is the whole point.
    EXPECT_EQ((*doc).get_int("interval"), 17);
    EXPECT_EQ((*doc).origin_of("interval"), Origin::Local);
    // The version is stamped, so a second boot does not migrate again.
    EXPECT_EQ(core.pst().schema_version(), std::string(kConfigSchemaVersion));
}

/// @brief Booting at the current schema version never re-migrates.
TEST(PstStore, BootAtTheCurrentSchemaVersionDoesNotRemigrate)
{
    BootFixture f;
    std::map<std::string, std::string> defaults{{"monitord", "enabled: true\ninterval: 5\n"}};
    ASSERT_TRUE(f.pst.seed_defaults(defaults).ok());
    ASSERT_TRUE(f.pst.set_schema_version(kConfigSchemaVersion).ok());
    {
        auto doc = f.pst.load_config("monitord");
        ASSERT_TRUE(doc.ok());
        (*doc).set("interval", "17", Origin::Local);
        ASSERT_TRUE(f.pst.save_config("monitord", *doc).ok());
    }
    IstateCore core(f.opts, f.fs, f.plat, f.ts, f.log);
    ASSERT_TRUE(core.boot(defaults));
    auto doc = core.pst().load_config("monitord");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ((*doc).get_int("interval"), 17);
}
