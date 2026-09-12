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
 * @file daemons/backup/include/instr/backup/domain.h
 * @brief YAML lists the paths; tar them into an archive; restore by
 *
 * untarring (REQ §7.10). Snapshot /pst/system under istate's direction;
 * manifest inside the archive; excludes factory/ and WireGuard keys; local
 * archives encrypted with a key derived from device identity; transactional
 * restore with preview.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "instr/common/crypto.h"
#include "instr/common/error.h"
#include "instr/common/filesystem.h"
#include "instr/common/tar.h"
#include "instr/common/timestamps.h"

namespace instr
{
/// @brief Backup daemon: encrypted archives, restore and reset (§7.10).
namespace backup
{
/// @brief Pure domain logic for this daemon: no transport, no sockets, fully unit-testable.
namespace domain
{

/// @brief Manifest embedded in every backup archive.
struct BackupManifest
{
    std::string device_serial;                    ///< Originating device.
    std::string hardware_revision;                ///< Board revision.
    std::string schema_version;                   ///< Manifest schema version.
    std::string firmware_version;                 ///< Build version at backup time.
    int64_t created_unix_s = 0;                   ///< Creation time.
    std::vector<std::string> files;                ///< Relative paths inside the archive.
    std::map<std::string, std::string> sha256;     ///< Per-file integrity.
};

/// @brief Options controlling backup creation.
struct BackupOptions
{
    std::string device_serial;        ///< Originating device.
    std::string hardware_revision;    ///< Board revision.
    std::string schema_version;       ///< Manifest schema version.
    std::string firmware_version;     ///< Build version.
    std::string identity_secret;      ///< Key-derivation input (never logged, never stored).
    ///< HQ's X25519 public key (32 raw bytes). An archive destined for HQ is
    // sealed to this key, so the upload is unreadable to anything between the
    // device and HQ — including whatever holds the local archive key (§7.10).
    std::string hq_public_key;
};

/// @brief Summary of a stored archive.
struct ArchiveInfo
{
    std::string name;               ///< Archive file name.
    uint64_t size_bytes = 0;        ///< Archive size.
    std::string sha256;             ///< Archive digest.
    BackupManifest manifest;        ///< Embedded manifest.
};

/// @brief Backup engine: create, verify, preview and transactional restore, with encryption.
class BackupEngine
{
  public:
    /// @brief Bind the engine to storage and its options.
    /// @param fs Storage backend.
    /// @param opts Identity, versions and keys.
    BackupEngine(IFileSystem& fs, const BackupOptions& opts) : fs_(fs), opts_(opts)
    {
    }

    /// @brief Create an archive from the listed paths.
    ///
    /// Paths listed by config; istate directs access. Excludes factory/ and
    /// WireGuard keys unconditionally — device identity is not portable (a
    /// backup restores config, never identity).
    ///
    /// @param paths Paths to archive.
    /// @param excludes Extra exclusion patterns.
    /// @param archive_dir Where to store the archive.
    /// @param now_unix_s Creation time.
    /// @return Archive summary, or an error.
    Result<ArchiveInfo> create(const std::vector<std::string>& paths, const std::vector<std::string>& excludes, const std::string& archive_dir, int64_t now_unix_s);

    /// @brief List archives in a directory.
    /// @param archive_dir Directory to scan.
    /// @return Archive summaries, oldest first.
    std::vector<ArchiveInfo> list(const std::string& archive_dir) const;
    /// @brief Read and verify a manifest.
    /// @param archive_path Archive to inspect.
    /// @return The embedded manifest, or an error.
    Result<BackupManifest> read_manifest(const std::string& archive_path) const;

