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
 * @file lib/common/src/filesystem.cpp
 * @brief Implementation of the filesystem abstraction: the native POSIX backend and the simulated backend used for unit tests and fault injection (REQ §, §12).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/common/filesystem.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace instr
{

// ---------------- NativeFileSystem ----------------

/// @brief Test whether a path exists.
/// @param path Path to test.
/// @return True when it exists.
bool NativeFileSystem::exists(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

/// @brief Read a whole file.
/// @param path File to read.
/// @return File contents, or an Io/NotFound error.
Result<std::string> NativeFileSystem::read(const std::string& path)
{
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return Result<std::string>::fail(Errc::Io, "open failed: " + std::string(strerror(errno)), path);
    std::string out;
    char buf[65536];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, size_t(n));
    int saved = errno;
    ::close(fd);
    if (n < 0) return Result<std::string>::fail(Errc::Io, "read failed: " + std::string(strerror(saved)), path);
    return Result<std::string>::good(std::move(out));
}

/// @brief Plain write (not atomic); callers needing atomicity use AtomicFile.
/// @param path Destination file.
/// @param data Complete contents to write.
/// @param append Append instead of truncate when true.
/// @return ok(), or the underlying I/O error.
Error NativeFileSystem::write(const std::string& path, const std::string& data, bool append)
{
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) return Error::make(Errc::Io, "open failed: " + std::string(strerror(errno)), path);
    size_t off = 0;
    while (off < data.size())
    {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0)
        {
            int saved = errno;
            ::close(fd);
            return Error::make(Errc::Io, "write failed: " + std::string(strerror(saved)), path);
        }
        off += size_t(n);
    }
    ::fsync(fd);
    ::close(fd);
    return Error::none();
}

/// @brief Create a directory and any missing parents.
/// @param path Directory to create.
/// @return ok(), or the underlying I/O error.
Error NativeFileSystem::mkdirs(const std::string& path)
{
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i)
    {
        cur += path[i];
        if (path[i] == '/' && i > 0) { ::mkdir(cur.substr(0, i).c_str(), 0755); }
    }
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return Error::make(Errc::Io, "mkdir failed: " + std::string(strerror(errno)), path);
    return Error::none();
}

/// @brief Remove a file or directory (recursively when @p recursive).
/// @param path Path to remove.
/// @param recursive Remove directory contents when true.
/// @return ok(), or the underlying I/O error.
Error NativeFileSystem::remove(const std::string& path, bool recursive)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return Error::make(Errc::NotFound, "not found", path);
    if (S_ISDIR(st.st_mode))
    {
        if (!recursive) return Error::make(Errc::Busy, "directory not empty and recursive=false", path);
        DIR* d = ::opendir(path.c_str());
        if (!d) return Error::make(Errc::Io, "opendir failed", path);
        struct dirent* e;
        Error err = Error::none();
        while ((e = ::readdir(d)) != nullptr)
        {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            Error r = remove(path + "/" + name, true);
            if (!r.ok() && err.ok()) err = r;
        }
        ::closedir(d);
        if (!err.ok()) return err;
    }
    if (::unlink(path.c_str()) != 0 && ::rmdir(path.c_str()) != 0) return Error::make(Errc::Io, "unlink/rmdir failed: " + std::string(strerror(errno)), path);
    return Error::none();
}

/// @brief Rename/move a path within the same filesystem.
/// @param from Source path.
/// @param to Destination path.
/// @return ok(), or the underlying I/O error.
Error NativeFileSystem::rename(const std::string& from, const std::string& to)
{
    if (::rename(from.c_str(), to.c_str()) != 0) return Error::make(Errc::Io, "rename failed: " + std::string(strerror(errno)), from + " -> " + to);
    return Error::none();
}

/// @brief List entry names in a directory.
/// @param path Directory to list.
/// @return Entry names, or an Io/NotFound error.
Result<std::vector<std::string>> NativeFileSystem::list_dir(const std::string& path)
{
    DIR* d = ::opendir(path.c_str());
    if (!d) return Result<std::vector<std::string>>::fail(Errc::NotFound, "opendir failed", path);
    std::vector<std::string> out;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr)
    {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        out.push_back(name);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return Result<std::vector<std::string>>::good(std::move(out));
}

/// @brief Size of a file in bytes.
/// @param path File to measure.
/// @return Size in bytes, or an error.
Result<uint64_t> NativeFileSystem::file_size(const std::string& path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return Result<uint64_t>::fail(Errc::NotFound, "stat failed", path);
    return Result<uint64_t>::good(uint64_t(st.st_size));
}

/// @brief Free space on the filesystem containing @p path.
/// @param path Any path on the filesystem of interest.
/// @return Bytes available, or an error.
Result<uint64_t> NativeFileSystem::free_space(const std::string& path)
{
    struct statvfs v;
    if (::statvfs(path.c_str(), &v) != 0) return Result<uint64_t>::fail(Errc::Io, "statvfs failed", path);
    return Result<uint64_t>::good(uint64_t(v.f_bavail) * uint64_t(v.f_frsize));
}

