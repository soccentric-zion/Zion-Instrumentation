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
 * @file lib/common/include/instr/common/command_table.h
 * @brief the shared command table (REQ §3.2)
 *
 * Each daemon defines its operations once, in a single command table
 * (name, description, argument spec, handler). Both the gRPC service
 * implementation and the ctrl server dispatch from that table, which makes
 * _ctrl coverage structural rather than hand-maintained, and gives help and
 * shell completion for free.
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

#include "instr/common/error.h"
#include "instr/common/json.h"

namespace instr
{

/// @brief Outcome of a ctrl command invocation.
struct CommandResult
{
    bool ok = true; ///< True on success.
    std::string error_code; ///< Errc name when failed ("invalid-argument", ...).
    std::string error_message; ///< Diagnosable failure text.
    std::string json_data; ///< JSON object/string payload on success.

    /// @brief Construct a success result carrying a JSON payload.
    /// @param json JSON payload text.
    /// @return The success result.
    static CommandResult good(std::string json)
    {
        CommandResult r;
        r.json_data = std::move(json);
        return r;
    }
    /// @brief Construct a success result carrying a serialised JValue.
    /// @param v Value to serialise.
    /// @return The success result.
    static CommandResult good(JValue v)
    {
        return good(v.dump());
    }
    /// @brief Construct a failure result from an Errc code and message.
    /// @param c Failure category.
    /// @param msg Diagnosable failure text.
    /// @return The failure result.
    static CommandResult err(Errc c, const std::string& msg)
    {
        CommandResult r;
        r.ok = false;
        r.error_code = errc_name(c);
        r.error_message = msg;
        return r;
    }
    /// @brief Construct a failure result (extra context argument kept for ABI compatibility).
    /// @param c Failure category.
    /// @param msg Diagnosable failure text.
    /// @return The failure result.
    static CommandResult err(Errc c, const std::string& msg, const std::string&)
    {
        return err(c, msg);
    }
    /// @brief Convert a generic Error into a result (success maps to an empty JSON object).
    /// @param e Error to convert.
    /// @return The result (success maps to an empty JSON object).
    static CommandResult from_error(const Error& e)
    {
        return e.ok() ? good(std::string("{}")) : err(e.code, e.str());
    }
    /// @brief Render the wire envelope: {"ok":true,"data":...} / {"ok":false,"error":{...}}.
    /// @return The envelope JSON text.
    std::string envelope() const;
};

/// @brief One command argument: JSON key, requiredness and help text.
struct ArgSpec
{
    std::string name; ///< JSON key of the argument.
    bool required = false; ///< True when the command cannot run without it.
    std::string help; ///< Short help text shown by --help.
};

/// @brief One ctrl command: operation name, description, args and handler.
struct CommandSpec
{
    std::string op; ///< Operation name, e.g. "raise".
    std::string description; ///< Human-readable summary for --help.
    std::vector<ArgSpec> args; ///< Argument list shown by --help.
    std::function<CommandResult(const std::string& json_args)> handler; ///< Implementation; receives the raw JSON args string.
};

/// @brief Registry of ctrl commands for a daemon or tool; also renders help and completions.
class CommandTable
{
  public:
    /// @brief Register a command (replaces any existing spec with the same op).
    /// @param spec Command to register.
    void add(CommandSpec spec)
    {
        ops_[spec.op] = std::move(spec);
    }
    /// @brief Look up a command by operation name.
    /// @return Pointer to the spec, or nullptr when unknown.
    /// @param op Operation name.
    /// @return Pointer to the spec, or nullptr when unknown.
    const CommandSpec* find(const std::string& op) const
    {
        auto it = ops_.find(op);
        return it == ops_.end() ? nullptr : &it->second;
    }
    /// @brief All registered commands, in op order.
    /// @return All registered commands, in op order.
    std::vector<const CommandSpec*> all() const
    {
        std::vector<const CommandSpec*> out;
        for (const auto& kv : ops_) out.push_back(&kv.second);
        return out;
    }
    /// @brief Number of registered commands.
    size_t size() const
    {
        return ops_.size();
    }

    /// @brief Run a command's handler, reporting unknown ops as NotFound.
    /// @param op Operation name.
    /// @param json_args Raw JSON argument string passed through to the handler.
    /// @return The handler's result, or a NotFound error.
    CommandResult dispatch(const std::string& op, const std::string& json_args) const
    {
        const CommandSpec* spec = find(op);
        if (!spec) return CommandResult::err(Errc::NotFound, "unknown op: " + op);
        return spec->handler(json_args);
    }

    /// @brief --help output listing every command.
    /// @param prog Tool name used in the usage line.
    /// @return The help text.
    std::string help_text(const std::string& prog) const;
    /// @brief Help for a single command ("help <op>" output).
    /// @param op Operation name to describe.
    /// @return Help text, or a NotFound error message when unknown.
    std::string help_for(const std::string& op) const;

    /// @brief Generate a bash completion script for @p prog from the registered ops.
    /// @param prog Tool name to complete.
    /// @return The completion script.
    std::string completion_bash(const std::string& prog) const;
    /// @brief Generate a zsh completion script for @p prog from the registered ops.
    /// @param prog Tool name to complete.
    /// @return The completion script.
    std::string completion_zsh(const std::string& prog) const;

  private:
    std::map<std::string, CommandSpec> ops_; ///< Registered commands keyed by op name.
};

/// @brief Parse command arguments into a JValue object.
/// @param json_args Raw JSON text; "{}" is substituted for an empty string.
/// @return Parsed object, or a Corrupt error for malformed JSON.
Result<JValue> args_object(const std::string& json_args);

} // namespace instr