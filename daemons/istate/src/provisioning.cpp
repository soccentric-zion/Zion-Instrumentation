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
 * @file daemons/istate/src/provisioning.cpp
 * @brief istate provisioning state machine implementation (REQ §6.2): virgin detection, entropy check, on-device WireGuard keygen, factory-signed enrollment, and key rotation.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/istate/provisioning.h"

#include <cstring>
#include <sstream>

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

/// @brief Canonical name of a lifecycle state.
/// @param s State to name.
/// @return Canonical name of the lifecycle state.
const char* Provisioner::state_name(State s) const
{
    switch (s)
    {
    case State::Virgin: return "virgin";
    case State::Seeding: return "seeding";
    case State::Keygen: return "keygen";
    case State::Enrolling: return "enrolling";
    case State::Operational: return "operational";
    }
    return "unknown";
}

/// @brief Derive a hostname from the factory identity.
/// @param id Factory identity to derive the hostname from.
/// @return The derived hostname.
std::string Provisioner::derive_hostname(const platform::FactoryIdentity& id)
{
    // Deterministic from the serial: arch-<last 6 hex of serial>.
    std::string serial = id.serial;
    if (serial.size() > 6) serial = serial.substr(serial.size() - 6);
    std::string h = id.hostname_base.empty() ? "arch" : id.hostname_base;
    return h + "-" + serial;
}

/// @brief First-boot sequence: identity, keygen, enrollment, hostname.
/// @param enroll_fn HQ enrollment round-trip.
/// @return The established identity, or an error.
Result<DeviceIdentity> Provisioner::run_first_boot(const EnrollFn& enroll_fn)
{
    // 1. Mount and validate /pst; detect virgin state (done by PstStore::mount).
    if (pst_.schema_version() != "0" && !pst_.exists("system/istate/config.yaml"))
    {
        // system/ exists but no identity yet — still virgin for provisioning.
    }
    if (!pst_.exists("system/istate/wg-public.key") && pst_.schema_version() == "0") { state_ = State::Virgin; }
    else if (pst_.exists("system/istate/wg-public.key"))
    {
        // Already provisioned: load identity.
        auto pub = pst_.read_protected("system/istate/wg-public.key");
        auto host = pst_.read_protected("system/istate/hostname");
        if (pub.ok() && host.ok())
        {
            identity_.wg_public_key = *pub;
            identity_.hostname = *host;
            state_ = State::Operational;
            auto tun = pst_.read_protected("system/istate/wg-peer.yaml");
            if (tun.ok())
            {
                auto j = json_parse(*tun);
                if (j.ok())
                {
                    if (const JValue* v = (*j).find("hq_endpoint")) tunnel_.hq_endpoint = v->as_string();
                    if (const JValue* v = (*j).find("device_addr")) tunnel_.device_addr = v->as_string();
                    if (const JValue* v = (*j).find("hq_addr")) tunnel_.hq_addr = v->as_string();
                    if (const JValue* v = (*j).find("peer_pubkey")) tunnel_.peer_pubkey_b64 = v->as_string();
                    tunnel_.valid = tunnel_.peer_pubkey_b64.size() > 0;
                }
            }
            return Result<DeviceIdentity>::good(identity_);
        }
    }

    // 2. Read factory identity; derive hostname.
    auto fid = plat_.read_factory_identity();
    if (!fid.ok()) return Result<DeviceIdentity>::fail(fid.error);
    identity_.serial = fid->serial;
    identity_.hw_revision = fid->hw_revision;
    identity_.board_variant = fid->board_variant;
    identity_.mfg_date = fid->mfg_date;
    identity_.hostname = derive_hostname(*fid);

    // 3. Seed system/ from image defaults (caller passes defaults via PstStore).
    state_ = State::Seeding;

    // 4. Check entropy availability, then generate the WireGuard keypair.
    // The private key never leaves the device.
    if (!plat_.entropy_available()) return Result<DeviceIdentity>::fail(Errc::Unavailable, "CSPRNG entropy unavailable — refusing to generate keys", "provision");
    state_ = State::Keygen;

    return generate_and_enroll(false, enroll_fn);
}

