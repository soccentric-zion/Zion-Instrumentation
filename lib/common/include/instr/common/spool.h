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
 * @file lib/common/include/instr/common/spool.h
 * @brief bounded, disk-backed spool with explicit drop
 *
 * policy (REQ §4, §7.1, §11). Nothing northbound depends on the tunnel being
 * available at the moment it is produced.
 * 
 * Drop policy when the spool is full: oldest telemetry first, then log refs,
 * then timeline events; alarms and faults are NEVER dropped. If only
 * protected records remain and the spool is still full, the append is
 * refused with BudgetExceeded so the caller can raise the alarm.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "instr/common/error.h"
#include "instr/common/filesystem.h"

namespace instr
{

/// @brief Eviction class for a spool record; lower values drop first.
enum class SpoolPriority : uint8_t
{
    Telemetry = 0, ///< Dropped first.
    LogRef = 1, ///< Dropped second.
    Event = 2, ///< Dropped third.
    Fault = 3, ///< Protected; never dropped.
    Alarm = 4, ///< Protected; never dropped.
};

/// @brief Sizing and location parameters for a DiskSpool.
struct SpoolConfig
{
    std::string dir; ///< Directory holding segment files.
    uint64_t max_bytes = 64 * 1024 * 1024; ///< Total size budget across segments.
    uint64_t segment_bytes = 4 * 1024 * 1024; ///< Per-segment size cap.
};

/// @brief Bounded, disk-backed spool with an explicit drop policy (REQ §4, §7.1, §11).
///
/// Drop policy when full: oldest telemetry first, then log refs, then
/// timeline events; alarms and faults are NEVER dropped. If only protected
/// records remain and the spool is still full, the append is refused with
/// BudgetExceeded.
class DiskSpool
{
  public:
    /// @brief Bind a spool to a filesystem and configuration.
    /// @param fs Storage backend (real or simulated for tests).
    /// @param cfg Directory, budget and segment sizing.
    DiskSpool(IFileSystem& fs, SpoolConfig cfg) : fs_(fs), cfg_(std::move(cfg))
    {
    }

    /// @brief Open/create the spool directory and rebuild the index from segment files (survives restart).
    /// @return ok(), or the open error.
    Error open();

    /// @brief One stored record as returned by peek().
    struct Entry
    {
        uint64_t seq = 0; ///< Process-wide arrival sequence.
        SpoolPriority priority = SpoolPriority::Telemetry; ///< Eviction class.
        uint64_t mono_ns = 0; ///< Monotonic production time.
        std::string payload; ///< Opaque record bytes.
    };

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
    Error append(SpoolPriority prio, const std::string& payload, uint64_t mono_ns, int* dropped = nullptr);

    /// @brief Oldest-first snapshot of up to @p max records (non-destructive).
    /// @param max Maximum entries returned.
    /// @return The snapshot, oldest first.
    std::vector<Entry> peek(size_t max) const;

    /// @brief Consume the oldest @p n records (after a successful drain/northbound send).
    /// @param n Number of records to consume.
    /// @return ok(), or an error when storage fails.
    Error pop_front(size_t n);

    /// @brief Lifetime counters, including per-priority drop counts.
    struct Stats
    {
        uint64_t count = 0; ///< Records currently in the spool.
        uint64_t bytes = 0; ///< Bytes currently stored.
        uint64_t dropped_telemetry = 0; ///< Lifetime telemetry evictions.
        uint64_t dropped_logref = 0; ///< Lifetime log-ref evictions.
        uint64_t dropped_event = 0; ///< Lifetime event evictions.
        uint64_t refused_protected = 0; ///< Appends refused because only protected records fit.
    };
    /// @brief Current statistics snapshot.
    /// @return Current statistics snapshot.
    const Stats& stats() const
    {
        return stats_;
    }

    /// @brief Number of records currently indexed.
    size_t size() const
    {
        return index_.size();
    }
    /// @brief Bytes currently stored across segments.
    uint64_t bytes() const
    {
        return bytes_;
    }

  private:
    /// @brief On-disk record header used while rebuilding the index.
    struct Record
    {
        SpoolPriority prio; ///< Eviction class.
        uint64_t mono_ns; ///< Monotonic production time.
        uint32_t len; ///< Payload length in bytes.
    };
    /// @brief Start a new segment file (rolls when segment_bytes is reached).
    /// @return ok(), or the write error.
    Error append_segment();
    /// @brief Rescan segment files and rebuild the in-memory index.
    /// @return ok(), or the read error.
    Error load_index();
    /// @brief Evict the oldest droppable record (never protected classes).
    /// @return ok() when one was dropped; an error when nothing was droppable.
    Error drop_one_oldest_droppable();
    /// @brief Remove index entry @p victim and its bytes from segment storage.
    /// @param victim Index position to erase.
    /// @return ok(), or an error when storage fails.
    Error erase_index_record(size_t victim);
    /// @brief Segment file path for a segment sequence number.
    /// @param seg_seq Segment sequence number.
    /// @return The segment file path.
    std::string segment_path(uint64_t seg_seq) const
    {
        return fs_.join(cfg_.dir, "spool-" + std::to_string(seg_seq) + ".seg");
    }

    IFileSystem& fs_; ///< Storage backend.
    SpoolConfig cfg_; ///< Location and sizing.
    /// @brief One segment file's bookkeeping.
    struct Seg
    {
        uint64_t seq; ///< Segment sequence number.
        uint64_t bytes; ///< Bytes stored in this segment.
        uint64_t records; ///< Records stored in this segment.
    };
    std::deque<Seg> segments_; ///< Live segments, oldest first.
    /// @brief In-memory index: record header + segment seq + offset.
    struct IdxRec
    {
        SpoolPriority prio; ///< Eviction class.
        uint64_t mono_ns; ///< Monotonic production time.
        uint32_t len; ///< Payload length.
        uint64_t seg; ///< Owning segment sequence.
        uint64_t off; ///< Byte offset within the segment.
    };
    std::deque<IdxRec> index_; ///< Oldest-first record index.
    uint64_t next_seg_seq_ = 1; ///< Sequence for the next segment file.
    uint64_t next_seq_ = 1; ///< Sequence for the next record.
    uint64_t bytes_ = 0; ///< Total bytes stored.
    uint64_t seg_offset_ = 0; ///< Write offset in the current segment.
    Stats stats_; ///< Lifetime statistics.
};

} // namespace instr