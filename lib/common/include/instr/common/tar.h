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
 * @file lib/common/include/instr/common/tar.h
 * @brief minimal POSIX ustar archive writer/reader
 *
 * Used by Backup/Restore and LogManager bundles: tar the files into an
 * archive; restore by untarring (REQ §7.10). No external tar binary — the
 * format is implemented here so it is testable and deterministic.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "instr/common/error.h"

namespace instr
{
/// @brief Minimal POSIX ustar archive writer/reader (§7.10).
namespace tar
{

/// @brief One file in a ustar archive.
struct FileEntry
{
    std::string name; ///< Relative path inside the archive.
    std::string data; ///< File contents.
    uint32_t mode = 0644; ///< Permission bits.
    uint64_t mtime = 0; ///< Modification time, unix seconds.
};

/// @brief Write a POSIX ustar archive (no external tar binary; testable and deterministic).
/// @param files Files to archive, in write order.
/// @return The archive bytes.
std::string write_ustar(const std::vector<FileEntry>& files);

/// @brief Read a ustar archive back into entries.
/// @param archive Archive bytes.
/// @return The entries, or a Corrupt error for a malformed archive.
Result<std::vector<FileEntry>> read_ustar(const std::string& archive);

} // namespace tar
} // namespace instr