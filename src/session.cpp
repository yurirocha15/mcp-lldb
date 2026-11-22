#include "session.hpp"

#include "describe.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <stdlib.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <crt_externs.h>
#endif

namespace mcp_lldb {

namespace {

/// Bytes of process output kept; older output is dropped first.
constexpr std::size_t g_OUTPUT_LIMIT = std::size_t{1} << 20;
/// Bytes of new output a stop report carries; the output tool returns more.
constexpr std::size_t g_REPORT_OUTPUT_LIMIT = 8192;
/// Bytes of output the output tool returns at most.
constexpr std::size_t g_OUTPUT_TOOL_LIMIT = 65536;
constexpr std::size_t g_READ_CHUNK = 4096;
/// Seconds to wait for a process to stop after an interrupt, kill or detach.
constexpr std::uint32_t g_SETTLE_SECONDS = 5;

void set_environment(const char* name, const std::string& value) {
#ifdef _WIN32
    (void)_putenv_s(name, value.c_str());
#else
    (void)setenv(name, value.c_str(), 1);
#endif
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

/// LLDB on Linux starts processes through lldb-server, which it looks for next to liblldb. Debian
/// and Ubuntu install liblldb elsewhere, so point LLDB at the lldb-server of the LLVM install the
/// build found, or at one that matches the library's version.
void locate_debug_server() {
    if (std::getenv("LLDB_DEBUGSERVER_PATH") != nullptr) {
        return;
    }
    std::vector<std::filesystem::path> candidates;
#ifdef MCP_LLDB_SERVER_HINT
    candidates.emplace_back(MCP_LLDB_SERVER_HINT);
#endif
#ifdef __APPLE__
    candidates.emplace_back(
        "/Applications/Xcode.app/Contents/SharedFrameworks/LLDB.framework/Resources/debugserver");
    candidates.emplace_back(
        "/Library/Developer/CommandLineTools/Library/PrivateFrameworks/LLDB.framework/Resources/"
        "debugserver");
#endif
    for (const auto& candidate : candidates) {
        if (file_exists(candidate)) {
            set_environment("LLDB_DEBUGSERVER_PATH", candidate.string());
            return;
        }
    }
}

/// The server's environment as NAME=VALUE entries, with @p overrides set.
std::vector<std::string> launch_environment(const std::map<std::string, std::string>& overrides) {
#if defined(_WIN32)
    char** entries = _environ;
#elif defined(__APPLE__)
    char** entries = *_NSGetEnviron();
#else
    char** entries = environ;
#endif
    std::map<std::string, std::string> merged;
    for (char** entry = entries; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string text(*entry);
        const auto equals = text.find('=');
        if (equals != std::string::npos && equals > 0) {
            merged[text.substr(0, equals)] = text.substr(equals + 1);
        }
    }
    for (const auto& [name, value] : overrides) {
        merged[name] = value;
    }
    std::vector<std::string> out;
    out.reserve(merged.size());
    for (const auto& [name, value] : merged) {
        out.push_back(name + "=" + value);
    }
    return out;
}

std::string error_text(const lldb::SBError& error, std::string_view fallback) {
    const char* text = error.GetCString();
    return text != nullptr && *text != '\0' ? std::string(text) : std::string(fallback);
}

/// Whether two paths name the same file.
bool same_file(const std::string& a, const std::string& b) {
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(a, b, error);
    return !error && equivalent;
}

lldb::SBThread thread_with_stop_reason(lldb::SBProcess process) {
    lldb::SBThread selected = process.GetSelectedThread();
    const auto meaningful = [](lldb::SBThread thread) {
        const auto reason = thread.GetStopReason();
        return thread.IsValid() && reason != lldb::eStopReasonNone &&
               reason != lldb::eStopReasonInvalid;
    };
    if (meaningful(selected)) {
        return selected;
    }
    for (std::uint32_t i = 0; i < process.GetNumThreads(); ++i) {
        lldb::SBThread thread = process.GetThreadAtIndex(i);
        if (meaningful(thread)) {
            return thread;
        }
    }
    if (selected.IsValid()) {
        return selected;
    }
    return process.GetNumThreads() > 0 ? process.GetThreadAtIndex(0) : lldb::SBThread();
}

}  // namespace

LldbLibrary::LldbLibrary() {
    locate_debug_server();
    lldb::SBDebugger::Initialize();
}

LldbLibrary::~LldbLibrary() { lldb::SBDebugger::Terminate(); }

Session::Session() : debugger_(lldb::SBDebugger::Create(false)), listener_("mcp-lldb.process") {
    if (!debugger_.IsValid()) {
        throw std::runtime_error("LLDB could not create a debugger");
    }
    debugger_.SetAsync(true);
    debugger_.SetUseColor(false);
    // The server's standard input and output may carry the protocol: LLDB must neither read the
    // one nor write messages of its own (such as a watchpoint's old and new value) to the other.
#ifdef _WIN32
    const char* null_device = "NUL";
#else
    const char* null_device = "/dev/null";
#endif
    if (FILE* input = std::fopen(null_device, "r"); input != nullptr) {
        debugger_.SetInputFileHandle(input, true);
    }
    if (FILE* output = std::fopen(null_device, "w"); output != nullptr) {
        debugger_.SetOutputFileHandle(output, true);
    }
    if (FILE* error = std::fopen(null_device, "w"); error != nullptr) {
        debugger_.SetErrorFileHandle(error, true);
    }
    lldb::SBCommandReturnObject ignored;
    debugger_.GetCommandInterpreter().HandleCommand("settings set auto-confirm true", ignored);
}

Session::~Session() {
    try {
        end_process();
        if (target_.IsValid()) {
            debugger_.DeleteTarget(target_);
        }
    } catch (...) {
        // Nothing can be reported while the server shuts down.
    }
    lldb::SBDebugger::Destroy(debugger_);
}

bool Session::has_target() const { return target_.IsValid(); }

lldb::SBTarget Session::require_target() {
    adopt_selected_target();
    if (!target_.IsValid()) {
        throw ToolError("No target: load a program with create_target or launch first");
    }
    return target_;
}

lldb::SBProcess Session::require_process() {
    lldb::SBProcess process = require_target().GetProcess();
    if (!process.IsValid() || is_finished(process.GetState())) {
        throw ToolError("No process: start one with launch or attach, or load a core file");
    }
    return process;
}

lldb::SBProcess Session::require_stopped_process() {
    lldb::SBProcess process = require_process();
    const auto state = process.GetState();
    if (!is_stopped(state)) {
        throw ToolError("The process is " + state_name(state) +
                        "; it must be stopped (use interrupt to pause it)");
    }
    return process;
}

lldb::SBThread Session::thread_for(lldb::SBProcess process, std::optional<std::uint32_t> thread) {
    if (thread) {
        lldb::SBThread found = process.GetThreadByIndexID(*thread);
        if (!found.IsValid()) {
            throw ToolError("No thread #" + std::to_string(*thread) + "; the threads tool lists them");
        }
        return found;
    }
    lldb::SBThread selected = process.GetSelectedThread();
    if (selected.IsValid()) {
        return selected;
    }
    if (process.GetNumThreads() == 0) {
        throw ToolError("The process has no threads");
    }
    return process.GetThreadAtIndex(0);
}

lldb::SBFrame Session::frame_for(const FrameRef& where) {
    lldb::SBProcess process = require_stopped_process();
    lldb::SBThread thread = thread_for(process, where.thread);
    if (where.frame) {
        if (*where.frame >= thread.GetNumFrames()) {
            throw ToolError("Thread #" + std::to_string(thread.GetIndexID()) + " has " +
                            std::to_string(thread.GetNumFrames()) + " frames; frame " +
                            std::to_string(*where.frame) + " does not exist");
        }
        return thread.GetFrameAtIndex(*where.frame);
    }
    lldb::SBFrame frame = thread.GetSelectedFrame();
    if (!frame.IsValid()) {
        frame = thread.GetFrameAtIndex(0);
    }
    if (!frame.IsValid()) {
        throw ToolError("Thread #" + std::to_string(thread.GetIndexID()) + " has no frames");
    }
    return frame;
}

void Session::adopt_selected_target() {
    // The command tool can create or select another target; follow it.
    lldb::SBTarget selected = debugger_.GetSelectedTarget();
    if (selected.IsValid() && selected != target_) {
        target_ = selected;
        events_hooked_ = false;
        attached_ = false;
        core_ = false;
        core_path_.reset();
        running_ = false;
    }
    if (target_.IsValid()) {
        lldb::SBProcess process = target_.GetProcess();
        if (process.IsValid() && !running_) {
            const auto state = process.GetState();
            if (state == lldb::eStateRunning || state == lldb::eStateStepping) {
                running_ = true;
                resume_stop_id_ = process.GetStopID();
            }
        }
    }
}

void Session::replace_target(lldb::SBTarget target) {
    end_process();
    if (target_.IsValid() && target_ != target) {
        debugger_.DeleteTarget(target_);
    }
    target_ = target;
    debugger_.SetSelectedTarget(target_);
    attached_ = false;
    core_ = false;
    core_path_.reset();
    output_.clear();
    reported_ = 0;
    output_dropped_ = false;
    pending_cr_ = false;
}

void Session::end_process() {
    if (!target_.IsValid()) {
        return;
    }
    lldb::SBProcess process = target_.GetProcess();
    if (process.IsValid() && !is_finished(process.GetState())) {
        if (attached_) {
            (void)process.Detach();
        } else {
            (void)process.Kill();
        }
    }
    lldb::SBEvent event;
    while (listener_.GetNextEvent(event)) {
    }
    running_ = false;
}

nlohmann::json Session::create_target(const std::string& program,
                                      const std::optional<std::string>& arch) {
    lldb::SBError error;
    lldb::SBTarget target =
        debugger_.CreateTarget(program.c_str(), arch ? arch->c_str() : nullptr, nullptr, true, error);
    if (error.Fail() || !target.IsValid()) {
        throw ToolError("Could not load " + program + ": " + error_text(error, "unknown error"));
    }
    replace_target(target);

    nlohmann::json out;
    out["executable"] = path_of(target_.GetExecutable());
    out["triple"] = text_of(target_.GetTriple());
    out["modules"] = target_.GetNumModules();
    lldb::SBModule main_module = target_.FindModule(target_.GetExecutable());
    out["has_debug_info"] = main_module.IsValid() && main_module.GetNumCompileUnits() > 0;
    return out;
}

nlohmann::json Session::load_core(const std::string& program, const std::string& core) {
    create_target(program, std::nullopt);
    lldb::SBError error;
    events_hooked_ = false;
    lldb::SBProcess process = target_.LoadCore(core.c_str(), error);
    if (error.Fail() || !process.IsValid()) {
        throw ToolError("Could not load core file " + core + ": " + error_text(error, "unknown error"));
    }
    core_ = true;
    core_path_ = core;
    running_ = false;
    return stop_report();
}

void Session::begin_run(lldb::SBProcess process) {
    resume_stop_id_ = process.GetStopID();
    running_ = true;
}

void Session::listen_to(lldb::SBProcess process) {
    events_hooked_ = true;
    attaching_ = false;
    begin_run(process);
}

void Session::start_launch(const LaunchRequest& request) {
    if (request.program) {
        const bool same_program = target_.IsValid() && !core_ &&
                                  same_file(path_of(target_.GetExecutable()), *request.program);
        if (!same_program) {
            create_target(*request.program, std::nullopt);
        }
    }
    lldb::SBTarget target = require_target();
    if (core_) {
        throw ToolError("The target holds a core file; load the program with create_target to run it");
    }
    end_process();
    output_.clear();
    reported_ = 0;
    output_dropped_ = false;
    pending_cr_ = false;

    std::vector<const char*> argv;
    argv.reserve(request.args.size() + 1);
    for (const auto& arg : request.args) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);
    lldb::SBLaunchInfo info(argv.data());

