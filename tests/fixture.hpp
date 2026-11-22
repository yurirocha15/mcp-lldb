#pragma once

#include <cstdint>
#include <string>

namespace mcp_lldb::test {

/// The path of the program the tests debug.
inline const char* sample_program() { return MCP_LLDB_SAMPLE_PROGRAM; }

/// The path of its source file.
inline const char* sample_source() { return MCP_LLDB_SAMPLE_SOURCE; }

/// The path of the server binary.
inline const char* server_binary() { return MCP_LLDB_BINARY; }

/// The line of the sample source that carries the comment "MARK:<name>".
std::uint32_t line_of(const std::string& name);

}  // namespace mcp_lldb::test
