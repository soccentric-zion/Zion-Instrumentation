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
 * @file daemons/istate/include/instr/istate/config_service.h
 * @brief configuration service (REQ §9, §10). Every key carries an
 *
 * origin; HQ pushes are signed and verified on device (tunnel compromise
 * alone must not reconfigure a fleet); audit trail records who changed
 * what, when, from where; drift report exposes effective config.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "instr/common/config.h"
#include "instr/common/crypto.h"
#include "instr/common/filesystem.h"
#include "instr/istate/pst.h"
#include "instr/common/timestamps.h"

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

/// @brief One auditable action, appended to the tamper-evident log.
struct AuditEntry
{
    uint64_t seq = 0;             ///< Monotonic entry sequence.
    int64_t wall_unix_s = 0;      ///< Wall time of the action.
    std::string actor;            ///< Who (local operator / hq / app).
    std::string source;           ///< From where (configuration-ui / hq-tunnel / local-console).
    std::string action;           ///< config_set / update / remote_access / reset.
    std::string detail_json;      ///< Action detail.
};

/// @brief Bounded, persisted audit log of configuration-changing actions.
class AuditLog
{
  public:
    /// @brief Bind the log to storage, a path and a size bound.
    /// @param fs Storage backend.
    /// @param path Audit file path.
    /// @param max_entries Entry bound (oldest dropped).
    AuditLog(IFileSystem& fs, const std::string& path, size_t max_entries) : fs_(fs), path_(path), max_entries_(max_entries)
    {
    }
    /// @brief Append one entry (stamped from @p ts).
    /// @param actor Who performed the action.
    /// @param source Where it came from.
    /// @param action Action kind.
    /// @param detail_json Action detail.
    /// @param ts Clock for stamping.
    void append(const std::string& actor, const std::string& source, const std::string& action, const std::string& detail_json, TimeSource& ts);
    /// @brief In-memory entries, oldest first.
    /// @return In-memory entries, oldest first.
    const std::deque<AuditEntry>& entries() const
    {
        return entries_;
    }
    /// @brief Persist the log atomically.
    /// @return ok(), or the write error.
    Error persist();
    /// @brief Load the log (tolerates a missing file).
    /// @return ok(), or the load error.
    Error load();

  private:
    IFileSystem& fs_;                 ///< Storage backend.
    std::string path_;                ///< Audit file path.
    size_t max_entries_;              ///< Entry bound (oldest dropped).
    std::deque<AuditEntry> entries_;   ///< Retained entries.
    uint64_t next_seq_ = 1;           ///< Next entry sequence.
};

/// @brief Configuration service: get/set by origin rules, signed HQ pushes, drift reporting.
class ConfigService
{
  public:
    /// @brief Bind the service to the /pst store and a filesystem.
    /// @param pst /pst store.
    /// @param fs Storage backend.
    ConfigService(PstStore& pst, IFileSystem& fs) : pst_(pst), fs_(fs)
    {
    }

    /// @brief Register a per-daemon JSON schema (validated at load; rejected with a clear file-and-line error).
    /// @param daemon Daemon the schema describes.
    /// @param schema JSON Schema for its config.
    void set_schema(const std::string& daemon, const JValue& schema)
    {
        schemas_[daemon] = schema;
    }

    /// @brief Get the effective config of a daemon.
    /// @param daemon Daemon to read.
    /// @return The effective config, or an error.
    Result<ConfigDocument> get(const std::string& daemon) const;
    /// @brief Drift report: actual effective config so HQ can compare against intent.
    /// @param daemon Daemon to report on.
    /// @return The drift report as JSON.
    std::string drift_report_json(const std::string& daemon) const;

    /// @brief Outcome of a config set.
    struct SetResult
    {
        std::vector<std::string> rejected;   ///< Protected paths not overwritten.
        std::string error;                  ///< Empty on success.
        bool ok = false;                    ///< Overall success.
    };
    /// @brief Set: merges incoming values by origin rules.
    ///
    /// @param daemon Target daemon.
    /// @param yaml_text Incoming YAML.
    /// @param origin Claimed origin of the values.
    /// @param force Overwrite protected local keys when true.
    /// @return Rejected paths and error text.
    SetResult set(const std::string& daemon, const std::string& yaml_text, Origin origin, bool force);

    /// @brief HQ's Ed25519 config-signing public keys, by key id (§9).
    ///
    /// The device holds only public keys: opening one device yields nothing
    /// that can sign for the fleet, which is the property §9 is after and
    /// that an HMAC — where the verifier is also a signer — cannot provide.
    /// Several ids can be trusted at once so a key can be rotated without a
    /// flag day: publish the new key, switch signing, retire the old one.
    /// @param key_id Key id to trust.
    /// @param public_key_raw32 Raw 32-byte public key.
    void add_signing_key(const std::string& key_id, const std::string& public_key_raw32)
    {
        signing_keys_[key_id] = public_key_raw32;
    }
    /// @brief Retire a signing key.
    /// @param key_id Key id to retire.
    void remove_signing_key(const std::string& key_id)
    {
        signing_keys_.erase(key_id);
    }
    /// @brief Trusted signing keys (key id → raw public key).
    /// @return Trusted signing keys (key id → raw public key).
    const std::map<std::string, std::string>& signing_keys() const
    {
        return signing_keys_;
    }

    /// @brief Signed config push from HQ (§9): verified before applying, so tunnel compromise alone cannot reconfigure a fleet.
    ///
    /// @p signature is Ed25519 over "<daemon>|<yaml>" and is the required path.
    /// @p hmac_hex is the legacy shared-secret form: accepted only when no
    /// signing key is provisioned at all, and reported by
    /// last_push_used_legacy_hmac().
    ///
    /// @return Rejected paths, or an error when verification fails.
    /// @param daemon Target daemon.
    /// @param yaml_text Incoming YAML.
    /// @param key_id Signing key id.
    /// @param signature Ed25519 signature over "<daemon>|<yaml>".
    /// @param hmac_hex Legacy HMAC (accepted only with no signing keys).
    /// @param enrollment_secret Secret for the legacy HMAC path.
    Result<std::vector<std::string>> apply_signed(const std::string& daemon, const std::string& yaml_text, const std::string& key_id, const std::string& signature, const std::string& hmac_hex, const std::string& enrollment_secret);
    /// @brief True when the last push fell back to the legacy HMAC path.
    bool last_push_used_legacy_hmac() const
    {
        return last_push_legacy_;
    }

    /// @brief Registered schemas per daemon.
    /// @return Registered schemas per daemon.
    const std::map<std::string, JValue>& schemas() const
    {
        return schemas_;
    }

  private:
    PstStore& pst_;                       ///< /pst store (not owned).
    IFileSystem& fs_;                      ///< Storage backend.
    std::map<std::string, JValue> schemas_;          ///< Per-daemon schemas.
    std::map<std::string, std::string> signing_keys_;   ///< Trusted signing keys.
    bool last_push_legacy_ = false;       ///< Latched when the legacy HMAC path is used.
};

} // namespace istate
} // namespace instr