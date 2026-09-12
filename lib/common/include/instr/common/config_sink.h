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
 * @file lib/common/include/instr/common/config_sink.h
 * @brief Spoke-side receiver for configuration delivered by istate.
 *
 * R4: istate is the sole reader and writer of /pst. A spoke never opens its
 * own config.yaml — it asks istate on registration and is pushed a new
 * document whenever the configuration changes (§7, §9).
 *
 * Every delivery is validated against the daemon's JSON Schema before it
 * reaches the daemon: a typo is a loud rejection, never a silent behavioural
 * change. On rejection the previously accepted document stays in force and
 * the daemon keeps running (R10) — the reason travels back to istate, which
 * reports it northbound.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <functional>
#include <mutex>
#include <string>

#include "instr/common/config.h"
#include "instr/common/json.h"

namespace instr
{

/// @brief Spoke-side receiver for configuration delivered by istate (R4, §7, §9).
///
/// Every delivery is validated against the daemon's JSON Schema before it
/// reaches the daemon. On rejection the previously accepted document stays in
/// force and the daemon keeps running (R10).
class ConfigSink
{
  public:
    /// @brief Applies an accepted document to the daemon's domain objects.
    ///
    /// Returning a non-ok Error rejects the delivery: the sink rolls back to
    /// the previous document so a bad push cannot leave a daemon half-configured.
    using Apply = std::function<Error(const ConfigDocument&)>;

    /// @brief Construct a sink for one daemon.
    /// @param daemon Daemon name (used in diagnostics).
    /// @param schema JSON Schema deliveries must satisfy.
    /// @param apply Callback applying an accepted document to the domain.
    ConfigSink(std::string daemon, JValue schema, Apply apply) : daemon_(std::move(daemon)), schema_(std::move(schema)), apply_(std::move(apply))
    {
    }

    /// @brief Deliver a candidate document.
    ///
    /// Never throws; never leaves a partial config.
    ///
    /// @param yaml Candidate YAML text.
    /// @param revision istate's monotonically increasing revision for this delivery.
    /// @return "" when accepted and applied, otherwise a diagnosable reason.
    std::string deliver(const std::string& yaml, uint64_t revision)
    {
        auto parsed = ConfigDocument::parse(yaml, schema_, daemon_ + " config (revision " + std::to_string(revision) + ")");
        if (!parsed.ok()) return parsed.error.str();
        std::lock_guard<std::mutex> lk(mu_);
        if (apply_)
        {
            Error e = apply_(*parsed);
            if (!e.ok())
            {
                // The daemon refused the values themselves (out of range, mutually
                // inconsistent). Re-apply what we had so the daemon is never left
                // running on a half-written document.
                if (revision_ > 0) (void)apply_(current_);
                return e.str();
            }
        }
        current_ = *parsed;
        revision_ = revision;
        return "";
    }

    /// @brief Copy of the currently accepted document (empty until configured).
    ConfigDocument current() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return current_;
    }
    /// @brief Revision of the currently accepted document; 0 before any delivery.
    uint64_t revision() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return revision_;
    }
    /// @brief True once a delivery has been accepted.
    ///
    /// Until then the daemon runs on its compiled defaults — a virgin /pst
    /// must not stop it starting.
    bool configured() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return revision_ > 0;
    }
/// @brief The daemon identity this hub accepts registrations from.
/// @return The owning daemon name.
    const std::string& daemon() const
    {
        return daemon_;
    }

  private:
    mutable std::mutex mu_; ///< Serialises deliver()/current()/revision() across threads.
    std::string daemon_; ///< Owning daemon name for diagnostics.
    JValue schema_; ///< Schema every delivery is validated against.
    Apply apply_; ///< Domain-apply callback; a non-ok result rejects the delivery.
    ConfigDocument current_; ///< Last accepted document (rolled back to on rejection).
    uint64_t revision_ = 0; ///< Revision of the last accepted delivery.
};

/// @brief Loads a daemon's JSON Schema from the image (installed into ${datadir}/instrumentation/schemas).
///
/// A missing schema is not fatal: the document is then accepted unvalidated
/// and the caller warns, because a packaging slip must not stop the device
/// booting (R10).
///
/// @param fs Filesystem abstraction for the load.
/// @param sysroot Root to resolve the schema directory under.
/// @param daemon Daemon name whose schema is wanted.
/// @return Parsed schema, or an Error (typically NotFound) when unavailable.
Result<JValue> load_daemon_schema(class IFileSystem& fs, const std::string& sysroot, const std::string& daemon);

} // namespace instr
