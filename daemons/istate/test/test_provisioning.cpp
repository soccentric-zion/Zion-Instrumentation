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
 * @file test/istate/test_provisioning.cpp
 * @brief provisioning state machine tests (REQ §6.2)
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <gtest/gtest.h>

#include "instr/common/filesystem.h"
#include "instr/istate/provisioning.h"
#include "instr/platform/simulated.h"

using namespace instr;
using namespace instr::istate;
using namespace instr::platform;

struct ProvFixture
{
    SimulatedFileSystem fs;
    SimulatedPlatform plat;
    BudgetManager budgets{fs, "/pst"};
    PstStore pst{fs, "/pst", budgets};
    Provisioner prov{pst, plat, fs};
    std::map<std::string, std::string> defaults{{"istate", "a: 1\n"}};
    /// @brief Provisions a temp store, simulated platform and scripted enrollment.
    ProvFixture()
    {
        plat.set_factory_identity([]() {
            platform::FactoryIdentity id;
            id.serial = "SIM-000123";
            id.hw_revision = "rev-a";
            id.board_variant = "sim";
            id.mfg_date = "2025-01-01";
            id.hostname_base = "arch";
            id.identity_secret = "factory-secret-abc";
            id.valid = true;
            return id;
        }());
        pst.mount();
        pst.seed_defaults(defaults);
    }
};

/// @brief First boot: identity, keygen, enrollment, hostname and /pst seed complete in order.
TEST(Provisioning, FirstBootFullSequence)
{
    ProvFixture f;
    int enroll_calls = 0;
    auto id = f.prov.run_first_boot([&](const DeviceIdentity& ident) {
        ++enroll_calls;
        EXPECT_EQ(ident.hostname, "arch-000123"); // derived from factory identity
        EXPECT_NE(ident.pubkey_b64().size(), 0u);
        // Factory signature present: HQ verifies against the manufacturing record.
        EXPECT_NE(ident.factory_sig.size(), 0u);
        TunnelConfig t;
        t.hq_endpoint = "hq.arches.example:51820";
        t.device_addr = "10.7.0.2";
        t.hq_addr = "10.7.0.1";
        t.peer_pubkey_b64 = "hq-pub-key";
        return Result<TunnelConfig>::good(t);
    });
    ASSERT_TRUE(id.ok());
    EXPECT_EQ(enroll_calls, 1);
    EXPECT_EQ(f.prov.state(), Provisioner::State::Operational);
    // Private key persisted on device only — never in the enrollment request.
    auto priv = f.pst.read_protected("system/networkd/wg-private.key");
    ASSERT_TRUE(priv.ok());
    EXPECT_NE(priv->size(), 0u);
    // Tunnel config persisted.
    auto peer = f.pst.read_protected("system/networkd/wg-peer.yaml");
    ASSERT_TRUE(peer.ok());
    EXPECT_NE(peer->find("hq.arches.example"), std::string::npos);
    // Second boot: loads existing identity, no re-enrollment.
    Provisioner again(f.pst, f.plat, f.fs);
    auto id2 = again.run_first_boot([](const DeviceIdentity&) {
        ADD_FAILURE() << "must not re-enroll on a provisioned device";
        return Result<TunnelConfig>::fail(Errc::Internal, "should not be called", "");
    });
    EXPECT_TRUE(id2.ok());
    EXPECT_EQ(again.state(), Provisioner::State::Operational);
    EXPECT_EQ(id2->hostname, "arch-000123");
}

/// @brief Key generation without entropy is refused — never a weak key (§6.2).
TEST(Provisioning, RefusesKeygenWithoutEntropy)
{
    ProvFixture f;
    f.plat.set_entropy_available(false);
    auto r = f.prov.run_first_boot([](const DeviceIdentity&) { return Result<TunnelConfig>::fail(Errc::Unavailable, "no", ""); });
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.error.code, Errc::Unavailable);
    EXPECT_NE(r.error.message.find("entropy"), std::string::npos);
}

/// @brief Key rotation regenerates and re-enrolls without factory involvement.
TEST(Provisioning, KeyRotationWithoutSiteVisit)
{
    ProvFixture f;
    auto first = f.prov.run_first_boot([](const DeviceIdentity&) {
        TunnelConfig t;
        t.valid = true;
        return Result<TunnelConfig>::good(t);
    });
    ASSERT_TRUE(first.ok());
    std::string old_pub = first->wg_public_key;
    int enroll_calls = 0;
    auto rotated = f.prov.rotate_key([&](const DeviceIdentity&) {
        ++enroll_calls;
        TunnelConfig t;
        t.valid = true;
        return Result<TunnelConfig>::good(t);
    });
    ASSERT_TRUE(rotated.ok());
    EXPECT_EQ(enroll_calls, 1); // re-enrolled with HQ
    EXPECT_NE(rotated->wg_public_key, old_pub); // fresh keypair
    EXPECT_EQ(f.prov.state(), Provisioner::State::Operational);
    auto pub_now = f.pst.read_protected("system/istate/wg-public.key");
    ASSERT_TRUE(pub_now.ok());
    EXPECT_EQ(*pub_now, rotated->wg_public_key);
}
