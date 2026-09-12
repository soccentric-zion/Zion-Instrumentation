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
 * @file daemons/ota/include/instr/ota/domain.h
 * @brief thin manager on top of SWUpdate (REQ §7.9). Artifacts land on disk
 *
 * before applying; the interface is shaped so true streaming can be added
 * later without breaking callers. Signature verification before staging;
 * integrity verification; download resume; stage to the inactive slot,
 * apply, set boot slot; explicit post-boot confirmation with automatic
 * rollback; anti-rollback; /home and /pst preserved; progress as a stream.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "instr/common/backoff.h"
#include "instr/common/crypto.h"
#include "instr/common/error.h"
#include "instr/common/filesystem.h"
#include "instr/common/rate_limiter.h"
#include "instr/common/timestamps.h"
#include "instr/platform/platform.h"

namespace instr
{
/// @brief OTA daemon: signed update pipeline with rollback (§7.9, §11).
namespace ota
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief Manifest describing an OTA artifact and how it may be applied.
struct ArtifactManifest
{
    std::string version;        ///< Release version.
    std::string sha256;         ///< Artifact integrity digest.
    // Release signature: Ed25519 over "<version>|<sha256>" under the release
    // signing key, which the image carries as a public key only. This is the
    // §7.9 "signature verification before staging" path.
    std::string signature;      ///< Ed25519 signature, 64 raw bytes.
    std::string key_id;         ///< Which release key signed it.
    ///< Legacy shared-secret form, accepted only where no release key is provisioned.
    std::string hmac;           ///< Legacy HMAC over the manifest.
    uint64_t size_bytes = 0;    ///< Artifact size.
    bool preserve_home = true;  ///< /home preserved unless requested otherwise.
    bool preserve_pst = true;   ///< /pst preserved unless requested otherwise.
};

/// @brief OTA policy delivered by istate.
struct OtaConfig
{
    std::string min_version;               ///< Anti-rollback floor.
    double confirm_timeout_s = 600;        ///< Unconfirmed update rolls back automatically.
    /// @brief One maintenance window where updates are permitted.
    struct Window
    {
        std::string day;     ///< "*" or weekday abbreviations "mon,sun".
        std::string start;   ///< "02:00".
        std::string end;     ///< "04:00".
    };
    std::vector<Window> windows;                 ///< Per-device update schedule.
    uint64_t bandwidth_limit_bytes_per_s = 0;   ///< 0 = unlimited.
};

/// @brief State-machine state of an update.
struct UpdateState
{
    std::string state = "idle";      ///< idle / download / verified / staged / applied_pending_confirm / confirmed / failed / rolled_back.
    std::string from_version;        ///< Version updated from.
    std::string to_version;          ///< Version updating to.
    double progress = 0;             ///< Download/apply progress 0..1.
    std::string staged_slot;         ///< Boot slot the update was applied to.
    uint64_t confirm_deadline_mono = 0;   ///< Trial deadline; past it, rollback.
    std::string error;               ///< Last failure reason.
};

/// @brief One finished update in the history.
struct HistoryEntry
{
    std::string from_version;       ///< Version updated from.
    std::string to_version;         ///< Version updated to.
    std::string result;             ///< confirmed / rolled_back / failed.
    int64_t finished_unix_s = 0;    ///< When it finished.
};

/// @brief Progress event emitted during download/verify/stage/apply.
struct OtaProgressEvent
{
    std::string stage;             ///< Stage name.
    double progress = 0;           ///< 0..1 progress within the stage.
    std::string message;           ///< Human-readable detail.
    std::string from_version;      ///< Version updated from.
    std::string to_version;        ///< Version updating to.
};

/// @brief Version comparison: dotted numeric with v-prefix tolerance ("1.2.10" > "1.2.9").
/// @return Negative / zero / positive, or an error for malformed versions.
/// @param a First version.
/// @param b Second version.
Result<int> compare_versions(const std::string& a, const std::string& b);

/// @brief Update engine: artifact intake, verification, staging, apply, confirm/rollback.
class OtaEngine
{
  public:
    /// @brief Bind the engine to storage, platform, a clock and its policy.
    /// @param fs Storage backend.
    /// @param plat Boot-slot operations.
    /// @param ts Clock.
    /// @param cfg Initial policy.
    OtaEngine(IFileSystem& fs, platform::IPlatform& plat, TimeSource& ts, OtaConfig cfg) : fs_(fs), plat_(plat), ts_(ts), cfg_(cfg)
    {
    }