    /// @brief Restore preview: what will change before committing.
    struct PreviewEntry
    {
        std::string path;      ///< Affected path.
        std::string action;    ///< add / replace / delete / unchanged.
        std::string detail;    ///< Human-readable detail.
    };
    /// @brief Compatibility verdict of an archive against this board.
    enum class Compatibility
    {
        Ok,                  ///< Restorable as-is.
        HardwareMismatch,    ///< Different board revision.
        SchemaMismatch       ///< Unknown manifest schema.
    };
    /// @brief Full preview of a restore.
    struct RestorePreview
    {
        Compatibility compatibility = Compatibility::Ok;   ///< Verdict.
        bool replacement_board = false;   ///< Manifest serial != ours → config-only + re-enroll.
        std::vector<PreviewEntry> entries;     ///< Per-path actions.
    };
    /// @brief Compute the restore preview for an archive.
    /// @param archive_path Archive to preview.
    /// @param restore_roots Roots restores may touch.
    /// @return The preview, or an error.
    Result<RestorePreview> restore_preview(const std::string& archive_path, const std::vector<std::string>& restore_roots);

    /// @brief Transactional restore: completes fully or leaves prior state.
    ///
    /// 1) extract to a staging dir; 2) verify every hash; 3) commit via the
    /// commit callback (istate direction); 4) on any failure, leave prior
    /// state intact.
    using CommitFn = std::function<Error(const std::vector<tar::FileEntry>&)>;
    /// @brief Run the transactional restore.
    /// @return Number of entries committed, or an error (prior state intact).
    /// @param archive_path Archive to restore.
    /// @param commit istate-directed commit callback.
    Result<size_t> restore_commit(const std::string& archive_path, const CommitFn& commit);

    /// @brief Decide whether a relative path is excluded by the built-ins and @p excludes.
    /// @param rel Relative path to test.
    /// @param excludes Configured exclusion patterns.
    /// @return True when excluded.
    static bool excluded_path(const std::string& rel, const std::vector<std::string>& excludes);

    /// @brief Seal plaintext for upload to HQ (§7.10).
    ///
    /// Local archives are sealed with a key derived from device identity;
    /// archives going to HQ are sealed to HQ's public key instead — different
    /// threat models: the local key protects an archive at rest on the device,
    /// HQ's key protects it in transit and at rest in the cloud. Ephemeral
    /// X25519: a fresh keypair per archive, the shared secret run through
    /// HKDF; the ephemeral public key travels with the ciphertext, so HQ needs
    /// only its own private key to open it and the device keeps no long-term
    /// secret that could decrypt what it has already uploaded. Layout:
    /// "AHQ1" || ephemeral_pub(32) || aead_seal(...).
    ///
    /// @param plaintext Bytes to seal.
    /// @return Sealed token, or an error when no HQ key is configured.
    Result<std::string> seal_for_hq(const std::string& plaintext) const;
    /// @brief Open a seal_for_hq() token with HQ's private key.
    /// @param sealed Token produced by seal_for_hq().
    /// @param hq_private_key HQ's X25519 private key.
    /// @return The plaintext, or an error.
    static Result<std::string> open_from_hq(const std::string& sealed, const std::string& hq_private_key);
    /// @brief True when an HQ public key is configured (upload possible).
    bool hq_upload_available() const
    {
        return opts_.hq_public_key.size() == 32;
    }

  private:
    /// @brief Archive-encryption key: HKDF over identity serial+secret.
    /// @return The identity-derived archive key.
    std::string key_for_archive() const;
    /// @brief Decrypt a locally sealed archive.
    /// @param blob Sealed archive bytes.
    /// @return The decrypted archive, or an error.
    Result<tar::FileEntry> decrypt_archive(const std::string& blob) const;
    /// @brief Seal an archive with the local identity-derived key.
    /// @param tar_bytes Archive bytes to seal.
    /// @return The sealed archive bytes.
    std::string encrypt_archive(const std::string& tar_bytes) const;

    IFileSystem& fs_;        ///< Storage backend.
    BackupOptions opts_;     ///< Identity, versions and keys.
};

} // namespace domain
} // namespace backup
} // namespace instr