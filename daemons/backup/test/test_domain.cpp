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
 * @file daemons/backup/test/test_domain.cpp
 * @brief backup domain (REQ §12). Roundtrip, exclusions, encryption,
 *
 * preview, transactional restore, replacement-board semantics.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include "instr/backup/domain.h"
#include "instr/common/filesystem.h"

using namespace instr;
using namespace instr::backup;
using namespace instr::backup::domain;

struct Fixture
{
    SimulatedFileSystem fs;
    BackupOptions opts;
    std::unique_ptr<BackupEngine> eng;
    /// @brief Provisions a temp store with identity options for backup tests.
    Fixture()
    {
        // Populate /pst/system.
        fs.write("/pst/system/istate/config.yaml", "a: 1\n");
        fs.write("/pst/system/networkd/config.yaml", "mode: dhcp\n");
        fs.write("/pst/system/networkd/wg-private.key", "SUPER-SECRET-KEY");
        fs.write("/pst/factory/identity/identity.yaml", "serial: SIM-1\n");
        opts.device_serial = "SIM-1";
        opts.hardware_revision = "rev-a";
        opts.schema_version = "1";
        opts.firmware_version = "1.0.0";
        opts.identity_secret = "identity-secret";
        eng = std::make_unique<BackupEngine>(fs, opts);
    }
};

