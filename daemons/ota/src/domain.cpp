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
 * @file daemons/ota/src/domain.cpp
 * @brief ota domain implementation (REQ §7.9): signature and integrity verification, staging to the inactive slot, post-boot confirmation with automatic rollback, anti-rollback, and progress streaming.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/ota/domain.h"

#include "instr/common/signature.h"

#include <cstdio>

#include "instr/common/json.h"

#include <algorithm>
#include <ctime>
#include <sstream>

namespace instr
{
/// @brief OTA daemon: signed update pipeline with rollback (§7.9, §11).
namespace ota
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief Dotted-numeric version comparison with v-prefix tolerance.
/// @param a First version.
/// @param b Second version.
/// @return Negative / zero / positive, or an error for malformed versions.
Result<int> compare_versions(const std::string& a, const std::string& b)
{
    auto split = [](const std::string& s) {
        std::vector<uint64_t> out;
        std::string clean = s;
        if (!clean.empty() && (clean[0] == 'v' || clean[0] == 'V')) clean = clean.substr(1);
        std::istringstream ss(clean);
        std::string part;
        while (std::getline(ss, part, '.'))
        {
            try
            {
                out.push_back(std::stoull(part));
            }
            catch (...)
            {
                out.push_back(0);
            }
        }
        while (out.size() < 3) out.push_back(0);
        return out;
    };
    auto va = split(a);
    auto vb = split(b);
    for (size_t i = 0; i < 3; ++i)
    {
        if (va[i] < vb[i]) return Result<int>::good(-1);
        if (va[i] > vb[i]) return Result<int>::good(1);
    }
    return Result<int>::good(0);
}

/// @brief Append one artifact chunk with resume support.
/// @param data Chunk bytes.
/// @param offset Expected byte offset (resume checkpoint).
/// @return Bytes stored so far, or an error.
Result<uint64_t> OtaEngine::receive_chunk(const std::string& data, uint64_t offset)
{
    // Resume support: offset persisted alongside the artifact.
    uint64_t current = 0;
    auto off_txt = fs_.read(offset_path());
    if (off_txt.ok())
    {
        try
        {
            current = std::stoull(*off_txt);
        }
        catch (...)
        {
            current = 0;
        }
    }
    if (offset != current) return Result<uint64_t>::fail(Errc::InvalidArgument, "resume mismatch: have " + std::to_string(current) + ", got offset " + std::to_string(offset), "receive_chunk");
    Error e = fs_.write(staging_path(), data, /*append=*/true);
    if (!e.ok()) return Result<uint64_t>::fail(e);
    current += data.size();
    fs_.write(offset_path(), std::to_string(current));
    state_.state = "download";
    state_.progress = 0; // caller knows total; progress refined on verify
    emit("downloading", 0, "received " + std::to_string(current) + " bytes");
    return Result<uint64_t>::good(current);
}

/// @brief Bytes stored so far for the in-flight artifact.
/// @return Bytes stored so far for the in-flight artifact.
Result<uint64_t> OtaEngine::received_bytes() const
{
    auto off_txt = fs_.read(offset_path());
    if (!off_txt.ok()) return Result<uint64_t>::good(0);
    try
    {
        return Result<uint64_t>::good(std::stoull(*off_txt));
    }
    catch (...)
    {
        return Result<uint64_t>::good(0);
    }
}

/// @brief Parse and validate a manifest JSON document.
/// @param manifest_json Manifest JSON text.
/// @return The parsed manifest, or an error.
Result<ArtifactManifest> OtaEngine::parse_manifest(const std::string& manifest_json) const
{
    auto j = json_parse(manifest_json);
    if (!j.ok() || !(*j).is_object()) return Result<ArtifactManifest>::fail(Errc::InvalidArgument, "manifest must be a JSON object", "manifest");
    ArtifactManifest m;
    if (const JValue* v = (*j).find("version")) m.version = v->as_string();
    if (const JValue* v = (*j).find("sha256")) m.sha256 = v->as_string();
    if (const JValue* v = (*j).find("hmac")) m.hmac = v->as_string();
    if (const JValue* v = (*j).find("size_bytes")) m.size_bytes = uint64_t(v->as_number());
    if (const JValue* v = (*j).find("preserve_home")) m.preserve_home = v->as_bool(true);
    if (const JValue* v = (*j).find("preserve_pst")) m.preserve_pst = v->as_bool(true);
    if (m.version.empty() || m.sha256.empty()) return Result<ArtifactManifest>::fail(Errc::InvalidArgument, "manifest missing version/sha256", "manifest");
    return Result<ArtifactManifest>::good(m);
}

/// @brief Verify integrity, then signature, then anti-rollback floor.
/// @param m Parsed manifest.
/// @param enrollment_secret Secret for the legacy HMAC path.
/// @param running_version Currently running version (anti-rollback floor).
/// @return ok(), or the specific verification failure.
Error OtaEngine::verify(const ArtifactManifest& m, const std::string& enrollment_secret, const std::string& running_version)
{
    emit("verifying", 0.1, "integrity check");
    // Integrity: SHA-256 of the landed artifact.
    auto artifact = fs_.read(staging_path());
    if (!artifact.ok()) return Error::make(Errc::NotFound, "artifact not landed on disk", staging_path());
    if (crypto::sha256(*artifact).hex() != m.sha256) return Error::make(Errc::Corrupt, "artifact integrity verification failed (sha256 mismatch)", "verify");
    // Signature verification before staging (§7.9). This runs unconditionally:
    // an artifact whose origin cannot be established is refused. Skipping the
    // check when no secret happened to be configured — as this once did —
    // means an unsigned artifact installs on any device that has not been
    // enrolled yet, which is exactly the device most worth attacking.
    if (!release_keys_.empty())
    {
        // Asymmetric path: the device holds public keys only.
        auto it = release_keys_.find(m.key_id);
        if (it == release_keys_.end()) return Error::make(Errc::SignatureInvalid, "artifact names unknown release key id '" + m.key_id + "'", "verify");
        if (m.signature.empty()) return Error::make(Errc::SignatureInvalid, "artifact carries no release signature — refused", "verify");
        Error e = crypto::verify_ed25519(it->second, m.version + "|" + m.sha256, m.signature);
        if (!e.ok()) return Error::make(Errc::SignatureInvalid, "artifact signature invalid (" + e.message + ")", "verify");
    }
    else if (!enrollment_secret.empty())
    {
        // Legacy shared-secret form for devices provisioned before release keys
        // were distributed.
        const std::string expect = crypto::hmac_sha256(enrollment_secret, m.version + m.sha256).hex();
        if (m.hmac.empty() || m.hmac != expect) return Error::make(Errc::SignatureInvalid, "artifact signature invalid", "verify");
    }
    else
    {
        return Error::make(Errc::SignatureInvalid,
                           "no release key and no enrollment secret: cannot establish the origin "
                           "of this artifact — refused",
                           "verify");
    }
    // Anti-rollback: refuse downgrade below the configured minimum version.
    auto vs_min = compare_versions(m.version, cfg_.min_version);
    if (vs_min.ok() && *vs_min < 0) return Error::make(Errc::VersionMismatch, "refusing downgrade below minimum version " + cfg_.min_version, "anti-rollback");
    auto vs_run = compare_versions(m.version, running_version);
    if (vs_run.ok() && *vs_run < 0) return Error::make(Errc::VersionMismatch, "refusing downgrade below running version " + running_version, "anti-rollback");
    state_.state = "verified";
    state_.to_version = m.version;
    state_.from_version = running_version;
    emit("verified", 0.2, "artifact verified: " + m.version);
    return Error::none();
}

/// @brief Run the install engine (SWUpdate or the injected installer).
/// @param artifact Staged artifact path.
/// @param target_slot Boot slot to install into.
/// @param m Manifest describing the artifact.
/// @return The installer exit status, or an error.
Result<int> OtaEngine::run_install(const std::string& artifact, const std::string& target_slot, const ArtifactManifest& m)
{
    if (install_fn_) return install_fn_(artifact, target_slot, m);

    // Default engine: SWUpdate (§7.9). We are a thin manager on top of it —
    // it owns writing the image into the slot, we own verification, policy
    // and the boot-slot lifecycle around it.
    //   -i <file>   install this artifact
    //   -e <sel>    software selection: which slot to install into
    //   -p          post-install: we handle the reboot decision ourselves
    std::string cmd = swupdate_binary_ + " -i " + artifact + " -e stable," + target_slot;
    if (m.preserve_pst || m.preserve_home)
    {
        // /home and /pst are preserved unless the update explicitly asks
        // otherwise (§6, §7.9); SWUpdate honours this through the selection.
        cmd += " -e preserve,data";
    }
    cmd += " 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return Result<int>::fail(Errc::Unavailable, "cannot launch " + swupdate_binary_, "install");
    std::string output;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe)) output += buf;
    int rc = ::pclose(pipe);
    if (rc != 0) return Result<int>::fail(Errc::Io, "swupdate failed (rc=" + std::to_string(rc) + "): " + output, "install");
    return Result<int>::good(0);
}

