#include "options.hpp"
#include "server.hpp"
#include "session.hpp"

#include <mcp/mcp.hpp>
#include <mcp/transport/http_session_manager.hpp>
#include <mcp_lldb/version.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv, argv + argc);
    const auto parsed = mcp_lldb::parse_options(args);
    if (!parsed.options) {
        (parsed.exit_code == 0 ? std::cout : std::cerr) << parsed.message;
        return parsed.exit_code;
    }
    const mcp_lldb::Options& options = *parsed.options;

    try {
        // Destroyed in reverse order: the server stops, the LLDB thread ends, the session kills or
        // detaches its process, and LLDB shuts down last.
        const mcp_lldb::LldbLibrary lldb_library;
        mcp_lldb::Session session;

        // Every handler runs on this one thread, the only one that touches LLDB. Execution tools
        // block it while they wait for the process, and the server's own loop keeps answering
        // pings and cancellations meanwhile.
        mcp::Runtime lldb_thread;
        lldb_thread.start(1);

        mcp::Server server("mcp-lldb", mcp_lldb::g_VERSION);
        server.set_handler_executor(lldb_thread.executor());
        mcp_lldb::register_debugger(server, session, options);

        if (options.transport == mcp_lldb::Transport::eStdio) {
            server.run_stdio();
        } else {
            std::cerr << "mcp-lldb listening on http://" << options.host << ":" << options.port
                      << "/mcp\n";
            const std::string token = options.token;
            server.run_http(options.host, options.port,
                            [&token](mcp::StreamableHttpSessionManager& http) {
                                if (!token.empty()) {
                                    http.set_bearer_token_validator(
                                        [token](std::string_view sent) { return sent == token; });
                                }
                            });
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "mcp-lldb: " << error.what() << "\n";
        return EXIT_FAILURE;
    }
}
