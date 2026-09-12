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
 * @file daemons/istate/src/config_service.cpp
 * @brief istate configuration service implementation (REQ §9, §10): origin tracking, signed HQ push verification, audit trail, and drift reporting.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/istate/config_service.h"

#include "instr/common/signature.h"

#include <cmath>

namespace instr
{
/// @brief istate: /pst owner, spoke orchestrator and HQ northbound bridge (R4, §7.1).
namespace istate
{

// ---------------- AuditLog ----------------

/// @brief Append a stamped audit entry (bounded).
/// @param actor Who performed the action.
/// @param source Where it came from.
/// @param action Action kind.
/// @param detail_json Action detail.
/// @param ts Clock for stamping.
void AuditLog::append(const std::string& actor, const std::string& source, const std::string& action, const std::string& detail_json, TimeSource& ts)
{
    AuditEntry e;
    e.seq = next_seq_++;
    e.wall_unix_s = ts.wall_ns() / 1'000'000'000ll;
    e.actor = actor;
    e.source = source;
    e.action = action;
    e.detail_json = detail_json;
    entries_.push_back(e);
    while (entries_.size() > max_entries_) entries_.pop_front();
}

/// @brief Persist the audit log atomically.
Error AuditLog::persist()
{
    std::string text;
    for (const auto& e : entries_) { text += std::to_string(e.seq) + "\t" + std::to_string(e.wall_unix_s) + "\t" + e.actor + "\t" + e.source + "\t" + e.action + "\t" + e.detail_json + "\n"; }
    return AtomicFile::write(fs_, path_, text);
}

/// @brief Load the audit log from disk.
Error AuditLog::load()
{
    auto txt = fs_.read(path_);
    if (!txt.ok()) return Error::none();
    entries_.clear();
    size_t pos = 0;
    while (pos < txt->size())
    {
        size_t nl = txt->find('\n', pos);
        if (nl == std::string::npos) nl = txt->size();
        std::string line = txt->substr(pos, nl - pos);
        pos = nl + 1;
        // seq \t wall \t actor \t source \t action \t detail
        std::vector<std::string> f;
        size_t p = 0;
        while (true)
        {
            size_t t = line.find('\t', p);
            if (t == std::string::npos)
            {
                f.push_back(line.substr(p));
                break;
            }
            f.push_back(line.substr(p, t - p));
            p = t + 1;
        }
        if (f.size() < 6) continue;
        AuditEntry e;
        try
        {
            e.seq = std::stoull(f[0]);
            e.wall_unix_s = std::stoll(f[1]);
        }
        catch (...)
        {
            continue;
        }
        e.actor = f[2];
        e.source = f[3];
        e.action = f[4];
        e.detail_json = f[5];
        entries_.push_back(e);
        next_seq_ = std::max(next_seq_, e.seq + 1);
    }
    return Error::none();
}

// ---------------- ConfigService ----------------

/// @brief Load a daemon's effective config.
/// @param daemon Daemon to read.
/// @return The effective config, or an error.
Result<ConfigDocument> ConfigService::get(const std::string& daemon) const
{
    return pst_.load_config(daemon);
}

/// @brief Render the drift report (effective vs delivered) as JSON.
/// @param daemon Daemon to report on.
/// @return The drift report as JSON.
std::string ConfigService::drift_report_json(const std::string& daemon) const
{
    auto doc = pst_.load_config(daemon);
    if (!doc.ok()) return "{}";
    JValue j = JValue::object();
    JValue vals = JValue::object();
    JValue origins = JValue::object();
    for (const auto& kv : (*doc).values())
    {
        // Typed projection: numbers as numbers, booleans as booleans.
        try
        {
            const std::string& v = kv.second;
            if (v == "true" || v == "false")
                vals.set(kv.first, JValue(v == "true"));
            else
            {
                double d = std::stod(v, nullptr);
                vals.set(kv.first, JValue(d));
            }
        }
        catch (...)
        {
            vals.set(kv.first, JValue(kv.second));
        }
        origins.set(kv.first, JValue(origin_name((*doc).origin_of(kv.first))));
    }
    j.set("daemon", JValue(daemon));
    j.set("effective", vals);
    j.set("origins", origins);
    j.set("schema_version", JValue(pst_.schema_version()));
    return j.dump();
}

/// @brief Merge values by origin rules and persist.
/// @param daemon Target daemon.
/// @param yaml_text Incoming YAML.
/// @param origin Claimed origin of the values.
/// @param force Overwrite protected local keys when true.
/// @return Rejected paths and error text.
ConfigService::SetResult ConfigService::set(const std::string& daemon, const std::string& yaml_text, Origin origin, bool force)
{
    SetResult res;
    // Validate against the daemon's schema when one is registered.
    auto sit = schemas_.find(daemon);
    JValue schema = sit != schemas_.end() ? sit->second : JValue::object();
    if (!schema.is_null() && schema.is_object() && schema.size() > 0)
    {
        auto j = json_from_yaml_text(yaml_text);
        if (!j.ok())
        {
            res.error = j.error.str();
            return res;
        }
        std::vector<JsonValidator::Issue> issues;
        if (!JsonValidator::validate(schema, *j, &issues))
        {
            std::string msg = "configuration rejected (" + daemon + "):";
            for (const auto& is : issues) msg += " [" + is.path + "] " + is.message + ";";
            res.error = msg;
            return res;
        }
    }
    auto current = pst_.load_config(daemon);
    if (!current.ok())
    {
        // Missing config: start from parsed incoming.
        auto fresh = ConfigDocument::parse(yaml_text, schema.is_object() ? schema : JValue::object(), daemon);
        if (!fresh.ok())
        {
            res.error = fresh.error.str();
            return res;
        }
        Error e = pst_.save_config(daemon, *fresh);
        if (!e.ok())
        {
            res.error = e.str();
            return res;
        }
        res.ok = true;
        return res;
    }
    auto incoming = ConfigDocument::parse(yaml_text, schema.is_object() ? schema : JValue::object(), daemon);
    if (!incoming.ok())
    {
        res.error = incoming.error.str();
        return res;
    }
    res.rejected = (*current).merge_from(*incoming, origin, force);
    Error e = pst_.save_config(daemon, *current);
    if (!e.ok())
    {
        res.error = e.str();
        return res;
    }
    res.ok = true;
    return res;
}

/// @brief Verify then apply a signed config push.
/// @param daemon Target daemon.
/// @param yaml_text Incoming YAML.
/// @param key_id Signing key id.
/// @param signature Ed25519 signature.
/// @param hmac_hex Legacy HMAC.
/// @param enrollment_secret Secret for the legacy path.
/// @return Rejected paths, or an error when verification fails.
Result<std::vector<std::string>> ConfigService::apply_signed(const std::string& daemon, const std::string& yaml_text, const std::string& key_id, const std::string& signature, const std::string& hmac_hex, const std::string& enrollment_secret)
{
    // Verification happens BEFORE any write (§9). Nothing below this point
    // touches /pst until the push has proven it came from HQ.
    const std::string signed_bytes = daemon + "|" + yaml_text;
    last_push_legacy_ = false;

    if (!signing_keys_.empty())
    {
        // Asymmetric path: the device holds public keys only.
        auto it = signing_keys_.find(key_id);
        if (it == signing_keys_.end()) return Result<std::vector<std::string>>::fail(Errc::SignatureInvalid, "signed config push names unknown key id '" + key_id + "' — refused", "apply_signed");
        if (signature.empty())
            return Result<std::vector<std::string>>::fail(Errc::SignatureInvalid,
                                                          "signed config push carries no signature — refused (a device with signing keys "
                                                          "provisioned does not accept the legacy HMAC form)",
                                                          "apply_signed");
        Error e = crypto::verify_ed25519(it->second, signed_bytes, signature);
        if (!e.ok()) return Result<std::vector<std::string>>::fail(Errc::SignatureInvalid, "signed config push failed signature verification (" + e.message + ") — refused", "apply_signed");
    }
    else
    {
        // No signing key provisioned: fall back to the shared-secret form so a
        // device enrolled before key distribution can still be configured. This
        // is weaker — whatever verifies can also forge — so it is reported and
        // stops being accepted the moment a key is provisioned.
        if (enrollment_secret.empty()) return Result<std::vector<std::string>>::fail(Errc::SignatureInvalid, "no signing key and no enrollment secret: cannot verify this push — refused", "apply_signed");
        auto digest = crypto::hmac_sha256(enrollment_secret, signed_bytes);
        const std::string expect = crypto::hex_encode(std::string(reinterpret_cast<const char*>(digest.b.data()), 32));
        if (hmac_hex != expect) return Result<std::vector<std::string>>::fail(Errc::SignatureInvalid, "signed config push failed HMAC verification — refused", "apply_signed");
        last_push_legacy_ = true;
    }

    auto r = set(daemon, yaml_text, Origin::Hq, false);
    if (!r.ok) return Result<std::vector<std::string>>::fail(Error::make(Errc::InvalidArgument, r.error, "apply_signed"));
    return Result<std::vector<std::string>>::good(r.rejected);
}

} // namespace istate
} // namespace instr