    /// @brief Update the policy while the engine is live (§9).
    /// @param c New policy.
    void set_config(const OtaConfig& c)
    {
        cfg_ = c;
    }
    const OtaConfig& config() const
    {
        return cfg_;
    }
    /// @brief SWUpdate is the install engine (§7.9); the path is configurable so a
    /// test can point at a stub and CI never shells out to the real thing.
    /// @param p Install engine binary path.
    void set_swupdate_binary(std::string p)
    {
        swupdate_binary_ = std::move(p);
    }
    const std::string& swupdate_binary() const
    {
        return swupdate_binary_;
    }
    /// @brief Seam for tests and for the simulated platform: runs the installer and returns its exit status.
    ///
    /// Default implementation execs SWUpdate.
    using InstallFn = std::function<Result<int>(const std::string& artifact, const std::string& target_slot, const ArtifactManifest& m)>;
    /// @brief Replace the installer.
    /// @param f Installer replacement (tests).
    void set_install_fn(InstallFn f)
    {
        install_fn_ = std::move(f);
    }

    /// @brief Register a release public key (§7.9), by key id.
    ///
    /// Public keys only: a device can verify a release but nothing on it can
    /// sign one. Several are held at once so a signing key can be rotated
    /// across a fleet gradually.
    /// @param key_id Key id to trust.
    /// @param public_key_raw32 Raw 32-byte public key.
    void add_release_key(const std::string& key_id, const std::string& public_key_raw32)
    {
        release_keys_[key_id] = public_key_raw32;
    }
    /// @brief Remove a release key.
    /// @param key_id Key id to retire.
    void remove_release_key(const std::string& key_id)
    {
        release_keys_.erase(key_id);
    }
    /// @brief Registered release keys (key id → raw public key).
    /// @return Registered release keys (key id → raw public key).
    const std::map<std::string, std::string>& release_keys() const
    {
        return release_keys_;
    }

    /// @brief Current update state machine.
    /// @return The current update state machine.
    const UpdateState& state() const
    {
        return state_;
    }
    /// @brief Finished update history.
    /// @return Finished update history.
    const std::vector<HistoryEntry>& history() const
    {
        return history_;
    }

    /// @brief Receive one artifact chunk; supports resume via the persisted offset.
    ///
    /// Re-calls with the same offset append. Returns bytes stored so far.
    /// @param data Chunk bytes.
    /// @param offset Expected byte offset (resume checkpoint).
    /// @return Bytes stored so far, or an error.
    Result<uint64_t> receive_chunk(const std::string& data, uint64_t offset);
    /// @brief Bytes stored so far for the in-flight artifact.
    /// @return Bytes stored so far for the in-flight artifact.
    Result<uint64_t> received_bytes() const;

    /// @brief Parse and validate a manifest JSON document.
    /// @param manifest_json Manifest JSON text.
    /// @return The parsed manifest, or an error.
    Result<ArtifactManifest> parse_manifest(const std::string& manifest_json) const;
    /// @brief Verify an artifact: integrity, then signature, then anti-rollback.
    ///
    /// @param m Parsed manifest.
    /// @param enrollment_secret Secret for the legacy HMAC path (unused when release keys exist).
    /// @param running_version Currently running version (anti-rollback floor).
    /// @return ok(), or the specific verification failure.
    Error verify(const ArtifactManifest& m, const std::string& enrollment_secret, const std::string& running_version);

    /// @brief Verify and place the artifact in the staging slot (§7.9 gates staging on verification).
    /// @param m Parsed manifest.
    /// @param enrollment_secret Secret for the legacy HMAC path.
    /// @param running_version Currently running version (anti-rollback floor).
    /// @return ok(), or the verification error.
    Error stage(const ArtifactManifest& m, const std::string& enrollment_secret, const std::string& running_version);
    /// @brief Run the installer for the staged artifact and switch boot slots.
    /// @return ok(), or the platform error.
    Error apply();

    /// @brief Confirm the pending trial slot after a successful boot (§11).
    ///
    /// @return ok() on confirmation, or an error.
    /// On boot with a pending trial slot, the engine waits for confirm();
    /// past the deadline it rolls back automatically.
    Error confirm();
    /// @brief Roll back to the previous slot with a reason.
    /// @param reason Why the rollback happened.
    /// @return ok(), or the platform error.
    Error rollback(const std::string& reason);
    /// @brief Periodic call: fires the automatic rollback when the deadline is overdue.
    /// @param now_mono_ns Current monotonic time.
    /// @return A progress event, or std::nullopt.
    std::optional<OtaProgressEvent> tick(uint64_t now_mono_ns);

