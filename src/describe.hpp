#pragma once

// Conversions from LLDB objects to the JSON the tools return.

#include <lldb/API/LLDB.h>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace mcp_lldb {

/// A C string from LLDB, which may be null.
std::string text_of(const char* text);

/// @p address as 0x-prefixed hexadecimal.
std::string hex_address(std::uint64_t address);

/// The full path of @p file, or an empty string when it names nothing.
std::string path_of(const lldb::SBFileSpec& file);

/// The name of a process state, such as "stopped" or "exited".
std::string state_name(lldb::StateType state);

/// The name of a thread stop reason, such as "breakpoint" or "signal".
std::string stop_reason_name(lldb::StopReason reason);

/// Whether @p state means the process has ended or is gone.
bool is_finished(lldb::StateType state);

/// Whether @p state means the process is stopped and can be inspected.
bool is_stopped(lldb::StateType state);

/// One stack frame: index, pc, function, module, and source location when known.
nlohmann::json frame_json(lldb::SBFrame frame);

/// A thread: index ID, OS thread ID, name, stop reason and its top frame.
nlohmann::json thread_json(lldb::SBThread thread);

/// Limits on how much of a value tree value_json() writes.
struct ValueLimits {
    std::uint32_t depth = 1;          ///< Levels of children below the value.
    std::uint32_t max_children = 32;  ///< Children written per value.
    std::uint32_t max_nodes = 2000;   ///< Values written in total.
};

/// A value: name, type, value, summary, error, and children within @p limits. A value with
/// children that are not written gets `"has_children": true`; one with more children than
/// written gets `"more_children": true`.
nlohmann::json value_json(lldb::SBValue value, const ValueLimits& limits);

/// As value_json() above, counting written values in @p nodes across several calls.
nlohmann::json value_json(lldb::SBValue value, const ValueLimits& limits, std::uint32_t& nodes);

}  // namespace mcp_lldb
