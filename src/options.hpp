#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcp_lldb {

/// How the server talks to its client.
enum class Transport : std::uint8_t {
    eStdio,
    eHttp,
};

/// The command line, parsed.
struct Options {
    Transport transport = Transport::eStdio;
    std::string host = "127.0.0.1";
    std::uint16_t port = 9695;
    /// The bearer token HTTP clients must send; empty for none.
    std::string token;
    /// Whether the `command` tool, which runs any LLDB command, is offered.
    bool command_tool = true;
};

/// What parse_options() decided.
struct ParseResult {
    std::optional<Options> options;  ///< Set when the server should run.
    int exit_code = 0;               ///< The exit code when it should not.
    std::string message;             ///< Text for standard output (help, version) or an error.
};

/// The usage text.
std::string usage(std::string_view program);

/// Parses the command line. The MCP_LLDB_TOKEN environment variable supplies the token when
/// --token is not given.
ParseResult parse_options(const std::vector<std::string>& args);

/// Whether @p host names the loopback interface.
bool is_loopback_host(std::string_view host);

}  // namespace mcp_lldb