/// @brief Verify and place the artifact in the staging slot.
/// @param m Parsed manifest.
/// @param enrollment_secret Secret for the legacy HMAC path.
/// @param running_version Currently running version (anti-rollback floor).
Error OtaEngine::stage(const ArtifactManifest& m, const std::string& enrollment_secret, const std::string& running_version)
{
    Error e = verify(m, enrollment_secret, running_version);
    if (!e.ok()) return e;
    // Maintenance window gate: updates apply only within the schedule.
    int64_t wall = ts_.wall_ns() / 1'000'000'000ll;
    if (!in_maintenance_window(wall) && !cfg_.windows.empty()) return Error::make(Errc::Busy, "outside maintenance window", "stage");
    emit("staging", 0.4, "stage to inactive slot");
    auto slots = plat_.boot_slots();
    if (!slots.ok()) return slots.error;
    const std::string inactive = slots->inactive;

    // Hand the verified artifact to SWUpdate, which writes the inactive slot.
    auto installed = run_install(staging_path(), inactive, m);
    if (!installed.ok())
    {
        state_.state = "failed";
        state_.error = installed.error.str();
        emit("failed", 0.4, state_.error);
        HistoryEntry h;
        h.from_version = running_version;
        h.to_version = m.version;
        h.result = "failed";
        h.finished_unix_s = ts_.wall_ns() / 1'000'000'000ll;
        history_.push_back(h);
        fs_.write(history_path(), history_json());
        return installed.error;
    }
    state_.state = "staged";
    state_.staged_slot = inactive;
    state_.from_version = running_version;
    state_.to_version = m.version;
    emit("staged", 0.5, "staged to slot " + inactive);
    return Error::none();
}

