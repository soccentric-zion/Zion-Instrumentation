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
 * @file lib/common/src/spool.cpp
 * @brief Implementation of the bounded, disk-backed northbound spool: CRC'd record framing, append/replay, and the priority drop policy (REQ §, §.1, §11).
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include "instr/common/spool.h"

#include <cstdlib>
#include <cstring>
#include <map>

namespace instr
{

// Record wire format (little-endian):
//   magic u32 = 0x53504F4C ('SPOL') | crc32 u32 (of prio..payload) | prio u8
//   | reserved u8[3] | mono u64 | len u32 | payload
static constexpr uint32_t kMagic = 0x53504F4C;
static constexpr size_t kHeaderSize = 4 + 4 + 1 + 3 + 8 + 4; // 24

/// @brief CRC-32 integrity trailer for spool records.
/// @param data Bytes to checksum.
/// @return The CRC-32 value.
static uint32_t crc32(const std::string& data)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : data) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/// @brief Write a 4-byte little-endian unsigned integer onto a string buffer.
/// @param s Buffer to append to.
/// @param v Value to serialise.
static void put_u32(std::string& s, uint32_t v)
{
    for (int i = 0; i < 4; ++i) s += char((v >> (8 * i)) & 0xFF);
}
/// @brief Append a little-endian u64 to a buffer.
/// @param s Buffer to append to.
/// @param v Value to serialise.
static void put_u64(std::string& s, uint64_t v)
{
    for (int i = 0; i < 8; ++i) s += char((v >> (8 * i)) & 0xFF);
}
/// @brief Read a little-endian u32 from a buffer.
/// @param s Buffer to read from.
/// @param off Byte offset.
/// @return The u32 value.
static uint32_t get_u32(const std::string& s, size_t off)
{
    return uint32_t(uint8_t(s[off])) | (uint32_t(uint8_t(s[off + 1])) << 8) | (uint32_t(uint8_t(s[off + 2])) << 16) | (uint32_t(uint8_t(s[off + 3])) << 24);
}
/// @brief Read a little-endian u64 from a buffer.
/// @param s Buffer to read from.
/// @param off Byte offset.
/// @return The u64 value.
static uint64_t get_u64(const std::string& s, size_t off)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | uint8_t(s[off + i]);
    return v;
}

/// @brief Serialise one spool record: header + payload + CRC.
/// @param prio Eviction class.
/// @param mono Monotonic production time.
/// @param payload Record bytes.
/// @return The encoded record bytes.
static std::string encode_record(SpoolPriority prio, uint64_t mono, const std::string& payload)
{
    std::string body;
    body += char(prio);
    body += '\0';
    body += '\0';
    body += '\0';
    put_u64(body, mono);
    put_u32(body, uint32_t(payload.size()));
    std::string rec;
    put_u32(rec, kMagic);
    put_u32(rec, crc32(body + payload));
    rec += body;
    rec += payload;
    return rec;
}

// Decode one record at `off`; on success sets *out, advances off past it.
/// @brief Parse one record at @p off; advances @p off on success.
/// @param seg Segment bytes.
/// @param off Parse offset; advanced on success.
/// @param out Receives the parsed entry.
/// @return True when a record was parsed.
static bool decode_record(const std::string& seg, size_t& off, DiskSpool::Entry* out)
{
    if (off + kHeaderSize > seg.size()) return false;
    if (get_u32(seg, off) != kMagic) return false;
    uint32_t crc = get_u32(seg, off + 4);
    SpoolPriority prio = SpoolPriority(uint8_t(seg[off + 8]));
    uint64_t mono = get_u64(seg, off + 12);
    uint32_t len = get_u32(seg, off + 20);
    if (off + kHeaderSize + len > seg.size()) return false;
    std::string body = seg.substr(off + 8, 16);
    std::string payload = seg.substr(off + kHeaderSize, len);
    if (crc32(body + payload) != crc) return false;
    out->priority = prio;
    out->mono_ns = mono;
    out->payload = std::move(payload);
    off += kHeaderSize + len;
    return true;
}