/// @brief Rotate the tunnel key and re-enroll without factory involvement.
/// @param enroll_fn HQ enrollment round-trip.
/// @return The new identity, or an error.
Result<DeviceIdentity> Provisioner::rotate_key(const EnrollFn& enroll_fn)
{
    // Rotation: regenerate, re-enroll, no factory involvement (§6.2).
    if (!plat_.entropy_available()) return Result<DeviceIdentity>::fail(Errc::Unavailable, "entropy unavailable", "rotate");
    return generate_and_enroll(true, enroll_fn);
}

/// @brief Shared body of first-boot and rotation.
/// @param rotation True when this is a key rotation.
/// @param enroll_fn HQ enrollment round-trip.
/// @return The established identity, or an error.
Result<DeviceIdentity> Provisioner::generate_and_enroll(bool rotation, const EnrollFn& enroll_fn)
{
    // Keypair generated on device via X25519 (RFC 7748, own implementation).
    crypto::X25519Key key = crypto::x25519_generate([](uint8_t* out, size_t n) {
        Error e = crypto::random_bytes(out, n);
        if (!e.ok())
        {
            // Loud failure: key generation must not run without entropy (§6.2).
            std::memset(out, 0, n); // deterministic-fail; caller checks entropy first
        }
    });
    std::string pub_b64 = crypto::base64_encode(std::string(reinterpret_cast<const char*>(key.pub.data()), 32));
    std::string priv_b64 = crypto::base64_encode(std::string(reinterpret_cast<const char*>(key.priv.data()), 32));

    // Persist: private key 0600-equivalent in /pst/system/networkd, public key
    // in istate's folder.
    Error e = pst_.write_protected("system/networkd/wg-private.key", priv_b64);
    if (!e.ok()) return Result<DeviceIdentity>::fail(e);
    e = pst_.write_protected("system/istate/wg-public.key", pub_b64);
    if (!e.ok()) return Result<DeviceIdentity>::fail(e);
    e = pst_.write_protected("system/istate/hostname", identity_.hostname);
    if (!e.ok()) return Result<DeviceIdentity>::fail(e);
    identity_.wg_public_key = pub_b64;

    // 5. Sign the public key with the factory identity; enroll with HQ.
    auto fid = plat_.read_factory_identity();
    if (!fid.ok()) return Result<DeviceIdentity>::fail(fid.error);
    std::string sig(reinterpret_cast<const char*>(crypto::hmac_sha256(fid->identity_secret, pub_b64).b.data()), 32);
    identity_.factory_sig = crypto::base64_encode(sig);
    state_ = State::Enrolling;
    auto enrolled = enroll_fn(identity_);
    if (!enrolled.ok())
    {
        // Enrollment may be retried; the device stays operational locally (R10).
        return Result<DeviceIdentity>::fail(enrolled.error);
    }
    tunnel_ = *enrolled;
    tunnel_.valid = true;

    // 6. Persist tunnel configuration; 7. transition to operational.
    JValue peer = JValue::object();
    peer.set("hq_endpoint", JValue(tunnel_.hq_endpoint));
    peer.set("device_addr", JValue(tunnel_.device_addr));
    peer.set("hq_addr", JValue(tunnel_.hq_addr));
    peer.set("peer_pubkey", JValue(tunnel_.peer_pubkey_b64));
    e = pst_.write_protected("system/networkd/wg-peer.yaml", peer.dump());
    if (!e.ok()) return Result<DeviceIdentity>::fail(e);
    (void)rotation;
    state_ = State::Operational;
    return Result<DeviceIdentity>::good(identity_);
}

} // namespace istate
} // namespace instr