/// @brief Flush a file or directory to stable storage.
/// @param path Path to flush.
/// @return ok(), or the underlying I/O error.
Error NativeFileSystem::fsync_path(const std::string& path)
{
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return Error::make(Errc::Io, "fsync open failed", path);
    ::fsync(fd);
    ::close(fd);
    return Error::none();
}

// ---------------- SimulatedFileSystem ----------------

/// @brief Internal helper deciding whether a key denotes a directory.
/// @param p Path to test.
bool SimulatedFileSystem::is_dir(const std::string& p) const
{
    return dirs_.count(p) > 0;
}

/// @brief Test whether a path exists.
/// @param path Path to test.
/// @return True when it exists.
bool SimulatedFileSystem::exists(const std::string& path)
{
    return files_.count(path) > 0 || is_dir(path);
}

/// @brief Read a whole file.
/// @param path File to read.
/// @return File contents, or an Io/NotFound error.
Result<std::string> SimulatedFileSystem::read(const std::string& path)
{
    auto it = files_.find(path);
    if (it == files_.end()) return Result<std::string>::fail(Errc::NotFound, "no such file", path);
    int n = ++read_count_[path];
    std::string data = it->second;
    auto cit = corrupt_.find(path);
    if (cit != corrupt_.end() && n >= cit->second)
    {
        // Simulate corruption: flip the first byte.
        if (!data.empty()) data[0] = char(uint8_t(data[0]) ^ 0xFF);
    }
    return Result<std::string>::good(std::move(data));
}

/// @brief Plain write (not atomic); callers needing atomicity use AtomicFile.
/// @param path Destination file.
/// @param data Complete contents to write.
/// @param append Append instead of truncate when true.
/// @return ok(), or the underlying I/O error.
Error SimulatedFileSystem::write(const std::string& path, const std::string& data, bool append)
{
    ++write_count_;
    if (fail_writes_after_ > 0 && write_count_ > fail_writes_after_) return Error::make(Errc::Io, "simulated write failure", path);
    if (append) { files_[path] += data; }
    else { files_[path] = data; }
    // All ancestor directories implicitly exist.
    size_t pos = path.find_last_of('/');
    while (pos != std::string::npos && pos > 0)
    {
        dirs_[path.substr(0, pos)] = true;
        std::string parent = path.substr(0, pos);
        pos = parent.find_last_of('/');
        if (pos == 0)
        {
            dirs_["/"] = true;
            break;
        }
    }
    mtimes_[path] = now_unix_s_;
    return Error::none();
}

/// @brief Create a directory and any missing parents.
/// @param path Directory to create.
/// @return ok(), or the underlying I/O error.
Error SimulatedFileSystem::mkdirs(const std::string& path)
{
    if (fail_mkdirs_) return Error::make(Errc::Io, "simulated mkdir failure", path);
    std::string cur;
    size_t start = 0;
    if (!path.empty() && path[0] == '/')
    {
        dirs_["/"] = true;
        start = 1;
    }
    for (size_t i = start; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == '/')
        {
            std::string part = path.substr(0, i);
            if (!part.empty()) dirs_[part] = true;
        }
    }
    return Error::none();
}

/// @brief Remove a file or directory (recursively when @p recursive).
/// @param path Path to remove.
/// @param recursive Remove directory contents when true.
/// @return ok(), or the underlying I/O error.
Error SimulatedFileSystem::remove(const std::string& path, bool recursive)
{
    if (files_.erase(path) > 0) return Error::none();
    if (is_dir(path))
    {
        if (!recursive)
        {
            // Only remove if empty.
            for (const auto& f : files_)
                if (f.first.rfind(path + "/", 0) == 0) return Error::make(Errc::Busy, "directory not empty", path);
            for (const auto& d : dirs_)
                if (d.first.rfind(path + "/", 0) == 0) return Error::make(Errc::Busy, "directory not empty", path);
        }
        else
        {
            for (auto it = files_.begin(); it != files_.end();)
            {
                if (it->first.rfind(path + "/", 0) == 0)
                    it = files_.erase(it);
                else
                    ++it;
            }
            for (auto it = dirs_.begin(); it != dirs_.end();)
            {
                if (it->first.rfind(path + "/", 0) == 0)
                    it = dirs_.erase(it);
                else
                    ++it;
            }
        }
        dirs_.erase(path);
        return Error::none();
    }
    return Error::make(Errc::NotFound, "not found", path);
}

