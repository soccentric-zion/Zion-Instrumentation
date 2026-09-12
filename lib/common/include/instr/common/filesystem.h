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
 * @file lib/common/include/instr/common/filesystem.h
 * @brief filesystem abstraction (REQ §4, §12)
 *
 * Domain logic that must touch storage goes through this interface so every
 * component is unit-testable without a real filesystem and fault injection
 * (corrupt /pst, full disk, storage errors) is structural, not bolted on.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "instr/common/error.h"

namespace instr
{

/// @brief Storage interface so domain logic is unit-testable and fault injection is structural.
///
/// Domain logic that must touch storage goes through this interface; fault
/// injection (corrupt /pst, full disk, storage errors) is structural, not
/// bolted on.
class IFileSystem
{
  public:
    /// @brief Default destructor.
    /// @return Default destructor.
    virtual ~IFileSystem() = default;

    /// @brief Test whether a path exists.
    /// @param path Path to test.
    /// @return True when it exists.
    virtual bool exists(const std::string& path) = 0;
    /// @brief Read a whole file.
    /// @param path File to read.
    /// @return File contents, or an Io/NotFound error.
    virtual Result<std::string> read(const std::string& path) = 0;
    /// @brief Plain write (not atomic); callers needing atomicity use AtomicFile.
    /// @param path Destination file.
    /// @param data Complete contents to write.
    /// @param append Append instead of truncate when true.
    /// @return ok(), or the underlying I/O error.
    virtual Error write(const std::string& path, const std::string& data, bool append = false) = 0;
    /// @brief Create a directory and any missing parents.
    /// @param path Directory to create.
    /// @return ok(), or the underlying I/O error.
    virtual Error mkdirs(const std::string& path) = 0;
    /// @brief Remove a file or directory (recursively when @p recursive).
    /// @param path Path to remove.
    /// @param recursive Remove directory contents when true.
    /// @return ok(), or the underlying I/O error.
    virtual Error remove(const std::string& path, bool recursive = false) = 0;
    /// @brief Rename/move a path within the same filesystem.
    /// @param from Source path.
    /// @param to Destination path.
    /// @return ok(), or the underlying I/O error.
    virtual Error rename(const std::string& from, const std::string& to) = 0;
    /// @brief List entry names in a directory.
    /// @param path Directory to list.
    /// @return Entry names, or an Io/NotFound error.
    virtual Result<std::vector<std::string>> list_dir(const std::string& path) = 0;
    /// @brief Size of a file in bytes.
    /// @param path File to measure.
    /// @return Size in bytes, or an error.
    virtual Result<uint64_t> file_size(const std::string& path) = 0;
    /// @brief Last-modification time, unix seconds.
    ///
    /// Age-based retention (§7.7) needs it, and a store that cannot tell how
    /// old a file is cannot honour a retention policy expressed in days.
    ///
    /// @param path File to query.
    /// @return Mtime in unix seconds, or an error.
    virtual Result<int64_t> file_mtime_s(const std::string& path) = 0;
    /// @brief Free space on the filesystem containing @p path.
    /// @param path Any path on the filesystem of interest.
    /// @return Bytes available, or an error.
    virtual Result<uint64_t> free_space(const std::string& path) = 0;
    /// @brief Flush a file or directory to stable storage.
    /// @param path Path to flush.
    /// @return ok(), or the underlying I/O error.
    virtual Error fsync_path(const std::string& path) = 0;
    /// @brief Join two path components with a single separator.
    /// @param a Left component.
    /// @param b Right component.
    /// @return The joined path.
    virtual std::string join(const std::string& a, const std::string& b) const
    {
        return a.empty() ? b : (a.back() == '/' ? a + b : a + "/" + b);
    }
};

/// @brief POSIX implementation; the temp+fsync+rename discipline lives in atomic_file.h.
class NativeFileSystem : public IFileSystem
{
  public:
    /// @brief Test whether a path exists.
    /// @param path Path to test.
    /// @return True when it exists.
    bool exists(const std::string& path) override;
    /// @brief Read a whole file.
    /// @param path File to read.
    /// @return File contents, or an Io/NotFound error.
    Result<std::string> read(const std::string& path) override;
    /// @brief Plain write (not atomic); callers needing atomicity use AtomicFile.
    /// @param path Destination file.
    /// @param data Complete contents to write.
    /// @param append Append instead of truncate when true.
    /// @return ok(), or the underlying I/O error.
    Error write(const std::string& path, const std::string& data, bool append = false) override;
    /// @brief Create a directory and any missing parents.
    /// @param path Directory to create.
    /// @return ok(), or the underlying I/O error.
    Error mkdirs(const std::string& path) override;
    /// @brief Remove a file or directory (recursively when @p recursive).
    /// @param path Path to remove.
    /// @param recursive Remove directory contents when true.
    /// @return ok(), or the underlying I/O error.
    Error remove(const std::string& path, bool recursive) override;
    /// @brief Rename/move a path within the same filesystem.
    /// @param from Source path.
    /// @param to Destination path.
    /// @return ok(), or the underlying I/O error.
    Error rename(const std::string& from, const std::string& to) override;
    /// @brief List entry names in a directory.
    /// @param path Directory to list.
    /// @return Entry names, or an Io/NotFound error.
    Result<std::vector<std::string>> list_dir(const std::string& path) override;
    /// @brief Size of a file in bytes.
    /// @param path File to measure.
    /// @return Size in bytes, or an error.
    Result<uint64_t> file_size(const std::string& path) override;
    /// @brief Last-modification time, unix seconds.
    ///
    /// Age-based retention (§7.7) needs it, and a store that cannot tell how
    /// old a file is cannot honour a retention policy expressed in days.
    ///
    /// @param path File to query.
    /// @return Mtime in unix seconds, or an error.
    Result<int64_t> file_mtime_s(const std::string& path) override;
    /// @brief Free space on the filesystem containing @p path.
    /// @param path Any path on the filesystem of interest.
    /// @return Bytes available, or an error.
    Result<uint64_t> free_space(const std::string& path) override;
    /// @brief Flush a file or directory to stable storage.
    /// @param path Path to flush.
    /// @return ok(), or the underlying I/O error.
    Error fsync_path(const std::string& path) override;
};

/// @brief In-memory implementation with fault injectors (REQ §12: storage errors, corrupt reads, full disk, failed renames).
class SimulatedFileSystem : public IFileSystem
{
  public:
    /// @brief Test whether a path exists.
    /// @param path Path to test.
    /// @return True when it exists.
    bool exists(const std::string& path) override;
    /// @brief Read a whole file.
    /// @param path File to read.
    /// @return File contents, or an Io/NotFound error.
    Result<std::string> read(const std::string& path) override;
    /// @brief Plain write (not atomic); callers needing atomicity use AtomicFile.
    /// @param path Destination file.
    /// @param data Complete contents to write.
    /// @param append Append instead of truncate when true.
    /// @return ok(), or the underlying I/O error.
    Error write(const std::string& path, const std::string& data, bool append = false) override;
    /// @brief Create a directory and any missing parents.
    /// @param path Directory to create.
    /// @return ok(), or the underlying I/O error.
    Error mkdirs(const std::string& path) override;
    /// @brief Remove a file or directory (recursively when @p recursive).
    /// @param path Path to remove.
    /// @param recursive Remove directory contents when true.
    /// @return ok(), or the underlying I/O error.
    Error remove(const std::string& path, bool recursive) override;
    /// @brief Rename/move a path within the same filesystem.
    /// @param from Source path.
    /// @param to Destination path.
    /// @return ok(), or the underlying I/O error.
    Error rename(const std::string& from, const std::string& to) override;
    /// @brief List entry names in a directory.
    /// @param path Directory to list.
    /// @return Entry names, or an Io/NotFound error.
    Result<std::vector<std::string>> list_dir(const std::string& path) override;
    /// @brief Size of a file in bytes.
    /// @param path File to measure.
    /// @return Size in bytes, or an error.
    Result<uint64_t> file_size(const std::string& path) override;
    /// @brief Last-modification time, unix seconds.
    ///
    /// Age-based retention (§7.7) needs it, and a store that cannot tell how
    /// old a file is cannot honour a retention policy expressed in days.
    ///
    /// @param path File to query.
    /// @return Mtime in unix seconds, or an error.
    Result<int64_t> file_mtime_s(const std::string& path) override;
    /// @brief Free space on the filesystem containing @p path.
    /// @param path Any path on the filesystem of interest.
    /// @return Bytes available, or an error.
    Result<uint64_t> free_space(const std::string& path) override;
    /// @brief Flush a file or directory to stable storage.
    /// @param path Path to flush.
    /// @return ok(), or the underlying I/O error.
    Error fsync_path(const std::string& path) override;

