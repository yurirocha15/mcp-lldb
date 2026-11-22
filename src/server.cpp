#include "server.hpp"

#include <mcp/core/async.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace mcp_lldb {

namespace {

using nlohmann::json;

constexpr std::uint32_t g_DEFAULT_TIMEOUT_SECONDS = 30;
constexpr std::uint32_t g_MAX_TIMEOUT_SECONDS = 3600;
constexpr std::uint32_t g_SETTLE_SECONDS = 5;

const std::string g_STATUS_URI = "lldb://status";
const std::string g_BREAKPOINTS_URI = "lldb://breakpoints";
const std::string g_OUTPUT_URI = "lldb://output";

// Schemas.

json property(const char* type, const char* description) {
    return json{{"type", type}, {"description", description}};
}

json object_schema(json properties, std::initializer_list<const char*> required = {}) {
    json schema{{"type", "object"}, {"properties", std::move(properties)}};
    if (required.size() > 0) {
        schema["required"] = json::array();
        for (const char* name : required) {
            schema["required"].push_back(name);
        }
    }
    return schema;
}

json thread_property() {
    return property("integer",
                    "Thread index ID, as in 'thread #2' (the threads tool lists them). Default: the "
                    "selected thread, which is the one that stopped.");
}

json frame_property() {
    return property("integer",
                    "Frame index in the thread's backtrace, 0 for the innermost. Default: the "
                    "selected frame (0 after a stop).");
}

json timeout_property() {
    return property("integer",
                    "Seconds to wait for the process to stop (default 30, at most 3600). If it has "
                    "not stopped by then the tool returns and the process keeps running.");
}

// Arguments. The server checks them against the schema before a handler runs.

std::optional<std::string> string_arg(const json& args, const char* name) {
    if (const auto it = args.find(name); it != args.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return std::nullopt;
}

std::optional<std::uint32_t> uint_arg(const json& args, const char* name) {
    if (const auto it = args.find(name); it != args.end() && it->is_number_integer()) {
        const auto value = it->get<std::int64_t>();
        if (value < 0) {
            throw ToolError(std::string(name) + " must not be negative");
        }
        return static_cast<std::uint32_t>(
            std::min<std::int64_t>(value, std::numeric_limits<std::uint32_t>::max()));
    }
    return std::nullopt;
}

bool bool_arg(const json& args, const char* name, bool fallback) {
    if (const auto it = args.find(name); it != args.end() && it->is_boolean()) {
        return it->get<bool>();
    }
    return fallback;
}

FrameRef frame_arg(const json& args) {
    return FrameRef{uint_arg(args, "thread"), uint_arg(args, "frame")};
}

std::uint32_t timeout_arg(const json& args) {
    return std::min(uint_arg(args, "timeout_seconds").value_or(g_DEFAULT_TIMEOUT_SECONDS),
                    g_MAX_TIMEOUT_SECONDS);
}

// Tool definitions.

mcp::Tool tool(const char* name, const char* title, const char* description, json schema,
               bool read_only, bool destructive = false) {
    mcp::Tool out;
    out.name = name;
    out.title = title;
    out.description = description;
    out.inputSchema = std::move(schema);
    mcp::ToolAnnotations annotations;
    annotations.title = title;
    annotations.readOnlyHint = read_only;
    if (!read_only) {
        annotations.destructiveHint = destructive;
    }
    annotations.openWorldHint = false;
    out.annotations = std::move(annotations);
    return out;
}

/// Tells subscribers that the session's resources may have changed.
void notify_changed(mcp::Server& server) {
    for (const auto& uri : {g_STATUS_URI, g_BREAKPOINTS_URI, g_OUTPUT_URI}) {
        server.sync().notify_resource_updated(uri);
    }
}

/// Waits for the process to stop, reporting progress each second and pausing the process if the
/// client cancels the call. Runs on the handler thread, which it may block.
mcp::Task<json> wait_and_report(mcp::Context& ctx, Session& session, std::uint32_t timeout) {
    std::uint32_t waited = 0;
    while (true) {
        const bool stopped = session.wait_for_stop(waited < timeout ? 1 : 0);
        if (stopped) {
            break;
        }
        ++waited;
        if (ctx.is_cancelled()) {
            // The client gave up on the call: leave the process paused rather than running.
            try {
                session.interrupt();
            } catch (const ToolError&) {
            }
            for (std::uint32_t i = 0; i < g_SETTLE_SECONDS && !session.wait_for_stop(1); ++i) {
            }
            break;
        }
        if (waited >= timeout) {
            json report = session.running_report(waited);
            co_return report;
        }
        co_await ctx.report_progress(waited, timeout, std::string("Waiting for the process to stop"));
    }
    json report = session.stop_report();
    co_return report;
}

void add_session_tools(mcp::Server& server, Session& session) {
    server.add_tool(
        tool("create_target", "Load a program",
             "Load a program, without running it, so breakpoints can be set before launch. Give "
             "core to load a core file instead and inspect the crash it recorded (threads, "
             "backtrace, variables, memory). Replaces the current target: a process the server "
             "launched is killed, one it attached to is detached.",
             object_schema({{"program", property("string", "Path of the executable")},
                            {"core", property("string", "Path of a core file the program dumped")},
                            {"arch", property("string",
                                              "Architecture or triple, such as x86_64; "
                                              "default: the executable's")}},
                           {"program"}),
             false),
        [&server, &session](const json& args) -> json {
            const auto program = *string_arg(args, "program");
            json out = string_arg(args, "core")
                           ? session.load_core(program, *string_arg(args, "core"))
                           : session.create_target(program, string_arg(args, "arch"));
            notify_changed(server);
            return out;
        });

    json env_schema = property("object", "Environment variables to set, added to the server's own");
    env_schema["additionalProperties"] = json{{"type", "string"}};
    server.add_tool(
        tool("launch", "Launch the program",
             "Start the program under the debugger and wait until it stops at a breakpoint, "
             "crashes, receives a signal or exits. Returns why and where it stopped, or its exit "
             "status, and what it printed. Set breakpoints first (create_target loads the program "
             "without running it), or use stop_at_entry. Standard input is /dev/null unless stdin "
             "names a file.",
             object_schema({{"program", property("string",
                                                 "Path of the executable; default: the "
                                                 "loaded target")},
                            {"args", json{{"type", "array"},
                                          {"items", {{"type", "string"}}},
                                          {"description", "Command-line arguments"}}},
                            {"env", env_schema},
                            {"cwd", property("string", "Working directory")},
                            {"stdin", property("string", "File to read standard input from")},
                            {"stop_at_entry", property("boolean",
                                                       "Stop before the program's first "
                                                       "instruction (default false)")},
                            {"timeout_seconds", timeout_property()}}),
             false, true),
        [&server, &session](mcp::Context& ctx, const json& args) -> mcp::Task<json> {
            LaunchRequest request;
            request.program = string_arg(args, "program");
            if (const auto it = args.find("args"); it != args.end()) {
                request.args = it->get<std::vector<std::string>>();
            }
            if (const auto it = args.find("env"); it != args.end()) {
                request.env = it->get<std::map<std::string, std::string>>();
            }
            request.cwd = string_arg(args, "cwd");
            request.stdin_path = string_arg(args, "stdin");
            request.stop_at_entry = bool_arg(args, "stop_at_entry", false);
            session.start_launch(request);
            json report = co_await wait_and_report(ctx, session, timeout_arg(args));
            notify_changed(server);
            co_return report;
        });

    server.add_tool(
        tool("attach", "Attach to a process",
             "Attach to a running process by pid or by name, which stops it. With wait_for, wait "
             "for a process of that name to start. Detaching (terminate) leaves it running.",
             object_schema({{"pid", property("integer", "Process ID")},
                            {"name", property("string", "Process name")},
                            {"wait_for", property("boolean",
                                                  "Wait for a process called name to "
                                                  "start (default false)")},
                            {"timeout_seconds", timeout_property()}}),
             false, true),
        [&server, &session](mcp::Context& ctx, const json& args) -> mcp::Task<json> {
            AttachRequest request;
            if (const auto it = args.find("pid"); it != args.end() && it->is_number_integer()) {
                request.pid = it->get<std::uint64_t>();
            }
            request.name = string_arg(args, "name");
            request.wait_for = bool_arg(args, "wait_for", false);
            session.start_attach(request);
            json report = co_await wait_and_report(ctx, session, timeout_arg(args));
            notify_changed(server);
            co_return report;
        });

    server.add_tool(
        tool("continue", "Continue",
             "Resume the stopped process and wait until it stops again or exits. On a process "
             "that is still running, just wait. Cancelling the call pauses the process.",
             object_schema({{"timeout_seconds", timeout_property()}}), false),
        [&server, &session](mcp::Context& ctx, const json& args) -> mcp::Task<json> {
            session.start_continue();
            json report = co_await wait_and_report(ctx, session, timeout_arg(args));
            notify_changed(server);
            co_return report;
        });

    json step_schema = object_schema(
        {{"kind", property("string",
                           "over: next source line, stepping over calls (default); "
                           "into: into the call on this line; out: until the current "
                           "function returns; instruction / instruction_over: one machine "
                           "instruction")},
         {"thread", thread_property()},
         {"timeout_seconds", timeout_property()}});
    step_schema["properties"]["kind"]["enum"] = {"over", "into", "out", "instruction",
                                                 "instruction_over"};
    server.add_tool(
        tool("step", "Step", "Step one thread and wait for it to stop; returns the new location.",
             std::move(step_schema), false),
        [&server, &session](mcp::Context& ctx, const json& args) -> mcp::Task<json> {
            const std::string kind = string_arg(args, "kind").value_or("over");
            StepKind step = StepKind::eOver;
            if (kind == "into") {
                step = StepKind::eInto;
            } else if (kind == "out") {
                step = StepKind::eOut;
            } else if (kind == "instruction") {
                step = StepKind::eInstruction;
            } else if (kind == "instruction_over") {
                step = StepKind::eInstructionOver;
            }
            session.start_step(step, uint_arg(args, "thread"));
            json report = co_await wait_and_report(ctx, session, timeout_arg(args));
            notify_changed(server);
            co_return report;
        });

    server.add_tool(
        tool("run_to", "Run to a line",
             "Continue until the given source line runs, through a temporary breakpoint. The "
             "process stops earlier at any other breakpoint.",
             object_schema({{"file", property("string", "Source file, as a path or a file name")},
                            {"line", property("integer", "Line number")},
                            {"timeout_seconds", timeout_property()}},
                           {"file", "line"}),
             false),
        [&server, &session](mcp::Context& ctx, const json& args) -> mcp::Task<json> {
            session.start_run_to(*string_arg(args, "file"), *uint_arg(args, "line"));
            json report = co_await wait_and_report(ctx, session, timeout_arg(args));
            notify_changed(server);
            co_return report;
        });

    server.add_tool(
        tool("interrupt", "Pause the process", "Pause a running process and report where it is.",
             object_schema(json::object()), false),
        [&server, &session](mcp::Context& ctx, const json&) -> mcp::Task<json> {
            session.interrupt();
            json report = co_await wait_and_report(ctx, session, g_SETTLE_SECONDS);
            notify_changed(server);
            co_return report;
        });

    json terminate_schema =
        object_schema({{"action", property("string",
                                           "kill or detach; default: kill a process the server "
                                           "launched, detach from one it attached to")}});
    terminate_schema["properties"]["action"]["enum"] = {"kill", "detach"};
    server.add_tool(tool("terminate", "End the process", "Kill the process or detach from it.",
                         std::move(terminate_schema), false, true),
                    [&server, &session](const json& args) -> json {
                        json out = session.terminate(string_arg(args, "action"));
                        notify_changed(server);
                        return out;
                    });

    server.add_tool(tool("status", "Session status",
                         "The loaded program, the process's state and where it is stopped, and the "
                         "number of breakpoints. A good first call.",
                         object_schema(json::object()), true),
                    [&session](const json&) -> json { return session.status(); });
}

void add_breakpoint_tools(mcp::Server& server, Session& session) {
    server.add_tool(
        tool("set_breakpoint", "Set a breakpoint",
             "Set a breakpoint by file and line, function name, function-name regex or address. "
             "Returns the breakpoint and the code locations it resolved to; one that resolves "
             "nowhere yet stays pending until a matching library loads.",
             object_schema(
                 {{"file", property("string", "Source file, as a path or a file name; with line")},
                  {"line", property("integer", "Line number; with file")},
                  {"function", property("string", "Function name, such as main or Foo::bar")},
                  {"regex", property("string", "Regular expression matching function names")},
                  {"address", property("string", "Address, or an expression giving one")},
                  {"condition", property("string",
                                         "Stop only when this expression is true, such as "
                                         "i == 5")},
                  {"ignore_count", property("integer", "Skip this many hits before stopping")},
                  {"one_shot", property("boolean", "Delete the breakpoint after its first hit")}}),
             false),
        [&server, &session](const json& args) -> json {
            BreakpointRequest request;
            request.file = string_arg(args, "file");
            request.line = uint_arg(args, "line");
            request.function = string_arg(args, "function");
            request.regex = string_arg(args, "regex");
            request.address = string_arg(args, "address");
            request.condition = string_arg(args, "condition");
            request.ignore_count = uint_arg(args, "ignore_count");
            request.one_shot = bool_arg(args, "one_shot", false);
            json out = session.set_breakpoint(request);
            notify_changed(server);
            return out;
        });

    server.add_tool(tool("list_breakpoints", "List breakpoints",
                         "Every breakpoint and watchpoint, with locations and hit counts.",
                         object_schema(json::object()), true),
                    [&session](const json&) -> json { return session.list_breakpoints(); });

    server.add_tool(
        tool("delete_breakpoint", "Delete a breakpoint", "Delete a breakpoint or a watchpoint by ID.",
             object_schema({{"id", property("integer",
                                            "Breakpoint ID, or watchpoint ID with "
                                            "watchpoint")},
                            {"watchpoint", property("boolean", "The ID is a watchpoint's")}},
                           {"id"}),
             false),
        [&server, &session](const json& args) -> json {
            const auto id = args.at("id").get<std::int64_t>();
            json out = bool_arg(args, "watchpoint", false) ? session.delete_watchpoint(id)
                                                           : session.delete_breakpoint(id);
            notify_changed(server);
            return out;
        });

    server.add_tool(
        tool("set_watchpoint", "Set a watchpoint",
             "Stop when memory is written (or read): a variable of the current frame, such as "
             "count or node->next, or size bytes at an address. Hardware allows only a few.",
             object_schema({{"variable", property("string", "Variable path in the frame")},
                            {"address", property("string", "Address, or an expression giving one")},
                            {"size", property("integer", "Bytes to watch at address: 1, 2, 4 or 8")},
                            {"read", property("boolean", "Stop on reads (default false)")},
                            {"write", property("boolean",
                                               "Stop on writes that change the value "
                                               "(default true)")},
                            {"thread", thread_property()},
                            {"frame", frame_property()}}),
             false),
        [&server, &session](const json& args) -> json {
            WatchpointRequest request;
            request.variable = string_arg(args, "variable");
            request.address = string_arg(args, "address");
            request.size = uint_arg(args, "size").value_or(0);
            request.read = bool_arg(args, "read", false);
            request.write = bool_arg(args, "write", true);
            request.thread = uint_arg(args, "thread");
            request.frame = uint_arg(args, "frame");
            json out = session.set_watchpoint(request);
            notify_changed(server);
            return out;
        });
}

void add_inspection_tools(mcp::Server& server, Session& session) {
    server.add_tool(tool("threads", "List threads",
                         "Every thread of the stopped process, with its stop reason and location.",
                         object_schema(json::object()), true),
                    [&session](const json&) -> json { return session.threads(); });

    server.add_tool(
        tool("backtrace", "Backtrace",
             "The call stack of a thread, or of every thread with all_threads (to find a deadlock "
             "or see what each thread was doing at a crash).",
             object_schema(
                 {{"thread", thread_property()},
                  {"count", property("integer", "Frames to show (default 32, at most 1000)")},
                  {"all_threads", property("boolean", "Every thread's stack (default false)")}}),
             true),
        [&session](const json& args) -> json {
            return session.backtrace(uint_arg(args, "thread"), uint_arg(args, "count").value_or(32),
                                     bool_arg(args, "all_threads", false));
        });

    json variables_schema = object_schema(
        {{"thread", thread_property()},
         {"frame", frame_property()},
         {"name", property("string",
                           "One variable or path, such as config.timeout or items[3]; "
                           "default: all variables of scope")},
         {"scope", property("string", "locals: arguments and locals (default); args; statics; all")},
         {"depth", property("integer", "Levels of members and elements to expand (default 1)")},
         {"max_children", property("integer", "Members or elements shown per value (default 32)")}});
    variables_schema["properties"]["scope"]["enum"] = {"locals", "args", "statics", "all"};
    server.add_tool(tool("variables", "Frame variables",
                         "Variables of a stack frame with their types and values; containers such as "
                         "std::vector show their elements.",
                         std::move(variables_schema), true),
                    [&session](const json& args) -> json {
                        return session.variables(
                            frame_arg(args), string_arg(args, "scope").value_or("locals"),
                            uint_arg(args, "depth").value_or(1),
                            uint_arg(args, "max_children").value_or(32), string_arg(args, "name"));
                    });

    server.add_tool(
        tool(
            "evaluate", "Evaluate an expression",
            "Evaluate an expression in the language of the frame, such as v.size(), *node, or a "
            "function call. Calls run code in the process. Without a stopped process, only "
            "expressions that need no running code work.",
            object_schema({{"expression", property("string", "The expression")},
                           {"thread", thread_property()},
                           {"frame", frame_property()},
                           {"depth", property("integer", "Levels of the result to expand (default 2)")},
                           {"timeout_ms", property("integer",
                                                   "Milliseconds the expression may run "
                                                   "(default 5000)")}},
                          {"expression"}),
            false),
        [&session](const json& args) -> json {
            return session.evaluate(
                *string_arg(args, "expression"), frame_arg(args), uint_arg(args, "depth").value_or(2),
                std::min<std::uint32_t>(uint_arg(args, "timeout_ms").value_or(5000), 600000));
        });

    server.add_tool(
        tool("read_memory", "Read memory", "A hex dump of process memory, with the bytes as text.",
             object_schema({{"address", property("string",
                                                 "Address, or an expression giving one, "
                                                 "such as 0x7ffe0000, &buffer or $sp")},
                            {"size", property("integer", "Bytes to read (default 256, at most 16384)")},
                            {"thread", thread_property()},
                            {"frame", frame_property()}},
                           {"address"}),
             true),
        [&session](const json& args) -> json {
            return session.read_memory(*string_arg(args, "address"),
                                       uint_arg(args, "size").value_or(256), frame_arg(args));
        });

    server.add_tool(tool("registers", "Registers",
                         "The general-purpose registers of a frame, or every register set.",
                         object_schema({{"thread", thread_property()},
                                        {"frame", frame_property()},
                                        {"all_sets", property("boolean",
                                                              "Include floating-point and "
                                                              "vector registers")}}),
                         true),
                    [&session](const json& args) -> json {
                        return session.registers(frame_arg(args), bool_arg(args, "all_sets", false));
                    });

    server.add_tool(
        tool("disassemble", "Disassemble",
             "Machine instructions of a function, from an address, or by default around the "
             "frame's program counter (marked current).",
             object_schema(
                 {{"function", property("string", "Function name")},
                  {"address", property("string", "Start address, or an expression giving one")},
                  {"count", property("integer", "Instructions to show (default 32)")},
                  {"thread", thread_property()},
                  {"frame", frame_property()}}),
             true),
        [&session](const json& args) -> json {
            return session.disassemble(string_arg(args, "address"), string_arg(args, "function"),
                                       uint_arg(args, "count").value_or(32), frame_arg(args));
        });

    server.add_tool(
        tool("source", "Show source",
             "Source lines around a frame's current line (marked =>), or around a line of a file.",
             object_schema({{"file", property("string", "Source file; default: the frame's")},
                            {"line", property("integer", "Center line; default: the frame's line")},
                            {"context", property("integer", "Lines before and after (default 10)")},
                            {"thread", thread_property()},
                            {"frame", frame_property()}}),
             true),
        [&session](const json& args) -> json {
            return session.source(string_arg(args, "file"), uint_arg(args, "line"),
                                  uint_arg(args, "context").value_or(10), frame_arg(args));
        });

    server.add_tool(
        tool("lookup", "Look up a symbol",
             "Find functions, global variables and types by name (with their locations, signatures "
             "and fields), or say which function and line an address belongs to.",
             object_schema({{"name", property("string", "Function, variable or type name")},
                            {"address", property("string", "Address, or an expression giving one")}}),
             true),
        [&session](const json& args) -> json {
            return session.lookup(string_arg(args, "name"), string_arg(args, "address"));
        });

    server.add_tool(tool("modules", "List modules",
                         "The executable and shared libraries of the target, with load addresses and "
                         "whether they have debug information.",
                         object_schema(json::object()), true),
                    [&session](const json&) -> json { return session.modules(); });

    server.add_tool(
        tool("output", "Program output",
             "What the program wrote to standard output and standard error (the last "
             "64 KiB at most). Stop reports already carry new output.",
             object_schema({{"tail_bytes", property("integer",
                                                    "Return only this many "
                                                    "bytes from the end")}}),
             true),
        [&session](const json& args) -> json { return session.output(uint_arg(args, "tail_bytes")); });
}

void add_command_tool(mcp::Server& server, Session& session) {
    server.add_tool(
        tool("command", "Run an LLDB command",
             "Run any LLDB command line, such as 'image lookup -r -n foo' or 'settings show', for "
             "what the other tools do not cover. Execution commands return at once; follow them "
             "with continue to wait for the stop.",
             object_schema({{"command", property("string", "The LLDB command line")}}, {"command"}),
             false, true),
        [&server, &session](const json& args) -> json {
            json out = session.command(*string_arg(args, "command"));
            notify_changed(server);
            return out;
        });
}

void add_resources(mcp::Server& server, Session& session) {
    const auto json_resource = [&server](const std::string& uri, const char* name,
                                         const char* description, auto read) {
        mcp::Resource resource;
        resource.uri = uri;
        resource.name = name;
        resource.description = description;
        resource.mimeType = "application/json";
        server.add_text_resource(resource, [read]() -> std::string {
            try {
                return read().dump(2);
            } catch (const ToolError& error) {
                return json{{"error", error.what()}}.dump(2);
            }
        });
    };
    json_resource(g_STATUS_URI, "Session status", "The target, process state and stop location",
                  [&session] { return session.status(); });
    json_resource(g_BREAKPOINTS_URI, "Breakpoints", "Breakpoints and watchpoints",
                  [&session] { return session.list_breakpoints(); });
    json_resource(g_OUTPUT_URI, "Program output", "What the program wrote, the last 64 KiB",
                  [&session] { return session.output(std::nullopt); });
    server.capabilities().resources->subscribe = true;
}

mcp::PromptArgument prompt_argument(const char* name, const char* description, bool required) {
    mcp::PromptArgument argument;
    argument.name = name;
    argument.description = description;
    argument.required = required;
    return argument;
}

void add_prompts(mcp::Server& server) {
    server.add_text_prompt(
        "debug_crash", "Find out why a program crashes",
        {prompt_argument("program", "Path of the program", true),
         prompt_argument("args", "Its command-line arguments", false)},
        [](const mcp::Server::PromptArguments& args) {
            std::string text = "The program " + args.at("program");
            if (const auto it = args.find("args"); it != args.end() && !it->second.empty()) {
                text += " (arguments: " + it->second + ")";
            }
            text +=
                " crashes. Find the root cause with the debugger tools:\n"
                "1. launch it and let it run until it stops.\n"
                "2. At the crash, read the stop reason, then backtrace (all_threads if threads are "
                "involved).\n"
                "3. Inspect variables and source in the crashing frame and its callers; evaluate "
                "expressions to test what you suspect.\n"
                "4. If the bad value was written earlier, set a breakpoint or watchpoint there and "
                "launch again.\n"
                "Explain the cause with the evidence you found and propose a fix.";
            return text;
        });

    server.add_text_prompt(
        "analyze_core", "Explain a crash recorded in a core file",
        {prompt_argument("program", "Path of the program", true),
         prompt_argument("core", "Path of the core file", true)},
        [](const mcp::Server::PromptArguments& args) {
            return "Load the core file " + args.at("core") + " of " + args.at("program") +
                   " with create_target, then explain the crash: the stop reason, the backtrace of "
                   "the crashing thread (and of all threads if relevant), the variables and source "
                   "of the frames involved, and memory where pointers look wrong. Give the most "
                   "likely cause and a fix.";
        });

    server.add_text_prompt(
        "investigate", "Investigate a bug by running the program under the debugger",
        {prompt_argument("program", "Path of the program", true),
         prompt_argument("issue", "What goes wrong", true)},
        [](const mcp::Server::PromptArguments& args) {
            return "Investigate this problem in " + args.at("program") + ": " + args.at("issue") +
                   "\nLoad the program with create_target, set breakpoints where the behavior "
                   "starts to diverge, launch it, and step and inspect variables to confirm or "
                   "rule out each hypothesis. Report what you observed, the cause, and a fix.";
        });
}

}  // namespace

const char* server_instructions() {
    return "A debugger (LLDB) for native programs built with debug information, such as C and C++ "
           "programs. One session holds one program and its process. Typical flow: "
           "create_target, set_breakpoint, launch (waits for the first stop), then variables, "
           "backtrace, evaluate, step and continue. Execution tools wait for the process to stop "
           "and return why and where; cancelling one pauses the process. Inspection tools need a "
           "stopped process or a core file. Thread numbers are LLDB index IDs (thread #1, #2...). "
           "Programs run with the server's permissions.";
}

void register_debugger(mcp::Server& server, Session& session, const Options& options) {
    server.set_instructions(server_instructions());
    add_session_tools(server, session);
    add_breakpoint_tools(server, session);
    add_inspection_tools(server, session);
    if (options.command_tool) {
        add_command_tool(server, session);
    }
    add_resources(server, session);
    add_prompts(server);
}

}  // namespace mcp_lldb