/// @brief Rename/move a path within the same filesystem.
/// @param from Source path.
/// @param to Destination path.
/// @return ok(), or the underlying I/O error.
Error SimulatedFileSystem::rename(const std::string& from, const std::string& to)
{
    if (fail_renames_) return Error::make(Errc::Io, "simulated rename failure", from);
    auto fit = files_.find(from);
    if (fit != files_.end())
    {
        files_[to] = fit->second;
        files_.erase(fit);
        // A rename preserves the modification time on a real filesystem, and
        // log rotation relies on that: a rotated file must keep the age it had,
        // or retention-by-age would treat every rotation as fresh content.
        auto mit = mtimes_.find(from);
        if (mit != mtimes_.end())
        {
            mtimes_[to] = mit->second;
            mtimes_.erase(mit);
        }
        size_t pos = to.find_last_of('/');
        if (pos != std::string::npos && pos > 0) dirs_[to.substr(0, pos)] = true;
        return Error::none();
    }
    if (is_dir(from))
    {
        // Move all children.
        std::string prefix = from + "/";
        for (auto it = files_.begin(); it != files_.end();)
        {
            if (it->first.rfind(prefix, 0) == 0)
            {
                const std::string dest = to + "/" + it->first.substr(prefix.size());
                files_[dest] = it->second;
                auto mit = mtimes_.find(it->first);
                if (mit != mtimes_.end())
                {
                    mtimes_[dest] = mit->second;
                    mtimes_.erase(mit);
                }
                it = files_.erase(it);
            }
            else
                ++it;
        }
        for (auto it = dirs_.begin(); it != dirs_.end();)
        {
            if (it->first.rfind(prefix, 0) == 0)
            {
                dirs_[to + "/" + it->first.substr(prefix.size())] = true;
                it = dirs_.erase(it);
            }
            else
                ++it;
        }
        dirs_.erase(from);
        dirs_[to] = true;
        return Error::none();
    }
    return Error::make(Errc::NotFound, "rename source not found", from);
}

/// @brief List entry names in a directory.
/// @param path Directory to list.
/// @return Entry names, or an Io/NotFound error.
Result<std::vector<std::string>> SimulatedFileSystem::list_dir(const std::string& path)
{
    if (!is_dir(path)) return Result<std::vector<std::string>>::fail(Errc::NotFound, "not a directory", path);
    std::vector<std::string> out;
    std::string prefix = path == "/" ? "/" : path + "/";
    for (const auto& f : files_)
    {
        if (f.first.rfind(prefix, 0) == 0)
        {
            std::string rest = f.first.substr(prefix.size());
            if (rest.find('/') == std::string::npos) out.push_back(rest);
        }
    }
    for (const auto& d : dirs_)
    {
        if (d.first != path && d.first.rfind(prefix, 0) == 0)
        {
            std::string rest = d.first.substr(prefix.size());
            if (rest.find('/') == std::string::npos) out.push_back(rest);
        }
    }
    std::sort(out.begin(), out.end());
    return Result<std::vector<std::string>>::good(std::move(out));
}

/// @brief Size of a file in bytes.
/// @param path File to measure.
/// @return Size in bytes, or an error.
Result<uint64_t> SimulatedFileSystem::file_size(const std::string& path)
{
    auto it = files_.find(path);
    if (it == files_.end()) return Result<uint64_t>::fail(Errc::NotFound, "no such file", path);
    return Result<uint64_t>::good(it->second.size());
}

/// @brief Free space on the filesystem containing @p path.
/// @param path Any path on the filesystem of interest.
/// @return Bytes available, or an error.
Result<uint64_t> SimulatedFileSystem::free_space(const std::string&)
{
    return Result<uint64_t>::good(free_space_);
}

/// @brief Flush a file or directory to stable storage.
/// @param path Path to flush.
/// @return ok(), or the underlying I/O error.
Error SimulatedFileSystem::fsync_path(const std::string&)
{
    return Error::none();
}

/// @brief Total bytes stored across all files (test introspection).
uint64_t SimulatedFileSystem::used_bytes() const
{
    uint64_t total = 0;
    for (const auto& f : files_) total += f.second.size();
    return total;
}

} // namespace instr
namespace instr
{

// ---- file timestamps (age-based retention, §7.7) ----

/// @brief Last-modification time, unix seconds.
///
/// Age-based retention (§7.7) needs it, and a store that cannot tell how
/// old a file is cannot honour a retention policy expressed in days.
///
/// @param path File to query.
/// @return Mtime in unix seconds, or an error.
Result<int64_t> NativeFileSystem::file_mtime_s(const std::string& path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return Result<int64_t>::fail(Errc::NotFound, "cannot stat", path);
    return Result<int64_t>::good(int64_t(st.st_mtime));
}

/// @brief Last-modification time, unix seconds.
///
/// Age-based retention (§7.7) needs it, and a store that cannot tell how
/// old a file is cannot honour a retention policy expressed in days.
///
/// @param path File to query.
/// @return Mtime in unix seconds, or an error.
Result<int64_t> SimulatedFileSystem::file_mtime_s(const std::string& path)
{
    auto it = mtimes_.find(path);
    if (it == mtimes_.end())
    {
        if (files_.find(path) == files_.end()) return Result<int64_t>::fail(Errc::NotFound, "no such file", path);
        return Result<int64_t>::good(now_unix_s_);
    }
    return Result<int64_t>::good(it->second);
}

} // namespace instr
