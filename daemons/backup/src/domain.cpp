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
 * @file daemons/backup/src/domain.cpp
 * @brief backup domain implementation (REQ §7.10): tar archive build/restore, manifest, exclusions, identity-derived encryption, preview, and transactional restore.
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/backup/domain.h"

#include <cstring>

#include "instr/common/json.h"

#include <algorithm>
#include <sstream>

namespace instr
{
/// @brief Backup daemon: encrypted archives, restore and reset (§7.10).
namespace backup
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief Serialise a manifest to JSON.
/// @param m Manifest to serialise.
/// @return The manifest JSON text.
static std::string manifest_json(const BackupManifest& m)
{
    JValue j = JValue::object();
    j.set("device_serial", JValue(m.device_serial));
    j.set("hardware_revision", JValue(m.hardware_revision));
    j.set("schema_version", JValue(m.schema_version));
    j.set("firmware_version", JValue(m.firmware_version));
    j.set("created_unix_s", JValue(double(m.created_unix_s)));
    JValue files = JValue::array();
    for (const auto& f : m.files) files.push(JValue(f));
    j.set("files", files);
    JValue sums = JValue::object();
    for (const auto& kv : m.sha256) sums.set(kv.first, JValue(kv.second));
    j.set("sha256", sums);
    return j.dump();
}

/// @brief Parse and validate a manifest JSON document.
/// @param json Manifest JSON text.
/// @return The parsed manifest, or an error.
static Result<BackupManifest> parse_manifest(const std::string& json)
{
    auto j = json_parse(json);
    if (!j.ok() || !(*j).is_object()) return Result<BackupManifest>::fail(Errc::InvalidArgument, "manifest must be a JSON object", "manifest");
    BackupManifest m;
    if (const JValue* v = (*j).find("device_serial")) m.device_serial = v->as_string();
    if (const JValue* v = (*j).find("hardware_revision")) m.hardware_revision = v->as_string();
    if (const JValue* v = (*j).find("schema_version")) m.schema_version = v->as_string();
    if (const JValue* v = (*j).find("firmware_version")) m.firmware_version = v->as_string();
    if (const JValue* v = (*j).find("created_unix_s")) m.created_unix_s = int64_t(v->as_number());
    if (const JValue* v = (*j).find("files"))
        for (const auto& f : v->items()) m.files.push_back(f.as_string());
    if (const JValue* v = (*j).find("sha256"))
        for (const auto& kv : v->members()) m.sha256[kv.first] = kv.second.as_string();
    return Result<BackupManifest>::good(m);
}

/// @brief Decide whether a relative path is excluded (built-ins plus @p excludes).
/// @param rel Relative path to test.
/// @param excludes Configured exclusion patterns.
/// @return True when excluded.
bool BackupEngine::excluded_path(const std::string& rel, const std::vector<std::string>& excludes)
{
    // Identity is not portable: factory/ and WireGuard keys never in backups.
    if (rel.rfind("factory/", 0) == 0) return true;
    if (rel.find("wg-private.key") != std::string::npos) return true;
    if (rel.find(".secret") != std::string::npos) return true;
    for (const auto& e : excludes)
        if (rel.rfind(e, 0) == 0) return true;
    return false;
}

/// @brief Recursively collect (rel path, contents) pairs under a root.
/// @param fs Filesystem to walk.
/// @param root Root to collect under.
/// @param rel Current relative path.
/// @param excludes Exclusion patterns.
/// @param out Receives (rel path, contents) pairs.
static void collect(IFileSystem& fs, const std::string& root, const std::string& rel, const std::vector<std::string>& excludes, std::vector<std::pair<std::string, std::string>>* out)
{
    // rel is the path relative to root; out gets (relpath, content).
    auto entries = fs.list_dir(root + (rel.empty() ? "" : "/" + rel));
    if (!entries.ok()) return;
    for (const auto& name : *entries)
    {
        std::string child_rel = rel.empty() ? name : rel + "/" + name;
        std::string full = root + "/" + child_rel;
        if (fs.exists(full))
        {
            if (!fs.list_dir(full).ok())
            {
                // Regular file (list_dir fails on files).
                if (BackupEngine::excluded_path(child_rel, excludes)) continue;
                auto content = fs.read(full);
                if (content.ok()) out->emplace_back(child_rel, *content);
            }
            else { collect(fs, root, child_rel, excludes, out); }
        }
    }
}

/// @brief Derive the local archive key: HKDF over identity serial+secret.
/// @return The identity-derived archive key.
std::string BackupEngine::key_for_archive() const
{
    // Local archives: key derived from the factory-provisioned identity secret
    // (HKDF-SHA256). The secret is device-class identity, NOT the per-board
    // serial — so a replacement board can decrypt a config backup (the
    // manifest serial then forces fresh enrollment); the WireGuard keys and
    // factory/ are excluded unconditionally, so identity itself is never
    // portable through a backup (REQ §7.10).
    return crypto::hkdf_sha256(opts_.identity_secret, "arches-backup-v1", "local-archive-key", 32);
}

/// @brief Seal an archive with the local identity-derived key.
/// @param tar_bytes Archive bytes to seal.
/// @return The sealed bytes.
std::string BackupEngine::encrypt_archive(const std::string& tar_bytes) const
{
    return crypto::aead_seal(key_for_archive(), tar_bytes, [](uint8_t* out, size_t n) { (void)crypto::random_bytes(out, n); });
}

/// @brief Decrypt a locally sealed archive.
/// @param blob Sealed archive bytes.
/// @return The decrypted archive, or an error.
Result<tar::FileEntry> BackupEngine::decrypt_archive(const std::string& blob) const
{
    auto opened = crypto::aead_open(key_for_archive(), blob);
    if (!opened.ok()) return Result<tar::FileEntry>::fail(opened.error);
    tar::FileEntry e;
    e.name = "archive.tar";
    e.data = *opened;
    return Result<tar::FileEntry>::good(e);
}

/// @brief Tar, hash and store an archive with its manifest.
/// @param paths Paths to archive.
/// @param excludes Extra exclusions.
/// @param archive_dir Directory to store the archive.
/// @param now_unix_s Creation time.
/// @return Archive summary, or an error.
Result<ArchiveInfo> BackupEngine::create(const std::vector<std::string>& paths, const std::vector<std::string>& excludes, const std::string& archive_dir, int64_t now_unix_s)
{
    std::vector<std::pair<std::string, std::string>> files;
    for (const auto& p : paths) collect(fs_, p, "", excludes, &files);

    BackupManifest m;
    m.device_serial = opts_.device_serial;
    m.hardware_revision = opts_.hardware_revision;
    m.schema_version = opts_.schema_version;
    m.firmware_version = opts_.firmware_version;
    m.created_unix_s = now_unix_s;
    std::vector<tar::FileEntry> tar_files;
    for (const auto& kv : files)
    {
        tar::FileEntry e;
        e.name = kv.first;
        e.data = kv.second;
        e.mtime = uint64_t(now_unix_s);
        tar_files.push_back(e);
        m.files.push_back(kv.first);
        m.sha256[kv.first] = crypto::sha256(kv.second).hex();
    }
    // Manifest travels inside the archive.
    tar::FileEntry man;
    man.name = "__manifest__.json";
    man.data = manifest_json(m);
    man.mtime = uint64_t(now_unix_s);
    tar_files.push_back(man);

    std::string tar_bytes = tar::write_ustar(tar_files);
    if (tar_bytes.empty()) return Result<ArchiveInfo>::fail(Errc::InvalidArgument, "tar write failed (path too long?)", "create");
    std::string blob = encrypt_archive(tar_bytes);
    std::string name = "backup-" + std::to_string(now_unix_s) + ".abk";
    std::string path = fs_.join(archive_dir, name);
    Error e = fs_.write(path, blob);
    if (!e.ok()) return Result<ArchiveInfo>::fail(e);

    ArchiveInfo info;
    info.name = name;
    info.size_bytes = blob.size();
    info.sha256 = crypto::sha256(blob).hex();
    info.manifest = m;
    return Result<ArchiveInfo>::good(info);
}

/// @brief List archives in a directory.
/// @param archive_dir Directory to scan.
/// @return Archives found in the directory.
std::vector<ArchiveInfo> BackupEngine::list(const std::string& archive_dir) const
{
    std::vector<ArchiveInfo> out;
    auto files = fs_.list_dir(archive_dir);
    if (!files.ok()) return out;
    for (const auto& f : *files)
    {
        if (f.rfind("backup-", 0) != 0) continue;
        std::string path = fs_.join(archive_dir, f);
        auto blob = fs_.read(path);
        if (!blob.ok()) continue;
        ArchiveInfo info;
        info.name = f;
        info.size_bytes = blob->size();
        info.sha256 = crypto::sha256(*blob).hex();
        auto man = read_manifest(path);
        if (man.ok()) info.manifest = *man;
        out.push_back(info);
    }
    return out;
}

/// @brief Read and verify an archive's manifest.
/// @param archive_path Archive to inspect.
/// @return The embedded manifest, or an error.
Result<BackupManifest> BackupEngine::read_manifest(const std::string& archive_path) const
{
    auto blob = fs_.read(archive_path);
    if (!blob.ok()) return Result<BackupManifest>::fail(blob.error);
    auto tar_entry = decrypt_archive(*blob);
    if (!tar_entry.ok()) return Result<BackupManifest>::fail(tar_entry.error);
    auto entries = tar::read_ustar(tar_entry->data);
    if (!entries.ok()) return Result<BackupManifest>::fail(entries.error);
    for (const auto& e : *entries)
        if (e.name == "__manifest__.json") return parse_manifest(e.data);
    return Result<BackupManifest>::fail(Errc::Corrupt, "archive has no manifest", archive_path);
}

/// @brief Compute what a restore would change before committing.
/// @param archive_path Archive to preview.
/// @param restore_roots Roots restores may touch.
/// @return The preview, or an error.
Result<BackupEngine::RestorePreview> BackupEngine::restore_preview(const std::string& archive_path, const std::vector<std::string>& restore_roots)
{
    auto blob = fs_.read(archive_path);
    if (!blob.ok()) return Result<RestorePreview>::fail(blob.error);
    auto tar_entry = decrypt_archive(*blob);
    if (!tar_entry.ok()) return Result<RestorePreview>::fail(tar_entry.error);
    auto entries = tar::read_ustar(tar_entry->data);
    if (!entries.ok()) return Result<RestorePreview>::fail(entries.error);

    RestorePreview out;
    BackupManifest m;
    std::map<std::string, std::string> incoming;
    for (const auto& e : *entries)
    {
        if (e.name == "__manifest__.json")
        {
            auto mres = parse_manifest(e.data);
            if (!mres.ok()) return Result<RestorePreview>::fail(mres.error);
            m = *mres;
            continue;
        }
        incoming[e.name] = e.data;
    }
    if (m.files.empty() && incoming.empty()) return Result<RestorePreview>::fail(Errc::Corrupt, "archive has no manifest", archive_path);

    // Compatibility: hardware revision and schema version must match; a
    // replacement board forces fresh enrollment after config restore.
    out.replacement_board = m.device_serial != opts_.device_serial;
    if (m.hardware_revision != opts_.hardware_revision)
        out.compatibility = Compatibility::HardwareMismatch;
    else if (m.schema_version != opts_.schema_version)
        out.compatibility = Compatibility::SchemaMismatch;

    std::map<std::string, std::string> current;
    for (const auto& root : restore_roots)
    {
        std::vector<std::pair<std::string, std::string>> files;
        collect(fs_, root, "", {}, &files);
        for (const auto& kv : files) current[kv.first] = kv.second;
    }
    for (const auto& kv : incoming)
    {
        PreviewEntry e;
        e.path = kv.first;
        auto it = current.find(kv.first);
        if (it == current.end())
        {
            e.action = "add";
            e.detail = std::to_string(kv.second.size()) + " bytes";
        }
        else if (it->second == kv.second) { e.action = "unchanged"; }
        else
        {
            e.action = "replace";
            e.detail = "current " + std::to_string(it->second.size()) + " bytes → archive " + std::to_string(kv.second.size()) + " bytes";
        }
        out.entries.push_back(e);
    }
    for (const auto& kv : current)
    {
        if (!incoming.count(kv.first))
        {
            PreviewEntry e;
            e.path = kv.first;
            e.action = "delete";
            e.detail = "not present in archive";
            out.entries.push_back(e);
        }
    }
    return Result<RestorePreview>::good(std::move(out));
}

/// @brief Transactional restore: stage, verify every hash, then commit.
/// @param archive_path Archive to restore.
/// @param commit istate-directed commit callback.
/// @return Number of entries committed, or an error (prior state intact).
Result<size_t> BackupEngine::restore_commit(const std::string& archive_path, const CommitFn& commit)
{
    auto blob = fs_.read(archive_path);
    if (!blob.ok()) return Result<size_t>::fail(blob.error);
    auto tar_entry = decrypt_archive(*blob);
    if (!tar_entry.ok()) return Result<size_t>::fail(tar_entry.error);
    auto entries = tar::read_ustar(tar_entry->data);
    if (!entries.ok()) return Result<size_t>::fail(entries.error);

    // Stage + verify every hash before committing anything (transactional).
    std::vector<tar::FileEntry> staged;
    std::string manifest_json_str;
    for (const auto& e : *entries)
    {
        if (e.name == "__manifest__.json")
        {
            manifest_json_str = e.data;
            continue;
        }
        auto mres = parse_manifest(manifest_json_str.empty() ? "{}" : manifest_json_str);
        if (mres.ok())
        {
            auto it = mres->sha256.find(e.name);
            if (it != mres->sha256.end() && it->second != crypto::sha256(e.data).hex()) return Result<size_t>::fail(Errc::Corrupt, "file integrity failed: " + e.name, archive_path);
        }
        staged.push_back(e);
    }
    if (manifest_json_str.empty()) return Result<size_t>::fail(Errc::Corrupt, "archive has no manifest", archive_path);
    // Commit everything (completes fully or leaves prior state intact — the
    // commit callback is istate-directed and transactional).
    Error e = commit(staged);
    if (!e.ok()) return Result<size_t>::fail(e);
    return Result<size_t>::good(staged.size());
}

} // namespace domain
} // namespace backup
} // namespace instr
namespace instr
{
/// @brief Backup daemon: encrypted archives, restore and reset (§7.10).
namespace backup
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

// Sealed-box to HQ's public key (§7.10). See the header for the layout and
// why the ephemeral key matters.
static constexpr char kHqSealMagic[4] = {'A', 'H', 'Q', '1'};

/// @brief Seal plaintext to HQ's public key (ephemeral X25519 + AEAD).
/// @param plaintext Bytes to seal.
/// @return The sealed token, or an error when no HQ key is configured.
Result<std::string> BackupEngine::seal_for_hq(const std::string& plaintext) const
{
    if (opts_.hq_public_key.size() != 32)
        return Result<std::string>::fail(Errc::InvalidArgument,
                                         "no HQ public key configured: refusing to upload an "
                                         "archive unencrypted",
                                         "backup");
    auto rng = [](uint8_t* out, size_t n) { (void)crypto::random_bytes(out, n); };
    crypto::X25519Key eph = crypto::x25519_generate(rng);
    auto shared = crypto::x25519(eph.priv.data(), reinterpret_cast<const uint8_t*>(opts_.hq_public_key.data()));

    // Bind the ephemeral public key into the KDF so a shared secret cannot be
    // replayed under a different ephemeral key.
    std::string eph_pub(reinterpret_cast<const char*>(eph.pub.data()), 32);
    std::string key = crypto::hkdf_sha256(std::string(reinterpret_cast<const char*>(shared.data()), shared.size()), eph_pub, "arches-backup-hq-v1", 32);

    std::string out(kHqSealMagic, sizeof(kHqSealMagic));
    out += eph_pub;
    out += crypto::aead_seal(key, plaintext, rng);
    return Result<std::string>::good(std::move(out));
}

/// @brief Open an HQ-sealed token with HQ's private key.
/// @param sealed Token produced by seal_for_hq().
/// @param hq_private_key HQ's X25519 private key.
/// @return The plaintext, or an error.
Result<std::string> BackupEngine::open_from_hq(const std::string& sealed, const std::string& hq_private_key)
{
    // Present so the format is testable end to end on the device side: HQ runs
    // the same computation with its private key.
    if (hq_private_key.size() != 32) return Result<std::string>::fail(Errc::InvalidArgument, "bad HQ private key", "backup");
    if (sealed.size() < sizeof(kHqSealMagic) + 32 || std::memcmp(sealed.data(), kHqSealMagic, sizeof(kHqSealMagic)) != 0) return Result<std::string>::fail(Errc::Corrupt, "not an HQ-sealed archive", "backup");
    const std::string eph_pub = sealed.substr(sizeof(kHqSealMagic), 32);
    auto shared = crypto::x25519(reinterpret_cast<const uint8_t*>(hq_private_key.data()), reinterpret_cast<const uint8_t*>(eph_pub.data()));
    std::string key = crypto::hkdf_sha256(std::string(reinterpret_cast<const char*>(shared.data()), shared.size()), eph_pub, "arches-backup-hq-v1", 32);
    return crypto::aead_open(key, sealed.substr(sizeof(kHqSealMagic) + 32));
}

} // namespace domain
} // namespace backup
} // namespace instr
