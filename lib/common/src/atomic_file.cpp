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
 * @file lib/common/src/atomic_file.cpp
 * @brief Implementation of atomic file writes: temp, fsync, rename, directory fsync, plus SHA-256 checksum and redundant backup sidecars (REQ §, §).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/common/atomic_file.h"

#include <unistd.h>

#include "instr/common/crypto.h"

namespace instr
{

/// @brief Plain atomic write (no checksum sidecars); temp = path + ".tmp-<pid>".
/// @param fs Filesystem to write through.
/// @param path Final destination path.
/// @param data Complete contents to write.
/// @return ok() on success, or the underlying I/O error.
Error AtomicFile::write(IFileSystem& fs, const std::string& path, const std::string& data)
{
    std::string tmp = path + ".tmp-" + std::to_string(::getpid());
    Error e = fs.write(tmp, data);
    if (!e.ok()) return e;
    e = fs.fsync_path(tmp);
    if (!e.ok())
    {
        fs.remove(tmp, false);
        return e;
    }
    e = fs.rename(tmp, path);
    if (!e.ok())
    {
        fs.remove(tmp, false);
        return e;
    }
    // fsync the containing directory so the rename is durable.
    size_t pos = path.find_last_of('/');
    if (pos != std::string::npos && pos > 0) fs.fsync_path(path.substr(0, pos));
    return Error::none();
}

/// @brief Atomic write with .checksum and .bak redundant copy.
/// @param fs Filesystem to write through.
/// @param path Final destination path.
/// @param data Complete contents to write.
/// @return ok() on success, or the underlying I/O error.
Error AtomicFile::write_protected(IFileSystem& fs, const std::string& path, const std::string& data)
{
    // Redundant copy per level (REQ §4/§6): the .bak copy holds the same new
    // content; if a crash interrupts the update cycle, the old .bak from the
    // previous completed write remains last-known-good.
    Error e = write(fs, path, data);
    if (!e.ok()) return e;
    e = fs.write(backup_path(path), data);
    if (!e.ok()) return e;
    // Checksum sidecars for both copies.
    std::string sum = crypto::sha256(data).hex();
    e = write(fs, checksum_path(path), sum);
    if (!e.ok()) return e;
    return write(fs, backup_path(path) + ".checksum", sum);
}

/// @brief Read a protected file, falling back to the .bak copy on checksum mismatch.
///
/// Returns Corrupt only when both primary and backup fail (REQ §11: /pst
/// corrupt → fall back to the redundant copy; if both fail, boot
/// unprovisioned).
///
/// @param fs Filesystem to read through.
/// @param path Protected file path.
/// @return Verified contents, or a Corrupt/Io error.
Result<std::string> AtomicFile::read_protected(IFileSystem& fs, const std::string& path)
{
    auto primary = fs.read(path);
    if (primary.ok())
    {
        auto sum = fs.read(checksum_path(path));
        if (!sum.ok()) return primary; // no checksum recorded — unprotected legacy file
        if (crypto::sha256(*primary).hex() == *sum) return primary;
        // Primary fails its checksum: fall to the redundant copy; the failing
        // primary is never silently returned (REQ §11).
    }
    auto bak = fs.read(backup_path(path));
    if (bak.ok())
    {
        auto bsum = fs.read(backup_path(path) + ".checksum");
        if (bsum.ok() && crypto::sha256(*bak).hex() == *bsum) return bak;
        if (!fs.exists(backup_path(path) + ".checksum")) return bak; // legacy backup
    }
    return Result<std::string>::fail(Error::make(Errc::Corrupt, "both primary and redundant copy failed checks", path));
}

} // namespace instr