/// @brief Run the installer and switch boot slots.
Error OtaEngine::apply()
{
    if (state_.state != "staged") return Error::make(Errc::Busy, "no staged update to apply (state " + state_.state + ")", "apply");
    Error e = plat_.set_boot_slot(state_.staged_slot, /*trial=*/true);
    if (!e.ok()) return e;
    state_.state = "applied_pending_confirm";
    state_.confirm_deadline_mono = ts_.mono_ns() + uint64_t(cfg_.confirm_timeout_s * 1e9);
    emit("applied", 0.7, "applied; awaiting explicit confirmation (auto-rollback armed)");
    return Error::none();
}

/// @brief Confirm the pending trial slot.
Error OtaEngine::confirm()
{
    if (state_.state != "applied_pending_confirm") return Error::make(Errc::Busy, "nothing pending confirmation", "confirm");
    Error e = plat_.confirm_boot_slot();
    if (!e.ok()) return e;
    state_.state = "confirmed";
    emit("confirmed", 1.0, "update confirmed: " + state_.to_version);
    HistoryEntry h;
    h.from_version = state_.from_version;
    h.to_version = state_.to_version;
    h.result = "confirmed";
    h.finished_unix_s = ts_.wall_ns() / 1'000'000'000ll;
    history_.push_back(h);
    fs_.write(history_path(), history_json());
    return Error::none();
}

/// @brief Roll back to the previous slot.
/// @param reason Why the rollback happened.
Error OtaEngine::rollback(const std::string& reason)
{
    auto slots = plat_.boot_slots();
    if (!slots.ok()) return slots.error;
    Error e = plat_.set_boot_slot(slots->inactive, /*trial=*/false);
    if (!e.ok()) return e;
    e = plat_.confirm_boot_slot();
    if (!e.ok()) return e;
    state_.state = "rolled_back";
    state_.error = reason;
    emit("rolled_back", 0, "rollback: " + reason);
    HistoryEntry h;
    h.from_version = state_.from_version;
    h.to_version = state_.to_version;
    h.result = "rolled_back";
    h.finished_unix_s = ts_.wall_ns() / 1'000'000'000ll;
    history_.push_back(h);
    fs_.write(history_path(), history_json());
    return Error::none();
}