    const auto environment = launch_environment(request.env);
    std::vector<const char*> envp;
    envp.reserve(environment.size() + 1);
    for (const auto& entry : environment) {
        envp.push_back(entry.c_str());
    }
    envp.push_back(nullptr);
    info.SetEnvironmentEntries(envp.data(), false);

    if (request.cwd) {
        info.SetWorkingDirectory(request.cwd->c_str());
    }
#ifdef _WIN32
    const char* null_device = "NUL";
#else
    const char* null_device = "/dev/null";
#endif
    info.AddOpenFileAction(0, request.stdin_path ? request.stdin_path->c_str() : null_device, true,
                           false);
    std::uint32_t flags = info.GetLaunchFlags();
    if (request.stop_at_entry) {
        flags |= lldb::eLaunchFlagStopAtEntry;
    }
    info.SetLaunchFlags(flags);
    info.SetListener(listener_);

    lldb::SBError error;
    lldb::SBProcess process = target.Launch(info, error);
    if (error.Fail() || !process.IsValid()) {
        throw ToolError("Launch failed: " + error_text(error, "unknown error"));
    }
    attached_ = false;
    listen_to(process);
}

void Session::start_attach(const AttachRequest& request) {
    if (!request.pid && !request.name) {
        throw ToolError("Give pid or name");
    }
    if (!target_.IsValid() || core_) {
        lldb::SBError error;
        lldb::SBTarget target = debugger_.CreateTarget("", nullptr, nullptr, true, error);
        if (!target.IsValid()) {
            throw ToolError("Could not create a target to attach to: " +
                            error_text(error, "unknown error"));
        }
        replace_target(target);
    }
    end_process();
    output_.clear();
    reported_ = 0;
    output_dropped_ = false;
    pending_cr_ = false;

    lldb::SBAttachInfo info = request.pid ? lldb::SBAttachInfo(static_cast<lldb::pid_t>(*request.pid))
                                          : lldb::SBAttachInfo(request.name->c_str(), request.wait_for);
    info.SetListener(listener_);
    lldb::SBError error;
    lldb::SBProcess process = target_.Attach(info, error);
    if (error.Fail() || !process.IsValid()) {
        throw ToolError("Attach failed: " + error_text(error, "unknown error"));
    }
    attached_ = true;
    listen_to(process);
    // LLDB reports the stop that ends an attach through the attach call, not as an event: the
    // process is stopped when Attach() returns, or (waiting for a process to start) once its state
    // says so.
    attaching_ = true;
    if (is_stopped(process.GetState())) {
        attaching_ = false;
        running_ = false;
    }
}

void Session::start_continue() {
    lldb::SBProcess process = require_process();
    if (core_) {
        throw ToolError("A core file cannot run");
    }
    const auto state = process.GetState();
    if (state == lldb::eStateRunning || state == lldb::eStateStepping ||
        state == lldb::eStateLaunching || state == lldb::eStateAttaching) {
        running_ = true;
        return;
    }
    if (!is_stopped(state)) {
        throw ToolError("The process is " + state_name(state) + " and cannot continue");
    }
    begin_run(process);
    const lldb::SBError error = process.Continue();
    if (error.Fail()) {
        running_ = false;
        throw ToolError("Continue failed: " + error_text(error, "unknown error"));
    }
}

void Session::start_step(StepKind kind, std::optional<std::uint32_t> thread_id) {
    lldb::SBProcess process = require_stopped_process();
    if (core_) {
        throw ToolError("A core file cannot run");
    }
    lldb::SBThread thread = thread_for(process, thread_id);
    process.SetSelectedThread(thread);
    begin_run(process);
    lldb::SBError error;
    switch (kind) {
        case StepKind::eOver:
            thread.StepOver(lldb::eOnlyDuringStepping, error);
            break;
        case StepKind::eInto:
            thread.StepInto(nullptr, LLDB_INVALID_LINE_NUMBER, error, lldb::eOnlyDuringStepping);
            break;
        case StepKind::eOut:
            thread.StepOut(error);
            break;
        case StepKind::eInstruction:
            thread.StepInstruction(false, error);
            break;
        case StepKind::eInstructionOver:
            thread.StepInstruction(true, error);
            break;
    }
    if (error.Fail()) {
        running_ = false;
        throw ToolError("Step failed: " + error_text(error, "unknown error"));
    }
}

void Session::start_run_to(const std::string& file, std::uint32_t line) {
    lldb::SBProcess process = require_stopped_process();
    (void)process;
    lldb::SBBreakpoint breakpoint = target_.BreakpointCreateByLocation(file.c_str(), line);
    if (!breakpoint.IsValid() || breakpoint.GetNumLocations() == 0) {
        if (breakpoint.IsValid()) {
            target_.BreakpointDelete(breakpoint.GetID());
        }
        throw ToolError("No code at " + file + ":" + std::to_string(line));
    }
    breakpoint.SetOneShot(true);
    start_continue();
}

void Session::interrupt() {
    lldb::SBProcess process = require_process();
    const auto state = process.GetState();
    if (is_stopped(state)) {
        throw ToolError("The process is already stopped");
    }
    running_ = true;
    const lldb::SBError error = process.Stop();
    if (error.Fail()) {
        throw ToolError("Interrupt failed: " + error_text(error, "unknown error"));
    }
}

void Session::handle_event(const lldb::SBEvent& event) {
    if (!lldb::SBProcess::EventIsProcessEvent(event)) {
        return;
    }
    const std::uint32_t type = event.GetType();
    if ((type & (lldb::SBProcess::eBroadcastBitSTDOUT | lldb::SBProcess::eBroadcastBitSTDERR)) != 0) {
        collect_output();
    }
    if ((type & lldb::SBProcess::eBroadcastBitStateChanged) == 0) {
        return;
    }
    const lldb::StateType state = lldb::SBProcess::GetStateFromEvent(event);
    last_state_ = state;
    if (state == lldb::eStateStopped && lldb::SBProcess::GetRestartedFromEvent(event)) {
        return;
    }
    if (is_stopped(state) || is_finished(state)) {
        running_ = false;
    }
}

bool Session::wait_for_stop(std::uint32_t seconds) {
    if (!target_.IsValid()) {
        return true;
    }
    lldb::SBProcess process = target_.GetProcess();
    if (!process.IsValid()) {
        running_ = false;
        return true;
    }
    const auto settled = [&] {
        if (!running_) {
            return true;
        }
        // The events of a process the command tool started go to LLDB's own listener; its state
        // tells when it stopped. A process the session started reports through the session's
        // listener, whose events also tell a stop from one LLDB resumes at once (a breakpoint
        // whose condition is false), which the state alone does not.
        const auto state = process.GetState();
        if (is_finished(state)) {
            running_ = false;
            return true;
        }
        if (attaching_ && is_stopped(state)) {
            attaching_ = false;
            running_ = false;
            return true;
        }
        if (events_hooked_) {
            return false;
        }
        if (is_stopped(state) && process.GetStopID() != resume_stop_id_) {
            running_ = false;
            return true;
        }
        return false;
    };

    lldb::SBEvent event;
    while (listener_.GetNextEvent(event)) {
        handle_event(event);
    }
    if (settled()) {
        collect_output();
        return true;
    }
    if (seconds > 0 && listener_.WaitForEvent(seconds, event)) {
        handle_event(event);
        while (listener_.GetNextEvent(event)) {
            handle_event(event);
        }
    }
    const bool done = settled();
    collect_output();
    return done;
}

void Session::append_output(const char* data, std::size_t size) {
    // The process writes to a pseudo-terminal, which turns each newline into CR LF: undo that.
    for (std::size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (pending_cr_) {
            pending_cr_ = false;
            if (c != '\n') {
                output_.push_back('\r');
            }
        }
        if (c == '\r') {
            pending_cr_ = true;
        } else {
            output_.push_back(c);
        }
    }
    if (output_.size() > g_OUTPUT_LIMIT) {
        const std::size_t drop = output_.size() - g_OUTPUT_LIMIT;
        if (drop > reported_) {
            output_dropped_ = true;
        }
        reported_ = reported_ > drop ? reported_ - drop : 0;
        output_.erase(0, drop);
    }
}

void Session::collect_output() {
    if (!target_.IsValid()) {
        return;
    }
    lldb::SBProcess process = target_.GetProcess();
    if (!process.IsValid()) {
        return;
    }
    std::array<char, g_READ_CHUNK> buffer{};
    for (std::size_t n = process.GetSTDOUT(buffer.data(), buffer.size()); n > 0;
         n = process.GetSTDOUT(buffer.data(), buffer.size())) {
        append_output(buffer.data(), n);
    }
    for (std::size_t n = process.GetSTDERR(buffer.data(), buffer.size()); n > 0;
         n = process.GetSTDERR(buffer.data(), buffer.size())) {
        append_output(buffer.data(), n);
    }
}

namespace {

void add_new_output(nlohmann::json& out, std::string_view fresh, bool dropped) {
    if (fresh.empty() && !dropped) {
        return;
    }
    if (fresh.size() > g_REPORT_OUTPUT_LIMIT) {
        out["output"] = std::string(fresh.substr(fresh.size() - g_REPORT_OUTPUT_LIMIT));
        out["output_truncated"] = true;
    } else {
        out["output"] = std::string(fresh);
        if (dropped) {
            out["output_truncated"] = true;
        }
    }
}

}  // namespace

nlohmann::json Session::stop_report() {
    collect_output();
    nlohmann::json out;
    if (!target_.IsValid()) {
        out["state"] = "no_target";
        return out;
    }
    lldb::SBProcess process = target_.GetProcess();
    if (!process.IsValid()) {
        out["state"] = "no_process";
        return out;
    }
    const auto state = process.GetState();
    out["state"] = state_name(state);
    out["pid"] = process.GetProcessID();
    if (core_) {
        out["core_file"] = core_path_.value_or("");
    }
    if (state == lldb::eStateExited) {
        out["exit_status"] = process.GetExitStatus();
        if (const char* description = process.GetExitDescription(); description != nullptr) {
            out["exit_description"] = description;
        }
    } else if (is_stopped(state)) {
        lldb::SBThread thread = thread_with_stop_reason(process);
        if (thread.IsValid()) {
            process.SetSelectedThread(thread);
            out["thread"] = thread_json(thread);
        }
        out["threads"] = process.GetNumThreads();
    }
    add_new_output(out, std::string_view(output_).substr(std::min(reported_, output_.size())),
                   output_dropped_);
    reported_ = output_.size();
    output_dropped_ = false;
    return out;
}

nlohmann::json Session::running_report(std::uint32_t waited_seconds) {
    collect_output();
    nlohmann::json out;
    out["state"] = "running";
    if (target_.IsValid() && target_.GetProcess().IsValid()) {
        out["pid"] = target_.GetProcess().GetProcessID();
    }
    out["waited_seconds"] = waited_seconds;
    out["note"] =
        "The process is still running. Call continue to keep waiting for it to stop, or interrupt "
        "to pause it.";
    add_new_output(out, std::string_view(output_).substr(std::min(reported_, output_.size())),
                   output_dropped_);
    reported_ = output_.size();
    output_dropped_ = false;
    return out;
}

nlohmann::json Session::terminate(const std::optional<std::string>& action) {
    lldb::SBTarget target = require_target();
    lldb::SBProcess process = target.GetProcess();
    if (!process.IsValid() || is_finished(process.GetState())) {
        throw ToolError("No process is running");
    }
    if (core_) {
        throw ToolError("A core file has no process to end; create_target replaces it");
    }
    std::string chosen = action.value_or(attached_ ? "detach" : "kill");
    lldb::SBError error;
    if (chosen == "kill") {
        error = process.Kill();
    } else if (chosen == "detach") {
        error = process.Detach();
    } else {
        throw ToolError("action is kill or detach");
    }
    if (error.Fail()) {
        throw ToolError(chosen + " failed: " + error_text(error, "unknown error"));
    }
    running_ = true;
    resume_stop_id_ = process.GetStopID();
    for (std::uint32_t i = 0; i < g_SETTLE_SECONDS && !wait_for_stop(1); ++i) {
    }
    running_ = false;
    return stop_report();
}

nlohmann::json Session::status() {
    adopt_selected_target();
    nlohmann::json out;
    if (!target_.IsValid()) {
        out["target"] = nullptr;
        out["note"] = "No target. Load one with create_target, launch, attach or load_core.";
        return out;
    }
    nlohmann::json target;
    target["executable"] = path_of(target_.GetExecutable());
    target["triple"] = text_of(target_.GetTriple());
    if (core_) {
        target["core_file"] = core_path_.value_or("");
    }
    out["target"] = std::move(target);
    out["breakpoints"] = target_.GetNumBreakpoints();
    out["watchpoints"] = target_.GetNumWatchpoints();

    lldb::SBProcess process = target_.GetProcess();
    if (!process.IsValid()) {
        out["process"] = nullptr;
        return out;
    }
    collect_output();
    nlohmann::json proc;
    const auto state = process.GetState();
    proc["pid"] = process.GetProcessID();
    proc["state"] = state_name(state);
    proc["attached"] = attached_;
    if (state == lldb::eStateExited) {
        proc["exit_status"] = process.GetExitStatus();
    } else if (is_stopped(state)) {
        lldb::SBThread thread = process.GetSelectedThread();
        if (thread.IsValid()) {
            proc["selected_thread"] = thread_json(thread);
        }
        proc["threads"] = process.GetNumThreads();
    }
    proc["output_bytes"] = output_.size();
    out["process"] = std::move(proc);
    return out;
}

nlohmann::json Session::output(std::optional<std::uint32_t> tail_bytes) {
    collect_output();
    const std::size_t limit =
        std::min<std::size_t>(tail_bytes.value_or(g_OUTPUT_TOOL_LIMIT), g_OUTPUT_TOOL_LIMIT);
    nlohmann::json out;
    out["bytes"] = output_.size();
    if (output_.size() > limit) {
        out["output"] = output_.substr(output_.size() - limit);
        out["truncated"] = true;
    } else {
        out["output"] = output_;
    }
    reported_ = output_.size();
    return out;
}

nlohmann::json Session::command(const std::string& command) {
    lldb::SBCommandReturnObject result;
    debugger_.GetCommandInterpreter().HandleCommand(command.c_str(), result);
    adopt_selected_target();
    nlohmann::json out;
    out["succeeded"] = result.Succeeded();
    if (const char* text = result.GetOutput(); text != nullptr && *text != '\0') {
        out["output"] = text;
    }
    if (const char* text = result.GetError(); text != nullptr && *text != '\0') {
        out["error"] = text;
    }
    return out;
}

}  // namespace mcp_lldb