    /// @brief True when updates are permitted at this wall time.
    /// @param wall_unix_s Current wall time.
    /// @return True when updates are permitted.
    bool in_maintenance_window(int64_t wall_unix_s) const;
    /// @brief Replace the maintenance windows.
    /// @param w Maintenance windows to install.
    void set_windows(const std::vector<OtaConfig::Window>& w)
    {
        cfg_.windows = w;
    }
    /// @brief Override the bandwidth limit.
    /// @param bytes_per_s Download cap in bytes per second (0 = unlimited).
    void set_bandwidth_limit(uint64_t bytes_per_s)
    {
        cfg_.bandwidth_limit_bytes_per_s = bytes_per_s;
    }
    /// @brief Bytes the limiter permits right now (token bucket).
    /// @param dt_s Elapsed seconds since the last call.
    /// @return Bytes the limiter permits.
    uint64_t bandwidth_allotment(double dt_s);

    /// @brief Canary support: enough state for HQ staged rollouts.
    ///
    /// @param health_score Device health 0..1.
    /// @param fault_rate_24h Fault rate over 24h.
    /// @return Canary status JSON.
    std::string canary_json(double health_score, double fault_rate_24h) const;

    /// @brief Install the progress-event sink.
    /// @param f Progress-event sink.
    void set_event_sink(std::function<void(const OtaProgressEvent&)> f)
    {
        event_sink_ = std::move(f);
    }
    /// @brief Install the enrollment-secret provider (never stored at rest).
    /// @param f Enrollment-secret provider.
    void set_enrollment_secret_provider(std::function<std::string()> f)
    {
        secret_provider_ = std::move(f);
    }

  private:
    /// @brief Emit a progress event to the sink and record the state.
    /// @param stage Stage name.
    /// @param progress 0..1 progress.
    /// @param message Human-readable detail.
    /// @return ok(), or an error when the sink rejects.
    Error emit(const std::string& stage, double progress, const std::string& message);
    /// @brief Run the install engine (SWUpdate by default, install_fn_ when set).
    /// @param artifact Staged artifact path.
    /// @param target_slot Boot slot to install into.
    /// @param m Manifest describing the artifact.
    /// @return The installer exit status, or an error.
    Result<int> run_install(const std::string& artifact, const std::string& target_slot, const ArtifactManifest& m);
    /// @brief Staged artifact path.
    /// @return The staged artifact path.
    std::string staging_path() const
    {
        return "/pst/state/ota/artifact.bin";
    }
    /// @brief Resume-offset file path.
    /// @return The resume-offset file path.
    std::string offset_path() const
    {
        return "/pst/state/ota/offset";
    }
    /// @brief Manifest file path.
    /// @return The manifest file path.
    std::string manifest_path() const
    {
        return "/pst/state/ota/manifest.json";
    }
    /// @brief History file path.
    /// @return The history file path.
    std::string history_path() const
    {
        return "/pst/state/ota/history.json";
    }
    /// @brief Load the update history from disk.
    /// @return The update history loaded from disk.
    std::string history_json() const;

    IFileSystem& fs_;                     ///< Storage backend.
    platform::IPlatform& plat_;            ///< Boot-slot operations.
    TimeSource& ts_;                       ///< Clock.
    OtaConfig cfg_;                        ///< Current policy.
    UpdateState state_;                    ///< State-machine state.
    std::vector<HistoryEntry> history_;     ///< Finished updates.
    std::function<void(const OtaProgressEvent&)> event_sink_;   ///< Progress sink.
    std::function<std::string()> secret_provider_;   ///< Enrollment-secret provider.
    std::map<std::string, std::string> release_keys_;   ///< Release public keys by id.
    std::string swupdate_binary_ = "/usr/bin/swupdate";   ///< Install engine binary.
    InstallFn install_fn_;                 ///< Optional injected installer (tests).
    TokenBucket bandwidth_{0, 0};          ///< Download bandwidth limiter.
    double last_bw_now_s_ = 0;             ///< Last limiter refill time.
    double tokens_ = 0;                    ///< Limiter token count.
};

} // namespace domain
} // namespace ota
} // namespace instr