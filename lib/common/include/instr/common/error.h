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
 * @file lib/common/include/instr/common/error.h
 * @brief common result/error model (REQ §4, §14)
 *
 * One result/error model across the suite; no exceptions across ABI
 * boundaries; every rejected input produces a diagnosable message.
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#pragma once
#include <string>
#include <utility>

namespace instr
{

/** @brief Error codes shared across the suite (one result/error model, REQ §4). */
enum class Errc
{
    Ok = 0, ///< Success; the zero value must stay the success value.
    InvalidArgument, ///< Caller-supplied input failed validation.
    NotFound, ///< Referenced entity (key, file, path) does not exist.
    Io, ///< Filesystem/socket I/O failure.
    Corrupt, ///< Parsed artefact is malformed (JSON, frame, spool entry).
    BudgetExceeded, ///< Size/rate/disk budget exceeded; request refused, not truncated.
    SignatureInvalid, ///< Signature or checksum verification failed.
    VersionMismatch, ///< Artefact version not understood by this build.
    Busy, ///< Transient contention (lock, spool, device); retry may succeed.
    Unavailable, ///< Backing service or device not reachable.
    NotSupported, ///< Operation valid in the model but not on this platform/build.
    Internal, ///< Unexpected internal error; always a bug or a broken environment.
};

/// @brief Stable lowercase, hyphenated name of an error code for logs and ctrl output.
/// @param c Error code to name.
/// @return Name such as "invalid-argument"; "unknown" if @p c is out of range.
inline const char* errc_name(Errc c)
{
    switch (c)
    {
    case Errc::Ok: return "ok";
    case Errc::InvalidArgument: return "invalid-argument";
    case Errc::NotFound: return "not-found";
    case Errc::Io: return "io";
    case Errc::Corrupt: return "corrupt";
    case Errc::BudgetExceeded: return "budget-exceeded";
    case Errc::SignatureInvalid: return "signature-invalid";
    case Errc::VersionMismatch: return "version-mismatch";
    case Errc::Busy: return "busy";
    case Errc::Unavailable: return "unavailable";
    case Errc::NotSupported: return "not-supported";
    case Errc::Internal: return "internal";
    }
    return "unknown";
}

/// @brief A diagnosable failure: category code, message and origin context.
struct Error
{
    Errc code = Errc::Ok; ///< Failure category; Errc::Ok means success.
    std::string message; ///< Human-readable, diagnosable description.
    std::string context; ///< File/line/operation that produced it.

    /// @brief True when this represents success rather than a failure.
    /// @return True if @c code is Errc::Ok.
    bool ok() const
    {
        return code == Errc::Ok;
    }
    /// @brief Render as a single log-friendly line: "name: message (context)".
    /// @return The rendered string; the context part is omitted when empty.
    std::string str() const
    {
        std::string s = std::string(errc_name(code)) + ": " + message;
        if (!context.empty()) s += " (" + context + ")";
        return s;
    }
    /// @brief Construct a success (no-failure) value.
    /// @return Error with code Errc::Ok and empty message/context.
    static Error none()
    {
        return Error{};
    }
    /// @brief Construct a failure value.
    /// @param c Failure category.
    /// @param msg Human-readable description of what failed.
    /// @param ctx Optional origin context (file/line/operation).
    /// @return The populated Error.
    static Error make(Errc c, std::string msg, std::string ctx = "")
    {
        Error e;
        e.code = c;
        e.message = std::move(msg);
        e.context = std::move(ctx);
        return e;
    }
};

/// @brief Value-or-error return type used at every internal boundary (no exceptions).
/// @tparam T Payload type carried on success.
template <typename T> struct Result
{
    T value{}; ///< Payload; valid only when ok() is true.
    Error error; ///< Failure details; ok() when success.

    /// @brief True when the operation succeeded.
    /// @return True if the embedded Error is ok().
    bool ok() const
    {
        return error.ok();
    }
    /// @brief Access the payload (const overload); only valid when ok().
    const T& operator*() const
    {
        return value;
    }
    /// @brief Access the payload; only valid when ok().
    T& operator*()
    {
        return value;
    }
    /// @brief Member access to the payload (const overload); only valid when ok().
    const T* operator->() const
    {
        return &value;
    }
    /// @brief Member access to the payload; only valid when ok().
    T* operator->()
    {
        return &value;
    }

    /// @brief Construct a success result carrying @p v.
    /// @param v Payload to wrap.
    /// @return Result with empty error.
    static Result good(T v)
    {
        Result r;
        r.value = std::move(v);
        return r;
    }
/// @brief Construct a failure Status from parts.
/// @param c Failure category.
/// @param msg Human-readable description.
/// @param ctx Optional origin context.
/// @param e Failure details.
/// @return Status whose error() is built from the given parts.
/// @return The failure Status.
    static Result fail(Error e)
    {
        Result r;
        r.error = std::move(e);
        return r;
    }
    /// @brief Construct a failure result from parts.
    /// @param c Failure category.
    /// @param msg Human-readable description.
    /// @param ctx Optional origin context.
    /// @return Result whose error() is built from @p c/@p msg/@p ctx.
    static Result fail(Errc c, std::string msg, std::string ctx = "")
    {
        return fail(Error::make(c, std::move(msg), std::move(ctx)));
    }
};

/// @brief Placeholder payload for void-like success; makes Result usable without a value type.
struct Unit
{
};
/// @brief Status-only result type (success/failure, no payload).
using Status = Result<Unit>;

/// @brief Construct a success Status.
/// @return Status with an empty error.
inline Status good()
{
    return Status::good(Unit{});
}
/// @brief Construct a failure Status from parts.
/// @param c Failure category.
/// @param msg Human-readable description.
/// @param ctx Optional origin context.
/// @param e Failure details.
/// @return Status whose error() is built from the given parts.
/// @return The failure Result.
inline Status fail(Error e)
{
    return Status::fail(std::move(e));
}
/// @brief Construct a failure Status from parts.
/// @param c Failure category.
/// @param msg Human-readable description.
/// @param ctx Optional origin context.
/// @return Status whose error() is built from the given parts.
inline Status fail(Errc c, std::string msg, std::string ctx = "")
{
    return Status::fail(Error::make(c, std::move(msg), std::move(ctx)));
}

} // namespace instr