    /// @brief Fail writes from the n-th write onwards (1-based; 0 disables).
    /// @param n Write number to fail from (1-based; 0 disables).
    void fail_writes_after(int n)
    {
        fail_writes_after_ = n;
    }
    /// @brief Corrupt data returned by reads of @p path from the n-th read onwards (simulates bit rot / torn sector).
    /// @param path Path whose reads are corrupted.
    /// @param from_read Read number to corrupt from (1-based).
    void corrupt_reads_of(const std::string& path, int from_read)
    {
        corrupt_[path] = from_read;
    }
    /// @brief Clamp reported free space (simulates a filling / full partition).
    /// @param bytes Free space to report.
    void set_free_space(uint64_t bytes)
    {
        free_space_ = bytes;
    }
    /// @brief Set the simulated clock; tests advance it to age files without waiting.
    /// @param t Simulated unix time in seconds.
    void set_now_unix_s(int64_t t)
    {
        now_unix_s_ = t;
    }
    /// @brief Current simulated unix time.
    int64_t now_unix_s() const
    {
        return now_unix_s_;
    }
    /// @brief Force the mtime of a file (ageing without waiting).
    /// @param path File to stamp.
    /// @param t Mtime in unix seconds.
    void set_file_mtime_s(const std::string& path, int64_t t)
    {
        mtimes_[path] = t;
    }
    /// @brief Make every rename fail (simulates cross-device or ro-remount errors).
    /// @param v True to fail every rename.
    void fail_renames(bool v)
    {
        fail_renames_ = v;
    }
    /// @brief Make every mkdirs fail (simulates read-only storage).
    /// @param v True to fail every mkdirs.
    void fail_mkdirs(bool v)
    {
        fail_mkdirs_ = v;
    }
    /// @brief Clear all active fault injections.
    void reset_injections()
    {
        fail_writes_after_ = 0;
        corrupt_.clear();
        fail_renames_ = false;
        fail_mkdirs_ = false;
    }

