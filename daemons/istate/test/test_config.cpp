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
 * @file test/istate/test_config.cpp
 * @brief config service tests (REQ §9, §10): origin rules, signed HQ
 *
 * pushes, audit trail, drift report.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include "instr/common/filesystem.h"
#include <openssl/evp.h>

#include "instr/common/signature.h"
#include "instr/istate/config_service.h"

using namespace instr;
using namespace instr::istate;

struct CfgFixture
{
    SimulatedFileSystem fs;
    BudgetManager budgets{fs, "/pst"};
    PstStore pst{fs, "/pst", budgets};
    ConfigService svc{pst, fs};
    std::map<std::string, std::string> defaults{{"networkd", "mode: dhcp\ninterval: 5\n"}};
    /// @brief Provision a temp /pst with seeded defaults for config tests.
    CfgFixture()
    {
        pst.mount();
        pst.seed_defaults(defaults);
        svc.set_schema("networkd", *load_schema_text(R"({"type":"object","properties":{
      "mode":{"type":"string","enum":["dhcp","static"]},
      "interval":{"type":"number"}}})"));
    }
};

/// @brief A freshly seeded /pst reads back the shipped defaults.
TEST(ConfigService, GetReturnsSeededDefaults)
{
    CfgFixture f;
    auto doc = f.svc.get("networkd");
    ASSERT_TRUE(doc.ok());
    EXPECT_EQ((*doc).get_string("mode"), "dhcp");
    EXPECT_EQ((*doc).origin_of("mode"), Origin::Default);
}

/// @brief A set against an unknown key is a loud schema rejection.
TEST(ConfigService, SetValidatesAgainstSchemaLoudly)
{
    CfgFixture f;
    auto r = f.svc.set("networkd", "mode: pppoe\n", Origin::Local, false);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("mode"), std::string::npos);
    // Valid set works.
    auto r2 = f.svc.set("networkd", "mode: static\n", Origin::Local, false);
    EXPECT_TRUE(r2.ok);
    auto doc = f.svc.get("networkd");
    EXPECT_EQ((*doc).get_string("mode"), "static");
}

/// @brief Origin rules: an HQ push cannot overwrite a local edit without force.
TEST(ConfigService, HqPushCannotOverwriteLocalWithoutForce)
{
    CfgFixture f;
    EXPECT_TRUE(f.svc.set("networkd", "mode: static\ninterval: 9\n", Origin::Local, false).ok);
    // HQ push (unsigned path used only for the merge rules here): both keys
    // are operator-edited → both protected.
    auto r = f.svc.set("networkd", "mode: dhcp\ninterval: 7\n", Origin::Hq, false);
    EXPECT_TRUE(r.ok);
    ASSERT_EQ(r.rejected.size(), 2u); // mode + interval preserved
    auto doc = f.svc.get("networkd");
    EXPECT_EQ((*doc).get_string("mode"), "static");
    EXPECT_EQ((*doc).get_int("interval"), 9);
}

// The legacy shared-secret path, kept for devices enrolled before signing
// keys were distributed (see ConfigService::apply_signed).
/// @brief A signed push is verified before it can change anything (§9).
TEST(ConfigService, SignedPushVerifiedBeforeApply)
{
    CfgFixture f;
    std::string secret = "device-enrollment-secret";
    std::string yaml = "mode: dhcp\ninterval: 3\n";
    auto digest = crypto::hmac_sha256(secret, "networkd|" + yaml);
    std::string hmac = crypto::hex_encode(std::string(reinterpret_cast<const char*>(digest.b.data()), 32));
    // Tampered signature → refused, nothing written.
    auto bad = f.svc.apply_signed("networkd", yaml, "key-1", "", "00" + hmac.substr(2), secret);
    EXPECT_FALSE(bad.ok());
    EXPECT_EQ(bad.error.code, Errc::SignatureInvalid);
    auto doc = f.svc.get("networkd");
    EXPECT_EQ((*doc).get_int("interval"), 5); // untouched
    // Correct signature → applied.
    auto good = f.svc.apply_signed("networkd", yaml, "key-1", "", hmac, secret);
    ASSERT_TRUE(good.ok());
    doc = f.svc.get("networkd");
    EXPECT_EQ((*doc).get_int("interval"), 3);
    EXPECT_EQ((*doc).origin_of("interval"), Origin::Hq);
}

