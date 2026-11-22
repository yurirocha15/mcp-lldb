#include "options.hpp"

#include <mcp_lldb/version.hpp>

#include <charconv>
#include <cstdlib>
#include <sstream>

namespace mcp_lldb {

namespace {

std::optional<std::uint16_t> parse_port(std::string_view text) {
    unsigned value = 0;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc() || ptr != end || value == 0 || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

ParseResult error(std::string message) {
    ParseResult result;
    result.exit_code = 2;
    result.message = std::move(message) + "\nRun with --help for usage.\n";
    return result;
}

}  // namespace

std::string usage(std::string_view program) {
    std::ostringstream out;
    out << "Usage: " << program << " [options]\n"
        << "\n"
        << "An MCP server that debugs programs with LLDB.\n"
        << "\n"
        << "Options:\n"
        << "  --transport=<stdio|http>  How clients connect (default: stdio)\n"
        << "  --host=<address>          HTTP listen address (default: 127.0.0.1)\n"
        << "  --port=<n>                HTTP listen port (default: 9695)\n"
        << "  --token=<token>           Bearer token HTTP clients must send\n"
        << "                            (default: $MCP_LLDB_TOKEN; required off loopback)\n"
        << "  --no-command-tool         Do not offer the tool that runs any LLDB command\n"
        << "  --version                 Print the version\n"
        << "  --help                    Print this help\n";
    return out.str();
}

bool is_loopback_host(std::string_view host) {
    return host == "127.0.0.1" || host == "::1" || host == "localhost" || host.starts_with("127.");
}

ParseResult parse_options(const std::vector<std::string>& args) {
    Options options;
    if (const char* token = std::getenv("MCP_LLDB_TOKEN"); token != nullptr) {
        options.token = token;
    }
    const std::string_view program = args.empty() ? "mcp-lldb" : std::string_view(args.front());

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const auto value_of = [&arg](std::string_view prefix) -> std::optional<std::string_view> {
            if (arg.starts_with(prefix)) {
                return arg.substr(prefix.size());
            }
            return std::nullopt;
        };

        if (arg == "--help" || arg == "-h") {
            return {std::nullopt, 0, usage(program)};
        }
        if (arg == "--version") {
            return {std::nullopt, 0, std::string("mcp-lldb ") + g_VERSION + "\n"};
        }
        if (arg == "--no-command-tool") {
            options.command_tool = false;
        } else if (auto transport = value_of("--transport=")) {
            if (*transport == "stdio") {
                options.transport = Transport::eStdio;
            } else if (*transport == "http") {
                options.transport = Transport::eHttp;
            } else {
                return error("Unknown transport: " + std::string(*transport));
            }
        } else if (auto host = value_of("--host=")) {
            if (host->empty()) {
                return error("--host needs an address");
            }
            options.host = std::string(*host);
        } else if (auto port = value_of("--port=")) {
            const auto parsed = parse_port(*port);
            if (!parsed) {
                return error("Invalid port: " + std::string(*port));
            }
            options.port = *parsed;
        } else if (auto token = value_of("--token=")) {
            options.token = std::string(*token);
        } else {
            return error("Unknown option: " + std::string(arg));
        }
    }

    if (options.transport == Transport::eHttp && !is_loopback_host(options.host) &&
        options.token.empty()) {
        return error("Listening on " + options.host +
                     " needs a token (--token or MCP_LLDB_TOKEN): anyone who reaches the "
                     "server can run programs as you");
    }
    return {options, 0, {}};
}

}  // namespace mcp_lldb