/// @brief Open/create the spool directory and rebuild the index from segment files (survives restart).
Error DiskSpool::open()
{
    index_.clear();
    segments_.clear();
    bytes_ = 0;
    seg_offset_ = 0;
    next_seg_seq_ = 1;
    Error e = fs_.mkdirs(cfg_.dir);
    if (!e.ok()) return e;
    auto files = fs_.list_dir(cfg_.dir);
    if (!files.ok()) return files.error;
    for (const auto& name : *files)
    {
        if (name.rfind("spool-", 0) != 0) continue;
        if (name.size() < 8 || name.compare(name.size() - 4, 4, ".seg") != 0) continue;
        uint64_t seq = std::strtoull(name.c_str() + 6, nullptr, 10);
        if (seq >= next_seg_seq_) next_seg_seq_ = seq + 1;
        auto content = fs_.read(segment_path(seq));
        if (!content.ok()) continue;
        size_t off = 0;
        uint64_t records = 0;
        while (off < content->size())
        {
            size_t start = off;
            Entry ent;
            if (!decode_record(*content, off, &ent))
            {
                off = start + 1; // skip torn byte from an unclean shutdown tail
                continue;
            }
            IdxRec r;
            r.prio = ent.priority;
            r.mono_ns = ent.mono_ns;
            r.len = uint32_t(ent.payload.size());
            r.seg = seq;
            r.off = start;
            index_.push_back(r);
            bytes_ += off - start;
            ++records;
        }
        segments_.push_back(Seg{seq, uint64_t(content->size()), records});
        seg_offset_ = content->size();
    }
    // Active segment: reuse the last if under the segment size, else new one.
    if (!segments_.empty() && segments_.back().bytes < cfg_.segment_bytes)
    {
        // seg_offset_ already points at the tail of the last segment.
    }
    else
    {
        Error ae = append_segment();
        if (!ae.ok()) return ae;
    }
    return Error::none();
}

/// @brief Start a new segment file (rolls when segment_bytes is reached).
Error DiskSpool::append_segment()
{
    Error e = fs_.write(segment_path(next_seg_seq_), "");
    if (!e.ok()) return e;
    segments_.push_back(Seg{next_seg_seq_, 0, 0});
    seg_offset_ = 0;
    ++next_seg_seq_;
    return Error::none();
}

// Removes the record at `victim` (index into index_) by rewriting its
// segment and fixing offsets of the remaining records in that segment.
/// @brief Remove index entry @p victim and its bytes from segment storage.
/// @param victim Index position to erase.
/// @return ok(), or an error when storage fails.
Error DiskSpool::erase_index_record(size_t victim)
{
    IdxRec rec = index_[victim];
    auto content = fs_.read(segment_path(rec.seg));
    if (!content.ok()) return content.error;
    // Find the end of the victim record by scanning from its offset.
    Entry ent;
    size_t off = rec.off;
    if (!decode_record(*content, off, &ent)) return Error::make(Errc::Corrupt, "victim record undecodable", "spool segment");
    size_t rec_end = off;
    size_t removed = rec_end - rec.off;
    std::string rebuilt = content->substr(0, rec.off) + content->substr(rec_end);
    Error e = fs_.write(segment_path(rec.seg), rebuilt);
    if (!e.ok()) return e;
    for (auto& r : index_)
    {
        if (r.seg == rec.seg && r.off > rec.off) r.off -= removed;
    }
    bytes_ -= removed;
    for (auto& s : segments_)
        if (s.seq == rec.seg)
        {
            s.bytes -= removed;
            --s.records;
            break;
        }
    if (rec.seg == segments_.back().seq) seg_offset_ -= removed;
    index_.erase(index_.begin() + long(victim));
    return Error::none();
}

/// @brief Evict the oldest droppable record (never protected classes).
Error DiskSpool::drop_one_oldest_droppable()
{
    for (size_t i = 0; i < index_.size(); ++i)
    {
        SpoolPriority p = index_[i].prio;
        if (p == SpoolPriority::Telemetry || p == SpoolPriority::LogRef || p == SpoolPriority::Event)
        {
            switch (p)
            {
            case SpoolPriority::Telemetry: ++stats_.dropped_telemetry; break;
            case SpoolPriority::LogRef: ++stats_.dropped_logref; break;
            default: ++stats_.dropped_event; break;
            case SpoolPriority::Fault:
            case SpoolPriority::Alarm: break;
            }
            return erase_index_record(i);
        }
    }
    return Error::make(Errc::NotFound, "no droppable record", "spool");
}

