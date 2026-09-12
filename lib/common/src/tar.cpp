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
 * @file lib/common/src/tar.cpp
 * @brief Implementation of the minimal POSIX ustar archive writer/reader used by backups and log bundles (REQ §.10).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/common/tar.h"

#include <cstring>

namespace instr
{
/// @brief Minimal POSIX ustar archive writer/reader (§7.10).
namespace tar
{

/// @brief Render an integer as zero-padded octal into a fixed-width header field.
/// @param out Header field buffer.
/// @param width Field width.
/// @param v Value to render.
static void octal(char* out, size_t width, uint64_t v)
{
    // width includes the terminating NUL; write octal right-aligned.
    std::memset(out, '0', width - 1);
    out[width - 1] = '\0';
    size_t i = width - 2;
    while (v > 0 && i > 0)
    {
        out[i] = char('0' + (v & 7));
        v >>= 3;
        --i;
    }
}

/// @brief Compute the ustar header checksum (checksum field treated as spaces).
/// @param h 512-byte header block.
/// @return The header checksum.
static uint32_t header_checksum(const char* h)
{
    uint32_t sum = 0;
    for (int i = 0; i < 512; ++i) sum += uint8_t(h[i]);
    return sum;
}

/// @brief Write a POSIX ustar archive (no external tar binary; testable and deterministic).
/// @param files Files to archive, in write order.
/// @return The archive bytes.
std::string write_ustar(const std::vector<FileEntry>& files)
{
    std::string out;
    for (const auto& f : files)
    {
        std::string name = f.name;
        if (name.size() > 100) return std::string(); // long names unsupported; callers keep paths short
        char h[512];
        std::memset(h, 0, sizeof(h));
        std::memcpy(h, name.c_str(), name.size());
        octal(h + 100, 8, f.mode);
        octal(h + 108, 8, 0); // uid
        octal(h + 116, 8, 0); // gid
        octal(h + 124, 12, f.data.size());
        octal(h + 136, 12, f.mtime);
        std::memset(h + 148, ' ', 8); // checksum field placeholder
        h[156] = '0'; // regular file
        std::memcpy(h + 257, "ustar\0", 6);
        h[263] = '0';
        h[264] = '0';
        uint32_t sum = header_checksum(h);
        char chk[9];
        // checksum is 6 octal digits + NUL + space
        octal(chk, 8, sum);
        std::memcpy(h + 148, chk, 7);
        h[155] = ' ';
        out.append(h, 512);
        out += f.data;
        size_t pad = (512 - (f.data.size() % 512)) % 512;
        out.append(pad, '\0');
    }
    // End-of-archive: two zero blocks.
    out.append(1024, '\0');
    return out;
}

/// @brief Read a ustar archive back into entries.
/// @param archive Archive bytes.
/// @return The entries, or a Corrupt error for a malformed archive.
Result<std::vector<FileEntry>> read_ustar(const std::string& archive)
{
    std::vector<FileEntry> out;
    size_t off = 0;
    while (off + 512 <= archive.size())
    {
        const char* h = archive.data() + off;
        bool all_zero = true;
        for (int i = 0; i < 512; ++i)
            if (h[i] != 0)
            {
                all_zero = false;
                break;
            }
        if (all_zero) break; // end marker
        // Validate checksum: stored value vs computed with checksum field as spaces.
        char chkbuf[8];
        std::memcpy(chkbuf, h + 148, 8);
        uint32_t stored = uint32_t(std::strtoul(chkbuf, nullptr, 8));
        std::string block(h, 512);
        char* mh = &block[0];
        std::memset(mh + 148, ' ', 8);
        if (header_checksum(mh) != stored) return Result<std::vector<FileEntry>>::fail(Errc::Corrupt, "tar header checksum mismatch", std::string(h, 100));
        uint64_t size = std::strtoul(std::string(h + 124, 12).c_str(), nullptr, 8);
        std::string name(h, strnlen(h, 100));
        if (name.empty()) break;
        off += 512;
        if (off + size > archive.size()) return Result<std::vector<FileEntry>>::fail(Errc::Corrupt, "truncated file data", name);
        FileEntry f;
        f.name = name;
        f.mode = uint32_t(std::strtoul(std::string(h + 100, 8).c_str(), nullptr, 8));
        f.mtime = std::strtoul(std::string(h + 136, 12).c_str(), nullptr, 8);
        f.data = archive.substr(off, size);
        off += size;
        off += (512 - (size % 512)) % 512;
        out.push_back(std::move(f));
    }
    return Result<std::vector<FileEntry>>::good(std::move(out));
}

} // namespace tar
} // namespace instr