/// @brief Periodic call: fires the automatic rollback when overdue.
/// @param now_mono_ns Current monotonic time.
/// @return A progress event, or std::nullopt.
std::optional<OtaProgressEvent> OtaEngine::tick(uint64_t now_mono_ns)
{
    if (state_.state == "applied_pending_confirm" && now_mono_ns > state_.confirm_deadline_mono)
    {
        // An unconfirmed update rolls back automatically (§11).
        Error e = rollback("confirmation timeout elapsed");
        if (e.ok()) return OtaProgressEvent{"rolled_back", 0, "auto-rollback after confirmation timeout", state_.from_version, state_.to_version};
    }
    return std::nullopt;
}

/// @brief True when updates are permitted at this wall time.
/// @param wall_unix_s Current wall time.
/// @return True when updates are permitted.
bool OtaEngine::in_maintenance_window(int64_t wall_unix_s) const
{
    if (cfg_.windows.empty()) return true; // no schedule → always permitted
    std::time_t t = wall_unix_s;
    struct std::tm tm;
    gmtime_r(&t, &tm);
    static const char* days[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    std::string today = days[tm.tm_wday];
    int now_min = tm.tm_hour * 60 + tm.tm_min;
    for (const auto& w : cfg_.windows)
    {
        bool day_ok = (w.day == "*");
        if (!day_ok)
        {
            std::istringstream ss(w.day);
            std::string d;
            while (std::getline(ss, d, ','))
                if (d == today) day_ok = true;
        }
        if (!day_ok) continue;
        auto parse_hm = [](const std::string& s) {
            int h = 0, m = 0;
            if (sscanf(s.c_str(), "%d:%d", &h, &m) == 2) return h * 60 + m;
            return 0;
        };
        int start = parse_hm(w.start);
        int end = parse_hm(w.end);
        if (now_min >= start && now_min < end) return true;
    }
    return false;
}

/// @brief Bytes the token-bucket limiter permits right now.
/// @param dt_s Elapsed seconds since the last call.
/// @return Bytes the limiter permits.
uint64_t OtaEngine::bandwidth_allotment(double dt_s)
{
    if (cfg_.bandwidth_limit_bytes_per_s == 0) return 1ull << 30; // unlimited
    double rate = double(cfg_.bandwidth_limit_bytes_per_s);
    if (bandwidth_.rate() != rate)
    {
        bandwidth_ = TokenBucket(rate, rate); // burst = 1s worth
        last_bw_now_s_ = 0;
        tokens_ = rate;
    }
    // Refill by elapsed time, grant floor(tokens), consume what is granted.
    tokens_ = std::min(rate, tokens_ + dt_s * rate);
    uint64_t grant = static_cast<uint64_t>(tokens_);
    tokens_ -= double(grant);
    (void)last_bw_now_s_;
    return grant;
}

/// @brief Load the update history from disk.
/// @return The update history loaded from disk.
std::string OtaEngine::history_json() const
{
    std::ostringstream o;
    o << "[";
    bool first = true;
    for (const auto& h : history_)
    {
        if (!first) o << ",";
        first = false;
        o << "{\"from\":\"" << h.from_version << "\",\"to\":\"" << h.to_version << "\",\"result\":\"" << h.result << "\",\"finished\":" << h.finished_unix_s << "}";
    }
    o << "]";
    return o.str();
}

/// @brief Render the canary status for HQ staged rollouts.
/// @param health_score Device health 0..1.
/// @param fault_rate_24h Fault rate over 24h.
/// @return The canary status JSON.
std::string OtaEngine::canary_json(double health_score, double fault_rate_24h) const
{
    JValue o = JValue::object();
    o.set("version", JValue(state_.state == "confirmed" ? state_.to_version : state_.from_version));
    o.set("health_score", JValue(health_score));
    o.set("fault_rate_24h", JValue(fault_rate_24h));
    o.set("update_state", JValue(state_.state));
    return o.dump();
}

/// @brief Emit a progress event and record the state.
/// @param stage Stage name.
/// @param progress 0..1 progress.
/// @param message Human-readable detail.
Error OtaEngine::emit(const std::string& stage, double progress, const std::string& message)
{
    state_.progress = progress;
    if (event_sink_)
    {
        OtaProgressEvent ev;
        ev.stage = stage;
        ev.progress = progress;
        ev.message = message;
        ev.from_version = state_.from_version;
        ev.to_version = state_.to_version;
        event_sink_(ev);
    }
    return Error::none();
}

} // namespace domain
} // namespace ota
} // namespace instr