/// @brief Append one record, enforcing the size budget with the drop policy.
///
/// On success, *@p dropped (if non-null) reports how many older
/// low-priority records were evicted to make room — drops are reported,
/// never silent.
///
/// @param prio Eviction class of the record.
/// @param payload Record bytes.
/// @param mono_ns Monotonic production time.
/// @param dropped Optional out-count of evicted records.
/// @return ok() on success; BudgetExceeded when only protected records fit.
Error DiskSpool::append(SpoolPriority prio, const std::string& payload, uint64_t mono_ns, int* dropped)
{
    std::string rec = encode_record(prio, mono_ns, payload);
    uint64_t rec_size = rec.size();

    if (segments_.empty())
    {
        Error e = append_segment();
        if (!e.ok()) return e;
    }
    if (seg_offset_ + rec_size > cfg_.segment_bytes)
    {
        Error e = append_segment();
        if (!e.ok()) return e;
    }
    // Enforce the budget with the drop policy: drop until it fits or nothing
    // droppable remains.
    while (bytes_ + rec_size > cfg_.max_bytes)
    {
        Error e = drop_one_oldest_droppable();
        if (!e.ok())
        {
            if (prio == SpoolPriority::Alarm || prio == SpoolPriority::Fault) ++stats_.refused_protected;
            return Error::make(Errc::BudgetExceeded, "spool full and only protected records remain", "DiskSpool::append");
        }
    }
    Seg& active = segments_.back();
    auto content = fs_.read(segment_path(active.seq));
    if (!content.ok()) content = Result<std::string>::good(std::string());
    std::string updated = *content + rec;
    Error e = fs_.write(segment_path(active.seq), updated);
    if (!e.ok()) return e;
    IdxRec r;
    r.prio = prio;
    r.mono_ns = mono_ns;
    r.len = uint32_t(payload.size());
    r.seg = active.seq;
    r.off = seg_offset_;
    index_.push_back(r);
    bytes_ += rec_size;
    seg_offset_ += rec_size;
    active.bytes += rec_size;
    ++active.records;
    ++stats_.count;
    if (dropped) *dropped = 0;
    return Error::none();
}

/// @brief Oldest-first snapshot of up to @p max records (non-destructive).
/// @param max Maximum entries returned.
/// @return The snapshot, oldest first.
std::vector<DiskSpool::Entry> DiskSpool::peek(size_t max) const
{
    std::vector<Entry> out;
    std::map<uint64_t, std::string> cache; // avoid re-reading segments
    size_t n = 0;
    for (const auto& r : index_)
    {
        if (n >= max) break;
        if (!cache.count(r.seg))
        {
            auto content = fs_.read(segment_path(r.seg));
            if (!content.ok()) continue;
            cache[r.seg] = *content;
        }
        Entry ent;
        size_t off = r.off;
        if (!decode_record(cache[r.seg], off, &ent)) continue;
        out.push_back(ent);
        ++n;
    }
    return out;
}

/// @brief Consume the oldest @p n records (after a successful drain/northbound send).
/// @param n Number of records to consume.
/// @return ok(), or an error when storage fails.
Error DiskSpool::pop_front(size_t n)
{
    if (n > index_.size()) n = index_.size();
    std::map<uint64_t, bool> affected;
    for (size_t i = 0; i < n; ++i) affected[index_[i].seg] = true;
    index_.erase(index_.begin(), index_.begin() + long(n));
    stats_.count -= n;
    for (auto kv : affected)
    {
        uint64_t seg = kv.first;
        std::vector<IdxRec> keep;
        for (const auto& r : index_)
            if (r.seg == seg) keep.push_back(r);
        if (keep.empty())
        {
            Error e = fs_.remove(segment_path(seg), false);
            if (!e.ok()) return e;
            for (auto it = segments_.begin(); it != segments_.end(); ++it)
                if (it->seq == seg)
                {
                    segments_.erase(it);
                    break;
                }
            continue;
        }
        auto content = fs_.read(segment_path(seg));
        if (!content.ok()) return content.error;
        std::map<uint64_t, bool> keep_off;
        for (const auto& r : keep) keep_off[r.off] = true;
        std::string rebuilt;
        std::map<uint64_t, uint64_t> remap;
        size_t off = 0;
        while (off < content->size())
        {
            size_t start = off;
            Entry ent;
            if (!decode_record(*content, off, &ent))
            {
                off = start + 1;
                continue;
            }
            if (keep_off.count(start))
            {
                remap[start] = rebuilt.size();
                rebuilt += content->substr(start, off - start);
            }
        }
        Error e = fs_.write(segment_path(seg), rebuilt);
        if (!e.ok()) return e;
        for (auto& r : index_)
            if (r.seg == seg) r.off = remap[r.off];
        for (auto it = segments_.begin(); it != segments_.end(); ++it)
            if (it->seq == seg)
            {
                it->bytes = rebuilt.size();
                it->records = keep.size();
                break;
            }
    }
    return Error::none();
}

} // namespace instr