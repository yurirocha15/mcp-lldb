#pragma once

#include "options.hpp"
#include "session.hpp"

#include <mcp/mcp.hpp>

namespace mcp_lldb {

/// Registers on @p server the tools, resources and prompts that drive @p session.
///
/// Every handler touches LLDB, so the server must run them all on one thread: give it a
/// one-thread executor with Server::set_handler_executor() before it serves.
void register_debugger(mcp::Server& server, Session& session, const Options& options);

/// The instructions the server sends clients when they connect.
const char* server_instructions();

}  // namespace mcp_lldb
