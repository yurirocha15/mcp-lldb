#pragma once

#include "options.hpp"
#include "session.hpp"

#include <mcp/mcp.hpp>

namespace mcp_lldb {

/// Registers on @p server the tools, resources and prompts that drive @p session.
///
/// All handlers call LLDB, so they must run on one thread: set a single-threaded executor with
/// Server::set_handler_executor() before serving.
void register_debugger(mcp::Server& server, Session& session, const Options& options);

/// The instructions the server sends clients when they connect.
const char* server_instructions();

}  // namespace mcp_lldb