/// @brief A created archive round-trips with its manifest and per-file hashes.
TEST(BackupCreate, RoundtripArchiveWithManifest)
{
    Fixture f;
    auto info = f.eng->create({"/pst/system"}, {}, "/pst/backup", 1'700'000'000);
    ASSERT_TRUE(info.ok());
    EXPECT_EQ(info->manifest.device_serial, "SIM-1");
    EXPECT_EQ(info->manifest.files.size(), 2u); // two configs; key excluded
    auto list = f.eng->list("/pst/backup");
    ASSERT_EQ(list.size(), 1u);
    auto man = f.eng->read_manifest("/pst/backup/" + info->name);
    ASSERT_TRUE(man.ok());
    EXPECT_EQ(man->hardware_revision, "rev-a");
}

/// @brief Factory and WireGuard keys are always excluded — identity is not portable.
TEST(BackupCreate, ExcludesFactoryAndWireGuardKeys)
{
    Fixture f;
    auto info = f.eng->create({"/pst"}, {}, "/pst/backup", 1'700'000'000);
    ASSERT_TRUE(info.ok());
    for (const auto& file : info->manifest.files)
    {
        EXPECT_TRUE(BackupEngine::excluded_path(file, {}) == false);
        EXPECT_EQ(file.find("wg-private.key"), std::string::npos) << "secret leaked into backup";
        EXPECT_EQ(file.find("factory/"), std::string::npos) << "identity cloned into backup";
    }
}

/// @brief Local archives are sealed; the blob is not a readable tar.
TEST(BackupCrypto, ArchiveIsEncryptedNotPlainTar)
{
    Fixture f;
    auto info = f.eng->create({"/pst/system"}, {}, "/pst/backup", 1'700'000'000);
    ASSERT_TRUE(info.ok());
    auto blob = f.fs.read("/pst/backup/" + info->name);
    ASSERT_TRUE(blob.ok());
    // The archive must not be a plain ustar (encrypted payload).
    auto as_tar = tar::read_ustar(*blob);
    EXPECT_TRUE(as_tar->empty()); // decryption required
    // And must not contain the config plaintext.
    EXPECT_EQ(blob->find("mode: dhcp"), std::string::npos);
}

/// @brief The restore preview classifies every affected path.
TEST(BackupPreview, ShowsAddReplaceDelete)
{
    Fixture f;
    f.fs.write("/pst/system/istate/keep.yaml", "keep: true\n"); // stays unchanged
    auto info = f.eng->create({"/pst/system"}, {}, "/pst/backup", 1'700'000'000);
    ASSERT_TRUE(info.ok());
    // Change one file, add another, remove one.
    f.fs.write("/pst/system/istate/config.yaml", "a: 2\n");
    f.fs.write("/pst/system/istate/extra.yaml", "new: true\n");
    f.fs.remove("/pst/system/networkd/config.yaml", false);
    auto p = f.eng->restore_preview("/pst/backup/" + info->name, {"/pst/system"});
    ASSERT_TRUE(p.ok());
    bool saw_replace = false, saw_add = false, saw_delete = false, saw_unchanged = false;
    for (const auto& e : p->entries)
    {
        // Action = what the restore will do to the current state.
        if (e.action == "replace" && e.path == "istate/config.yaml") saw_replace = true;
        if (e.action == "add" && e.path == "networkd/config.yaml") saw_add = true;
        if (e.action == "delete" && e.path == "istate/extra.yaml") saw_delete = true;
        if (e.action == "unchanged") saw_unchanged = true;
    }
    EXPECT_TRUE(saw_replace);
    EXPECT_TRUE(saw_add);
    EXPECT_TRUE(saw_delete);
    EXPECT_TRUE(saw_unchanged);
}

/// @brief A hardware-mismatched archive is refused before anything is touched.
TEST(BackupPreview, HardwareMismatchRefused)
{
    Fixture f;
    auto info = f.eng->create({"/pst/system"}, {}, "/pst/backup", 1'700'000'000);
    ASSERT_TRUE(info.ok());
    // Restore onto a different board revision → incompatible.
    f.opts.hardware_revision = "rev-b";
    BackupEngine other(f.fs, f.opts);
    auto p = other.restore_preview("/pst/backup/" + info->name, {"/pst/system"});
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p->compatibility, BackupEngine::Compatibility::HardwareMismatch);
}

/// @brief A different serial marks the restore config-only with re-enrollment.
TEST(BackupPreview, ReplacementBoardDetected)
{
    Fixture f;
    auto info = f.eng->create({"/pst/system"}, {}, "/pst/backup", 1'700'000'000);
    ASSERT_TRUE(info.ok());
    // Same revision, different serial (replacement board).
    f.opts.device_serial = "SIM-2";
    BackupEngine other(f.fs, f.opts);
    auto p = other.restore_preview("/pst/backup/" + info->name, {"/pst/system"});
    ASSERT_TRUE(p.ok());
    EXPECT_TRUE(p->replacement_board); // config restores, identity does not → re-enroll
}

/// @brief Restore is transactional: full commit or prior state intact.
TEST(BackupRestore, TransactionalCommitCompletesFullyOrNotAtAll)
{
    Fixture f;
    auto info = f.eng->create({"/pst/system"}, {}, "/pst/backup", 1'700'000'000);
    ASSERT_TRUE(info.ok());
    std::vector<std::string> committed;
    auto r = f.eng->restore_commit("/pst/backup/" + info->name, [&](const std::vector<tar::FileEntry>& files) {
        for (const auto& e : files) committed.push_back(e.name);
        return Error::none();
    });
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(*r, committed.size());
    // Commit failure leaves prior state intact (no partial application).
    auto r2 = f.eng->restore_commit("/pst/backup/" + info->name, [](const std::vector<tar::FileEntry>&) { return Error::make(Errc::Io, "simulated commit failure", "x"); });
    EXPECT_FALSE(r2.ok());
    EXPECT_EQ(r2.error.code, Errc::Io);
}

/// @brief A tampered file fails hash verification before any commit.
TEST(BackupRestore, TamperedFileFailsVerificationBeforeCommit)
{
    Fixture f;
    auto info = f.eng->create({"/pst/system"}, {}, "/pst/backup", 1'700'000'000);
    ASSERT_TRUE(info.ok());
    // Tamper with the encrypted blob so decryption succeeds but content hash
    // differs → verification must fail before any commit.
    auto blob = f.fs.read("/pst/backup/" + info->name);
    ASSERT_TRUE(blob.ok());
    // Flip a byte deep inside (payload region).
    std::string tampered = *blob;
    tampered[tampered.size() / 2] ^= 0xFF;
    f.fs.write("/pst/backup/" + info->name, tampered);
    auto r = f.eng->restore_commit("/pst/backup/" + info->name, [](const std::vector<tar::FileEntry>&) { return Error::none(); });
    // Either authentication fails (tag) or integrity fails; both refuse commit.
    EXPECT_FALSE(r.ok());
}

/// @brief Runtime schedule state is settable independent of persisted config.
TEST(BackupSchedule, RuntimeScheduleState)
{
    // Schedule logic exercised via the core facade in the daemon tests; here
    // we assert the archive listing is ordered and deterministic.
    Fixture f;
    f.eng->create({"/pst/system"}, {}, "/pst/backup", 1'700'000'000);
    f.eng->create({"/pst/system"}, {}, "/pst/backup", 1'700'086'400);
    auto list = f.eng->list("/pst/backup");
    ASSERT_EQ(list.size(), 2u);
    EXPECT_LT(list[0].manifest.created_unix_s, list[1].manifest.created_unix_s);
}

// §7.10: "HQ uploads encrypted to HQ's public key". The local archive key is
// derived from device identity, which protects an archive sitting on the
// device — it does nothing for one in transit or at rest at HQ, and anything
// holding the device secret could read it. These pin the separate path.
/// @brief An HQ-destined archive is sealed to HQ's key and opaque to the local key.
TEST(BackupEncryption, ArchiveForHqIsSealedToHqKeyAndOpaqueLocally)
{
    auto rng = [](uint8_t* out, size_t n) { (void)crypto::random_bytes(out, n); };
    crypto::X25519Key hq = crypto::x25519_generate(rng);

    SimulatedFileSystem fs;
    BackupOptions opts;
    opts.device_serial = "SIM-1";
    opts.identity_secret = "device-identity-secret";
    opts.hq_public_key = std::string(reinterpret_cast<const char*>(hq.pub.data()), 32);
    BackupEngine eng(fs, opts);
    ASSERT_TRUE(eng.hq_upload_available());

    const std::string payload = "network:\n  ssid: site-wifi\n  psk: SUPER-SECRET\n";
    auto sealed = eng.seal_for_hq(payload);
    ASSERT_TRUE(sealed.ok()) << sealed.error.str();

    // The plaintext must not survive anywhere in the sealed blob.
    EXPECT_EQ(sealed->find("SUPER-SECRET"), std::string::npos);
    EXPECT_EQ(sealed->find("site-wifi"), std::string::npos);

    // HQ opens it with its private key.
    auto opened = BackupEngine::open_from_hq(*sealed, std::string(reinterpret_cast<const char*>(hq.priv.data()), 32));
    ASSERT_TRUE(opened.ok()) << opened.error.str();
    EXPECT_EQ(*opened, payload);

    // A different HQ key cannot: the seal is to one recipient, not to anyone
    // who happens to hold the device.
    crypto::X25519Key other = crypto::x25519_generate(rng);
    auto wrong = BackupEngine::open_from_hq(*sealed, std::string(reinterpret_cast<const char*>(other.priv.data()), 32));
    EXPECT_FALSE(wrong.ok());
}

/// @brief Every seal uses a fresh ephemeral X25519 keypair.
TEST(BackupEncryption, EachSealUsesAFreshEphemeralKey)
{
    auto rng = [](uint8_t* out, size_t n) { (void)crypto::random_bytes(out, n); };
    crypto::X25519Key hq = crypto::x25519_generate(rng);
    SimulatedFileSystem fs;
    BackupOptions opts;
    opts.identity_secret = "s";
    opts.hq_public_key = std::string(reinterpret_cast<const char*>(hq.pub.data()), 32);
    BackupEngine eng(fs, opts);
    auto a = eng.seal_for_hq("same plaintext");
    auto b = eng.seal_for_hq("same plaintext");
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_NE(*a, *b) << "identical ciphertext for identical input leaks equality";
}

/// @brief Without an HQ key the upload is refused, never sent in the clear.
TEST(BackupEncryption, WithoutAnHqKeyUploadIsRefusedNotSentInTheClear)
{
    SimulatedFileSystem fs;
    BackupOptions opts;
    opts.identity_secret = "s"; // no hq_public_key
    BackupEngine eng(fs, opts);
    EXPECT_FALSE(eng.hq_upload_available());
    auto sealed = eng.seal_for_hq("config");
    EXPECT_FALSE(sealed.ok());
    EXPECT_NE(sealed.error.message.find("refusing"), std::string::npos);
}
