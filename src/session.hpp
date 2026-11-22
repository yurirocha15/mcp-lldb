#pragma once

// One debugging session: an LLDB debugger with at most one target and its process.
//
// Session is not thread-safe. The server calls it from one thread only.

#include <lldb/API/LLDB.h>

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mcp_lldb {

/// A request the session cannot carry out. Its message is the tool's error result.
class ToolError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

/// Initializes LLDB for the life of the object. Construct one before any Session.
class LldbLibrary {
   public:
    LldbLibrary();
    ~LldbLibrary();
    LldbLibrary(const LldbLibrary&) = delete;
    LldbLibrary& operator=(const LldbLibrary&) = delete;
};

struct LaunchRequest {
    std::optional<std::string> program;  ///< Replaces the target when given.
    std::vector<std::string> args;
    std::map<std::string, std::string> env;  ///< Added to the server's environment.
    std::optional<std::string> cwd;
    std::optional<std::string> stdin_path;  ///< Standard input; /dev/null when not given.
    bool stop_at_entry = false;
};

struct AttachRequest {
    std::optional<std::uint64_t> pid;
    std::optional<std::string> name;
    bool wait_for = false;  ///< Wait for a process called `name` to start.
};

struct BreakpointRequest {
    std::optional<std::string> file;
    std::optional<std::uint32_t> line;
    std::optional<std::string> function;
    std::optional<std::string> regex;
    std::optional<std::string> address;
    std::optional<std::string> condition;
    std::optional<std::uint32_t> ignore_count;
    bool one_shot = false;
};

struct WatchpointRequest {
    std::optional<std::string> variable;  ///< A variable path in the frame, such as `p->count`.
    std::optional<std::string> address;
    std::uint32_t size = 0;  ///< Bytes to watch at `address`.
    bool read = false;
    bool write = true;
    std::optional<std::uint32_t> thread;
    std::optional<std::uint32_t> frame;
};

enum class StepKind : std::uint8_t {
    eOver,
    eInto,
    eOut,
    eInstruction,
    eInstructionOver,
};

/// Which frame an inspection refers to: a thread index ID (as in "thread #2") and a frame
/// index, the selected thread and its frame 0 when not given.
struct FrameRef {
    std::optional<std::uint32_t> thread;
    std::optional<std::uint32_t> frame;
};

class Session {
   public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Targets.

    /// Loads @p program as the target, replacing the current one (killing a process the session
    /// launched, detaching from one it attached to).
    nlohmann::json create_target(const std::string& program, const std::optional<std::string>& arch);
    /// Loads @p program and the core file @p core, to inspect a crash after the fact.
    nlohmann::json load_core(const std::string& program, const std::string& core);

    // Running. Each start_* call returns once LLDB has the request; wait_for_stop() waits.

    void start_launch(const LaunchRequest& request);
    void start_attach(const AttachRequest& request);
    /// Resumes a stopped process; does nothing to one that is already running.
    void start_continue();
    void start_step(StepKind kind, std::optional<std::uint32_t> thread);
    /// Resumes until @p file:@p line is reached, through a one-shot breakpoint.
    void start_run_to(const std::string& file, std::uint32_t line);
    /// Asks a running process to stop.
    void interrupt();

    /// Waits up to @p seconds for the process to stop or end. Returns whether it has.
    bool wait_for_stop(std::uint32_t seconds);
    /// The process's state after a stop: why and where it stopped, or how it ended, with the
    /// output it wrote since the previous report.
    nlohmann::json stop_report();
    /// The report for a process still running after @p waited_seconds.
    nlohmann::json running_report(std::uint32_t waited_seconds);

    /// Kills or detaches the process: "kill", "detach", or by default whichever undoes how the
    /// session got it.
    nlohmann::json terminate(const std::optional<std::string>& action);
    /// The target, process state, selected location and breakpoint counts.
    nlohmann::json status();

    // Breakpoints and watchpoints.

    nlohmann::json set_breakpoint(const BreakpointRequest& request);
    nlohmann::json list_breakpoints();
    nlohmann::json delete_breakpoint(std::int64_t id);
    nlohmann::json set_watchpoint(const WatchpointRequest& request);
    nlohmann::json delete_watchpoint(std::int64_t id);

    // Inspection.

    nlohmann::json threads();
    nlohmann::json backtrace(std::optional<std::uint32_t> thread, std::uint32_t count,
                             bool all_threads);
    /// Variables of a frame. @p scope is "locals", "args", "statics" or "all".
    nlohmann::json variables(const FrameRef& where, const std::string& scope, std::uint32_t depth,
                             std::uint32_t max_children, const std::optional<std::string>& name);
    nlohmann::json evaluate(const std::string& expression, const FrameRef& where, std::uint32_t depth,
                            std::uint32_t timeout_ms);
    nlohmann::json read_memory(const std::string& address, std::uint32_t size, const FrameRef& where);
    nlohmann::json registers(const FrameRef& where, bool all_sets);
    nlohmann::json disassemble(const std::optional<std::string>& address,
                               const std::optional<std::string>& function, std::uint32_t count,
                               const FrameRef& where);
    nlohmann::json source(const std::optional<std::string>& file, std::optional<std::uint32_t> line,
                          std::uint32_t context, const FrameRef& where);
    nlohmann::json lookup(const std::optional<std::string>& name,
                          const std::optional<std::string>& address);
    nlohmann::json modules();
    /// The process's output: all that is buffered, or the last @p tail_bytes of it.
    nlohmann::json output(std::optional<std::uint32_t> tail_bytes);
    /// Runs @p command in LLDB's command interpreter.
    nlohmann::json command(const std::string& command);

    /// Whether the session has a target.
    [[nodiscard]] bool has_target() const;

   private:
    lldb::SBTarget require_target();
    lldb::SBProcess require_process();
    lldb::SBProcess require_stopped_process();
    lldb::SBThread thread_for(lldb::SBProcess process, std::optional<std::uint32_t> thread);
    lldb::SBFrame frame_for(const FrameRef& where);
    std::uint64_t resolve_address(const std::string& text, const FrameRef& where);

    void replace_target(lldb::SBTarget target);
    void end_process();
    void begin_run(lldb::SBProcess process);
    void listen_to(lldb::SBProcess process);
    void handle_event(const lldb::SBEvent& event);
    void collect_output();
    void append_output(const char* data, std::size_t size);
    void adopt_selected_target();

    lldb::SBDebugger debugger_;
    lldb::SBListener listener_;
    lldb::SBTarget target_;
    bool attached_ = false;
    bool core_ = false;
    /// Whether the process's events reach listener_ (it was launched or attached by the session).
    bool events_hooked_ = false;
    /// Whether an attach has not stopped the process yet.
    bool attaching_ = false;
    /// Whether the last byte of output was a CR not yet known to start a CR LF pair.
    bool pending_cr_ = false;
    std::optional<std::string> core_path_;
    /// The process's stop ID when it was last resumed.
    std::uint32_t resume_stop_id_ = 0;
    /// Whether the process was resumed and its stop has not been reported yet.
    bool running_ = false;
    std::optional<lldb::StateType> last_state_;
    /// Output the process wrote, trimmed from the front past g_OUTPUT_LIMIT bytes.
    std::string output_;
    /// Bytes of output_ already returned by a stop report.
    std::size_t reported_ = 0;
    /// Whether output was trimmed before it was reported.
    bool output_dropped_ = false;
};

}  // namespace mcp_lldb