/// @brief The drift report shows the effective config with per-key origins.
TEST(ConfigService, DriftReportShowsEffectiveConfigAndOrigins)
{
    CfgFixture f;
    f.svc.set("networkd", "interval: 9\n", Origin::Local, false);
    auto j = json_parse(f.svc.drift_report_json("networkd"));
    ASSERT_TRUE(j.ok());
    EXPECT_EQ((*j).find("effective")->find("interval")->as_number(), 9);
    EXPECT_EQ((*j).find("origins")->find("interval")->as_string(), "local");
    EXPECT_EQ((*j).find("origins")->find("mode")->as_string(), "default");
}

/// @brief The audit log records actor, source, action, time and detail.
TEST(AuditLog, RecordsWhoChangedWhatWhenFromWhere)
{
    CfgFixture f;
    ManualTimeSource ts;
    AuditLog audit(f.fs, "/pst/state/audit.log", 100);
    audit.append("alice", "configuration-ui", "config_set", "{\"daemon\":\"networkd\"}", ts);
    audit.append("hq", "hq-tunnel", "update", "{\"to\":\"2.0.0\"}", ts);
    EXPECT_TRUE(audit.persist().ok());
    AuditLog loaded(f.fs, "/pst/state/audit.log", 100);
    ASSERT_TRUE(loaded.load().ok());
    ASSERT_EQ(loaded.entries().size(), 2u);
    EXPECT_EQ(loaded.entries()[0].actor, "alice");
    EXPECT_EQ(loaded.entries()[0].source, "configuration-ui");
    EXPECT_EQ(loaded.entries()[1].action, "update");
}

/// @brief The audit log is a bounded ring; oldest entries drop first.
TEST(AuditLog, BoundedRing)
{
    CfgFixture f;
    ManualTimeSource ts;
    AuditLog audit(f.fs, "/pst/state/audit2.log", 10);
    for (int i = 0; i < 100; ++i) audit.append("actor", "src", "action", "{\"i\":" + std::to_string(i) + "}", ts);
    EXPECT_EQ(audit.entries().size(), 10u);
    EXPECT_EQ(audit.entries().back().detail_json, "{\"i\":99}");
}

// ---------------------------------------------------------------------------
// §9: "config pushes from HQ are signed and verified on device, so tunnel
// compromise alone does not let an attacker reconfigure a fleet." That
// property needs an asymmetric signature — with a shared secret the device
// holds a fleet-signing key, and opening one device is enough.
// ---------------------------------------------------------------------------

namespace
{

// A signing keypair standing in for HQ's. Only the public half is given to
// the device, which is the entire point.
struct HqSigner
{
    std::string public_key; // 32 raw bytes
    EVP_PKEY* key = nullptr;

    /// @brief Generates an Ed25519 keypair for signed-push tests.
    HqSigner()
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        EVP_PKEY_keygen_init(ctx);
        EVP_PKEY_keygen(ctx, &key);
        EVP_PKEY_CTX_free(ctx);
        size_t len = 32;
        unsigned char raw[32];
        EVP_PKEY_get_raw_public_key(key, raw, &len);
        public_key.assign(reinterpret_cast<char*>(raw), len);
    }
    /// @brief Releases the signing key context.
    /// @return Releases the signing key context.
    ~HqSigner()
    {
        EVP_PKEY_free(key);
    }

    /// @brief Sign a message with the test signing key.
    /// @param message Exact bytes to sign.
    /// @return The 64-byte signature.
    std::string sign(const std::string& message) const
    {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key);
        size_t siglen = 0;
        EVP_DigestSign(ctx, nullptr, &siglen, reinterpret_cast<const unsigned char*>(message.data()), message.size());
        std::string sig(siglen, '\0');
        EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(&sig[0]), &siglen, reinterpret_cast<const unsigned char*>(message.data()), message.size());
        sig.resize(siglen);
        EVP_MD_CTX_free(ctx);
        return sig;
    }
};

} // namespace