    /// @brief View of the raw file contents (test introspection).
    /// @return View of the raw file contents (test introspection).
    const std::map<std::string, std::string>& raw() const
    {
        return files_;
    }
    /// @brief Total bytes stored across all files (test introspection).
    /// @return Total bytes stored.
    uint64_t used_bytes() const;

  private:
    /// @brief Internal helper deciding whether a key denotes a directory.
    /// @param p Path to test.
    /// @return True when the path is a directory.
    bool is_dir(const std::string& p) const;
    std::map<std::string, std::string> files_; ///< Files only: path → contents.
    std::map<std::string, bool> dirs_; ///< Directories: path → recursive-creation flag.
    int fail_writes_after_ = 0; ///< Write-failure injection point (0 disables).
    int write_count_ = 0; ///< Writes performed since construction.
    std::map<std::string, int> corrupt_; ///< Read-corruption schedule per path.
    std::map<std::string, int> read_count_; ///< Reads performed per path.
    std::map<std::string, int64_t> mtimes_; ///< Forced mtimes per path.
    int64_t now_unix_s_ = 1'700'000'000; ///< Simulated unix clock, seconds.
    uint64_t free_space_ = 1ull << 40; ///< Reported free space, bytes.
    bool fail_renames_ = false; ///< Rename-failure injection.
    bool fail_mkdirs_ = false; ///< Mkdirs-failure injection.
};

} // namespace instr