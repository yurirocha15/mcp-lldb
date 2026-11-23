#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp_lldb {

/// MCP transport.
enum class Transport : std::uint8_t {
    eStdio,
    eHttp,
};

/// Parsed command-line options.
struct Options {
    Transport transport = Transport::eStdio;
    std::string host = "127.0.0.1";
    std::uint16_t port = 9695;
    /// Bearer token required from HTTP clients; empty for none.
    std::string token;
    /// Whether the `command` tool, which runs any LLDB command, is offered.
    bool command_tool = true;
};

/// Result of parse_options().
struct ParseResult {
    std::optional<Options> options;  ///< Set when the server should run.
    int exit_code = 0;               ///< Exit code when the server should not run.
    std::string message;             ///< Text for standard output (help, version) or an error.
};

/// The usage text.
std::string usage(std::string_view program);

/// Parses the command line. Reads the token from MCP_LLDB_TOKEN when --token is absent.
ParseResult parse_options(const std::vector<std::string>& args);

/// Whether @p host names the loopback interface.
bool is_loopback_host(std::string_view host);

}  // namespace mcp_lldb