/// @brief A valid Ed25519 push applies; tampering with it is refused.
TEST(ConfigServiceSigning, Ed25519SignedPushIsAppliedAndTamperingIsRefused)
{
    CfgFixture f;
    HqSigner hq;
    f.svc.add_signing_key("hq-2026", hq.public_key);

    const std::string yaml = "mode: static\ninterval: 11\n";
    const std::string sig = hq.sign("networkd|" + yaml);

    // A good signature applies, and the keys are recorded as HQ-origin.
    auto ok = f.svc.apply_signed("networkd", yaml, "hq-2026", sig, "", "");
    ASSERT_TRUE(ok.ok()) << ok.error.str();
    auto doc = f.svc.get("networkd");
    EXPECT_EQ((*doc).get_int("interval"), 11);
    EXPECT_EQ((*doc).origin_of("interval"), Origin::Hq);
    EXPECT_FALSE(f.svc.last_push_used_legacy_hmac());

    // One flipped byte in the payload invalidates the signature.
    std::string tampered = yaml;
    tampered.back() = '\n';
    tampered = "mode: static\ninterval: 99\n";
    auto bad = f.svc.apply_signed("networkd", tampered, "hq-2026", sig, "", "");
    EXPECT_FALSE(bad.ok());
    EXPECT_EQ(bad.error.code, Errc::SignatureInvalid);
    EXPECT_EQ((*f.svc.get("networkd")).get_int("interval"), 11) << "a refused push must not write";
}

/// @brief Once signing keys exist, the legacy HMAC path is refused.
TEST(ConfigServiceSigning, ADeviceWithKeysRefusesTheLegacyHmacForm)
{
    // Once a signing key is provisioned, the shared-secret path is closed —
    // otherwise an attacker holding the enrollment secret could simply choose
    // the weaker form and the asymmetric key would be decoration.
    CfgFixture f;
    HqSigner hq;
    f.svc.add_signing_key("hq-2026", hq.public_key);

    const std::string secret = "device-enrollment-secret";
    const std::string yaml = "interval: 7\n";
    auto digest = crypto::hmac_sha256(secret, "networkd|" + yaml);
    const std::string hmac = crypto::hex_encode(std::string(reinterpret_cast<const char*>(digest.b.data()), 32));

    auto r = f.svc.apply_signed("networkd", yaml, "hq-2026", "", hmac, secret);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error.code, Errc::SignatureInvalid);
}

/// @brief A signature from an unknown key id is refused.
TEST(ConfigServiceSigning, AnUnknownKeyIdIsRefused)
{
    CfgFixture f;
    HqSigner hq;
    f.svc.add_signing_key("hq-2026", hq.public_key);
    const std::string yaml = "interval: 7\n";
    auto r = f.svc.apply_signed("networkd", yaml, "hq-2019", hq.sign("networkd|" + yaml), "", "");
    EXPECT_FALSE(r.ok());
    EXPECT_NE(r.error.message.find("unknown key id"), std::string::npos);
}

/// @brief Two key ids trusted at once let signing keys rotate without a flag day.
TEST(ConfigServiceSigning, TwoKeysAreTrustedAtOnceSoRotationNeedsNoFlagDay)
{
    // §6.2/§9: rotating the signing key must not require every device to be
    // updated in the same instant as HQ switches over.
    CfgFixture f;
    HqSigner old_key, new_key;
    f.svc.add_signing_key("hq-2025", old_key.public_key);
    f.svc.add_signing_key("hq-2026", new_key.public_key);

    const std::string a = "interval: 3\n";
    EXPECT_TRUE(f.svc.apply_signed("networkd", a, "hq-2025", old_key.sign("networkd|" + a), "", "").ok());
    const std::string b = "interval: 4\n";
    EXPECT_TRUE(f.svc.apply_signed("networkd", b, "hq-2026", new_key.sign("networkd|" + b), "", "").ok());

    // Retiring the old key stops accepting anything signed with it.
    f.svc.remove_signing_key("hq-2025");
    const std::string c = "interval: 5\n";
    EXPECT_FALSE(f.svc.apply_signed("networkd", c, "hq-2025", old_key.sign("networkd|" + c), "", "").ok());
}

/// @brief Verification is available and fails closed on malformed input.
TEST(SignatureBackend, VerificationIsAvailableAndFailsClosedOnBadInput)
{
    EXPECT_TRUE(crypto::signature_verification_available()) << "built without a signature backend: signed content would be refused";
    // Wrong-sized inputs are refused rather than passed to the backend.
    EXPECT_FALSE(crypto::verify_ed25519("short", "msg", std::string(64, 'x')).ok());
    EXPECT_FALSE(crypto::verify_ed25519(std::string(32, 'k'), "msg", "short").ok());
}
