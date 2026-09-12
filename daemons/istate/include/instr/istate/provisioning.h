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
 * @file daemons/istate/include/instr/istate/provisioning.h
 * @brief provisioning state machine (REQ §6.2)
 *
 * Virgin detection → default generation → entropy check → WireGuard keypair
 * generated on device (private key never leaves) → factory-signed enrollment
 * → HQ returns tunnel configuration → operational. Key rotation without a
 * site visit: regenerate, re-enroll, no factory involvement.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <functional>
#include <string>
#include <vector>

#include "instr/common/crypto.h"
#include "instr/common/filesystem.h"
#include "instr/istate/pst.h"
#include "instr/platform/platform.h"

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

/// @brief Device identity established at first boot.
struct DeviceIdentity
{
    std::string hostname;          ///< Derived from factory identity.
    std::string serial;            ///< Factory serial.
    std::string hw_revision;       ///< Board revision.
    std::string board_variant;     ///< Board variant.
    std::string mfg_date;          ///< Manufacture date.
    std::string wg_public_key;     ///< WireGuard public key (base64).
    std::string factory_sig;       ///< HMAC(factory secret, pubkey) — base64.
    /// @brief The public key in base64 (identity accessor).
    /// @return The public key in base64.
    std::string pubkey_b64() const
    {
        return wg_public_key;
    }
};

/// @brief WireGuard tunnel configuration returned by HQ enrollment.
struct TunnelConfig
{
    std::string hq_endpoint;         ///< HQ tunnel endpoint.
    std::string device_addr;         ///< Device tunnel address.
    std::string hq_addr;             ///< HQ tunnel address.
    std::string peer_pubkey_b64;     ///< HQ's public key.
    bool valid = false;              ///< False until enrollment succeeds.
};

/// @brief First-boot provisioning state machine: key generation and HQ enrollment.
class Provisioner
{
  public:
    /// @brief Provisioning lifecycle states.
    enum class State
    {
        Virgin,        ///< No system/ yet.
        Seeding,       ///< Defaults written.
        Keygen,        ///< Generating tunnel keypair.
        Enrolling,     ///< Enrolling with HQ.
        Operational,   ///< Fully provisioned.
    };
    /// @brief Canonical name of a state.
    /// @param s State to name.
    /// @return Canonical name of the lifecycle state.
    const char* state_name(State s) const;

    /// @brief Bind the provisioner to the store, platform and filesystem.
    /// @param pst /pst store.
    /// @param plat Platform (factory identity + entropy).
    /// @param fs Storage backend.
    Provisioner(PstStore& pst, platform::IPlatform& plat, IFileSystem& fs) : pst_(pst), plat_(plat), fs_(fs)
    {
    }

    /// @brief Current lifecycle state.
    State state() const
    {
        return state_;
    }
    /// @brief Established identity (after first boot).
    /// @return The established identity (after first boot).
    const DeviceIdentity& identity() const
    {
        return identity_;
    }
    /// @brief Tunnel configuration from enrollment.
    /// @return The tunnel configuration from enrollment.
    const TunnelConfig& tunnel() const
    {
        return tunnel_;
    }

    /// @brief First-boot sequence (§6.2).
    ///
    /// @p enroll_fn performs the HQ round-trip: given the signed enrollment
    /// request, it returns the tunnel configuration.
    using EnrollFn = std::function<Result<TunnelConfig>(const DeviceIdentity&)>;
    /// @brief Run first boot: identity, keygen, enrollment, hostname, /pst seed.
    /// @param enroll_fn HQ enrollment round-trip.
    /// @return The established identity, or an error.
    Result<DeviceIdentity> run_first_boot(const EnrollFn& enroll_fn);

    /// @brief Key rotation without a site visit: regenerate, re-enroll, no factory involvement.
    /// @param enroll_fn HQ enrollment round-trip.
    /// @return The new identity, or an error.
    Result<DeviceIdentity> rotate_key(const EnrollFn& enroll_fn);

    /// @brief Hostname derivation from factory identity.
    /// @param id Factory identity to derive the hostname from.
    /// @return The derived hostname.
    static std::string derive_hostname(const platform::FactoryIdentity& id);

  private:
    /// @brief Shared body of first-boot and rotation.
    /// @param rotation True when this is a key rotation.
    /// @param enroll_fn HQ enrollment round-trip.
    /// @return The established identity, or an error.
    Result<DeviceIdentity> generate_and_enroll(bool rotation, const EnrollFn& enroll_fn);

    PstStore& pst_;                ///< /pst store (not owned).
    platform::IPlatform& plat_;     ///< Factory identity + entropy (not owned).
    IFileSystem& fs_;               ///< Storage backend.
    State state_ = State::Virgin;  ///< Lifecycle state.
    DeviceIdentity identity_;      ///< Established identity.
    TunnelConfig tunnel_;          ///< Enrollment result.
};

} // namespace istate
} // namespace instr