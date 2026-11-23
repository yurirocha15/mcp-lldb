// Tests of Session with the installed LLDB and the sample program in fixtures/.

#include "session.hpp"
#include "fixture.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <thread>

#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#endif
#ifdef __APPLE__
#include <crt_externs.h>
#endif

namespace mcp_lldb {
namespace {

#ifndef _WIN32
/// The environment of this process, which macOS does not declare as environ.
char** process_environment() {
#ifdef __APPLE__
    return *_NSGetEnviron();
#else
    return environ;
#endif
}
#endif

using nlohmann::json;
using test::line_of;
using test::sample_program;

class SessionTest : public ::testing::Test {
   protected:
    static void SetUpTestSuite() { library_ = std::make_unique<LldbLibrary>(); }
    static void TearDownTestSuite() { library_.reset(); }

    void SetUp() override { session_ = std::make_unique<Session>(); }
    void TearDown() override { session_.reset(); }

    /// Waits up to @p seconds for the process to stop and returns the stop report.
    json wait(std::uint32_t seconds = 30) {
        for (std::uint32_t i = 0; i < seconds; ++i) {
            if (session_->wait_for_stop(1)) {
                return session_->stop_report();
            }
        }
        ADD_FAILURE() << "The process did not stop within " << seconds << " seconds";
        return session_->running_report(seconds);
    }

    json launch(std::vector<std::string> args = {}) {
        LaunchRequest request;
        request.program = sample_program();
        request.args = std::move(args);
        session_->start_launch(request);
        return wait();
    }

    void break_at(const std::string& mark) {
        BreakpointRequest request;
        request.file = "sample.cpp";
        request.line = line_of(mark);
        const json breakpoint = session_->set_breakpoint(request);
        ASSERT_FALSE(breakpoint["locations"].empty()) << breakpoint.dump();
    }

    static std::string frame_function(const json& report) {
        return report.at("thread").at("frame").at("function").get<std::string>();
    }

    static std::unique_ptr<LldbLibrary> library_;
    std::unique_ptr<Session> session_;
};

std::unique_ptr<LldbLibrary> SessionTest::library_;

TEST_F(SessionTest, CreateTargetFindsDebugInformation) {
    const json target = session_->create_target(sample_program(), std::nullopt);
    EXPECT_TRUE(target["has_debug_info"].get<bool>());
    EXPECT_TRUE(std::filesystem::equivalent(target["executable"].get<std::string>(), sample_program()));
    EXPECT_FALSE(target["triple"].get<std::string>().empty());
}

TEST_F(SessionTest, CreateTargetReportsAMissingProgram) {
    EXPECT_THROW(session_->create_target("/no/such/program", std::nullopt), ToolError);
}

TEST_F(SessionTest, InspectionWithoutAProcessExplainsWhatIsMissing) {
    try {
        session_->threads();
        FAIL() << "threads() succeeded without a target";
    } catch (const ToolError& error) {
        EXPECT_NE(std::string(error.what()).find("No target"), std::string::npos);
    }
    session_->create_target(sample_program(), std::nullopt);
    try {
        session_->threads();
        FAIL() << "threads() succeeded without a process";
    } catch (const ToolError& error) {
        EXPECT_NE(std::string(error.what()).find("No process"), std::string::npos);
    }
}

TEST_F(SessionTest, LaunchStopsAtAFunctionBreakpointWithArguments) {
    session_->create_target(sample_program(), std::nullopt);
    BreakpointRequest request;
    request.function = "add";
    // System libraries can have methods named add too (macOS's libobjc does).
    request.module = std::filesystem::path(sample_program()).filename().string();
    const json breakpoint = session_->set_breakpoint(request);
    EXPECT_EQ(breakpoint["locations"].size(), 1U) << breakpoint.dump();

    const json report = launch();
    EXPECT_EQ(report["state"], "stopped") << report.dump();
    EXPECT_EQ(report["thread"]["stop_reason"], "breakpoint") << report.dump();
    EXPECT_NE(frame_function(report).find("add"), std::string::npos) << report.dump();
    EXPECT_EQ(report["output"], "starting normal\n") << report.dump();

    const json vars = session_->variables({}, "args", 1, 32, std::nullopt);
    ASSERT_EQ(vars["variables"].size(), 2U) << vars.dump();
    EXPECT_EQ(vars["variables"][0]["name"], "a");
    EXPECT_EQ(vars["variables"][0]["value"], "3");
    EXPECT_EQ(vars["variables"][1]["name"], "b");
    EXPECT_EQ(vars["variables"][1]["value"], "4");
}

TEST_F(SessionTest, AModuleLimitsWhereAFunctionBreakpointResolves) {
    session_->create_target(sample_program(), std::nullopt);
    BreakpointRequest request;
    request.function = "add";
    request.module = "libnot-loaded.so";
    const json breakpoint = session_->set_breakpoint(request);
    EXPECT_TRUE(breakpoint["locations"].empty()) << breakpoint.dump();
}

TEST_F(SessionTest, LaunchKeepsBreakpointsOfTheLoadedProgram) {
    session_->create_target(sample_program(), std::nullopt);
    break_at("add_body");
    const json report = launch();
    EXPECT_EQ(report["thread"]["frame"]["line"], line_of("add_body")) << report.dump();
}

TEST_F(SessionTest, ContinueRunsToExitWithStatusAndOutput) {
    session_->create_target(sample_program(), std::nullopt);
    const json report = launch();
    EXPECT_EQ(report["state"], "exited") << report.dump();
    EXPECT_EQ(report["exit_status"], 7) << report.dump();
    EXPECT_EQ(report["output"], "starting normal\ntotal 7 counter 3 size 3\n") << report.dump();
    EXPECT_EQ(session_->output(std::nullopt)["output"], "starting normal\ntotal 7 counter 3 size 3\n");
}

TEST_F(SessionTest, StepsOverIntoAndOut) {
    session_->create_target(sample_program(), std::nullopt);
    break_at("call_add");
    json report = launch();
    ASSERT_EQ(report["thread"]["frame"]["line"], line_of("call_add")) << report.dump();

    session_->start_step(StepKind::eInto, std::nullopt);
    report = wait();
    EXPECT_NE(frame_function(report).find("add"), std::string::npos) << report.dump();
    EXPECT_EQ(report["thread"]["frame"]["line"], line_of("add_body")) << report.dump();

    session_->start_step(StepKind::eOut, std::nullopt);
    report = wait();
    EXPECT_NE(frame_function(report).find("main"), std::string::npos) << report.dump();

    session_->start_step(StepKind::eOver, std::nullopt);
    report = wait();
    EXPECT_EQ(report["state"], "stopped") << report.dump();
    EXPECT_GT(report["thread"]["frame"]["line"].get<std::uint32_t>(), line_of("call_add"))
        << report.dump();
}

TEST_F(SessionTest, ConditionalBreakpointStopsOnlyWhenTrue) {
    session_->create_target(sample_program(), std::nullopt);
    BreakpointRequest request;
    request.file = "sample.cpp";
    request.line = line_of("bump_body");
    request.condition = "i == 2";
    session_->set_breakpoint(request);
    launch();
    const json i = session_->variables({}, "locals", 0, 32, std::string("i"));
    EXPECT_EQ(i["variable"]["value"], "2") << i.dump();
}

TEST_F(SessionTest, ShowsStructsAndContainers) {
    session_->create_target(sample_program(), std::nullopt);
    break_at("call_add");
    launch();
    const json origin = session_->variables({}, "locals", 1, 32, std::string("origin"));
    const json& members = origin["variable"]["children"];
    ASSERT_EQ(members.size(), 2U) << origin.dump();
    EXPECT_EQ(members[0]["name"], "x");
    EXPECT_EQ(members[0]["value"], "3");

    const json values = session_->variables({}, "locals", 1, 32, std::string("values"));
    EXPECT_EQ(values["variable"]["children"].size(), 3U) << values.dump();

    const json shallow = session_->variables({}, "locals", 0, 32, std::string("origin"));
    EXPECT_TRUE(shallow["variable"]["has_children"].get<bool>()) << shallow.dump();
}

TEST_F(SessionTest, EvaluatesExpressionsInTheFrame) {
    session_->create_target(sample_program(), std::nullopt);
    break_at("call_add");
    launch();
    const json sum = session_->evaluate("origin.x + origin.y", {}, 1, 5000);
    EXPECT_EQ(sum["result"]["value"], "7") << sum.dump();
    const json call = session_->evaluate("add(10, 20)", {}, 1, 5000);
    EXPECT_EQ(call["result"]["value"], "30") << call.dump();
    EXPECT_THROW(session_->evaluate("no_such_name + 1", {}, 1, 5000), ToolError);
}

TEST_F(SessionTest, CrashReportsTheSignalAndTheFaultingFrame) {
    session_->create_target(sample_program(), std::nullopt);
    const json report = launch({"crash"});
    EXPECT_EQ(report["state"], "stopped") << report.dump();
    const std::string reason = report["thread"]["stop_reason"];
    EXPECT_TRUE(reason == "signal" || reason == "exception") << report.dump();
    EXPECT_NE(frame_function(report).find("read_x"), std::string::npos) << report.dump();
    EXPECT_EQ(report["thread"]["frame"]["line"], line_of("crash")) << report.dump();

    const json stack = session_->backtrace(std::nullopt, 10, false);
    ASSERT_GE(stack["frames"].size(), 2U) << stack.dump();
    EXPECT_NE(stack["frames"][1]["function"].get<std::string>().find("main"), std::string::npos)
        << stack.dump();

    const json point = session_->variables({}, "args", 1, 32, std::string("point"));
    EXPECT_EQ(point["variable"]["value"], "0x0000000000000000") << point.dump();
    EXPECT_FALSE(point["variable"].contains("children")) << point.dump();
}

TEST_F(SessionTest, WatchpointStopsWhenAGlobalChanges) {
    session_->create_target(sample_program(), std::nullopt);
    break_at("call_add");
    launch();
    WatchpointRequest watch;
    watch.variable = "g_counter";
    const json watchpoint = session_->set_watchpoint(watch);
    EXPECT_GT(watchpoint["id"].get<int>(), 0) << watchpoint.dump();

    session_->start_continue();
    const json report = wait();
    EXPECT_EQ(report["thread"]["stop_reason"], "watchpoint") << report.dump();
    EXPECT_NE(frame_function(report).find("bump"), std::string::npos) << report.dump();

    const json listed = session_->list_breakpoints();
    EXPECT_EQ(listed["watchpoints"].size(), 1U) << listed.dump();
    session_->delete_watchpoint(watchpoint["id"].get<std::int64_t>());
    EXPECT_TRUE(session_->list_breakpoints()["watchpoints"].empty());
}

TEST_F(SessionTest, ReadsMemoryRegistersCodeAndSource) {
    session_->create_target(sample_program(), std::nullopt);
    break_at("call_add");
    launch();

    const json memory = session_->read_memory("&origin", 8, {});
    EXPECT_EQ(memory["bytes"], 8) << memory.dump();
    EXPECT_NE(memory["dump"].get<std::string>().find("03 00 00 00 04 00 00 00"), std::string::npos)
        << memory.dump();

    const json registers = session_->registers({}, false);
    ASSERT_EQ(registers["register_sets"].size(), 1U) << registers.dump();
    EXPECT_FALSE(registers["register_sets"][0]["registers"].empty());

    const json code = session_->disassemble(std::nullopt, std::nullopt, 16, {});
    bool current = false;
    for (const auto& instruction : code["instructions"]) {
        current = current || instruction.value("current", false);
    }
    EXPECT_TRUE(current) << code.dump();

    const json text = session_->source(std::nullopt, std::nullopt, 2, {});
    EXPECT_NE(text["text"].get<std::string>().find("=>"), std::string::npos) << text.dump();
    EXPECT_NE(text["text"].get<std::string>().find("MARK:call_add"), std::string::npos) << text.dump();
}

TEST_F(SessionTest, LooksUpFunctionsTypesAndAddresses) {
    session_->create_target(sample_program(), std::nullopt);
    const json add = session_->lookup(std::string("add"), std::nullopt);
    ASSERT_FALSE(add["functions"].empty()) << add.dump();
    EXPECT_EQ(add["functions"][0]["line"], line_of("add_body") - 1) << add.dump();

    const json point = session_->lookup(std::string("Point"), std::nullopt);
    ASSERT_FALSE(point["types"].empty()) << point.dump();
    EXPECT_EQ(point["types"][0]["fields"].size(), 2U) << point.dump();

    const json where =
        session_->lookup(std::nullopt, add["functions"][0]["address"].get<std::string>());
    EXPECT_NE(where["function"].get<std::string>().find("add"), std::string::npos) << where.dump();

    EXPECT_THROW(session_->lookup(std::string("no_such_symbol_anywhere"), std::nullopt), ToolError);
    EXPECT_FALSE(session_->modules()["modules"].empty());
}

TEST_F(SessionTest, InterruptPausesARunningProcess) {
    session_->create_target(sample_program(), std::nullopt);
    LaunchRequest request;
    request.args = {"spin"};
    session_->start_launch(request);
    bool stopped = false;
    for (int i = 0; i < 2 && !stopped; ++i) {
        stopped = session_->wait_for_stop(1);
    }
    EXPECT_FALSE(stopped);
    EXPECT_EQ(session_->running_report(2)["state"], "running");

    session_->interrupt();
    const json report = wait(10);
    EXPECT_EQ(report["state"], "stopped") << report.dump();

    const json ended = session_->terminate(std::nullopt);
    EXPECT_EQ(ended["state"], "exited") << ended.dump();
}

TEST_F(SessionTest, BacktraceOfAllThreads) {
    session_->create_target(sample_program(), std::nullopt);
    LaunchRequest request;
    request.args = {"threads"};
    session_->start_launch(request);
    for (int i = 0; i < 2; ++i) {
        session_->wait_for_stop(1);
    }
    session_->interrupt();
    wait(10);
    const json stacks = session_->backtrace(std::nullopt, 50, true);
    EXPECT_GE(stacks["threads"].size(), 2U) << stacks.dump();
    EXPECT_GE(session_->threads()["threads"].size(), 2U);
}

TEST_F(SessionTest, BreakpointsCanBeListedAndDeleted) {
    session_->create_target(sample_program(), std::nullopt);
    BreakpointRequest request;
    request.function = "bump";
    request.one_shot = true;
    request.ignore_count = 0;
    const json breakpoint = session_->set_breakpoint(request);
    const json listed = session_->list_breakpoints();
    ASSERT_EQ(listed["breakpoints"].size(), 1U);
    EXPECT_TRUE(listed["breakpoints"][0]["one_shot"].get<bool>());
    session_->delete_breakpoint(breakpoint["id"].get<std::int64_t>());
    EXPECT_TRUE(session_->list_breakpoints()["breakpoints"].empty());
    EXPECT_THROW(session_->delete_breakpoint(999), ToolError);

    BreakpointRequest pending;
    pending.function = "function_that_does_not_exist";
    EXPECT_TRUE(session_->set_breakpoint(pending)["pending"].get<bool>());
}

TEST_F(SessionTest, RunToStopsAtTheLine) {
    session_->create_target(sample_program(), std::nullopt);
    LaunchRequest request;
    request.stop_at_entry = true;
    session_->start_launch(request);
    json report = wait();
    ASSERT_EQ(report["state"], "stopped") << report.dump();
    session_->start_run_to("sample.cpp", line_of("report"));
    report = wait();
    EXPECT_EQ(report["thread"]["frame"]["line"], line_of("report")) << report.dump();
    EXPECT_TRUE(session_->list_breakpoints()["breakpoints"].empty());
}

#ifndef _WIN32
/// The sample program in "wait" mode, started outside the debugger and killed on destruction.
class WaitingProcess {
   public:
    WaitingProcess() {
        char* const argv[] = {const_cast<char*>(sample_program()), const_cast<char*>("wait"), nullptr};
        if (posix_spawn(&pid_, sample_program(), nullptr, nullptr, argv, process_environment()) != 0) {
            pid_ = 0;
        }
        // Give it time to reach its loop and allow tracing.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    ~WaitingProcess() {
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
        }
    }
    WaitingProcess(const WaitingProcess&) = delete;
    WaitingProcess& operator=(const WaitingProcess&) = delete;
    [[nodiscard]] pid_t pid() const { return pid_; }
    [[nodiscard]] bool alive() const { return pid_ > 0 && kill(pid_, 0) == 0; }

   private:
    pid_t pid_ = 0;
};

bool has_spin_frame(const json& stack) {
    for (const auto& frame : stack["frames"]) {
        if (frame.value("function", "").find("spin") != std::string::npos) {
            return true;
        }
    }
    return false;
}

TEST_F(SessionTest, AttachesToARunningProcessAndDetaches) {
    WaitingProcess process;
    ASSERT_GT(process.pid(), 0);
    AttachRequest request;
    request.pid = static_cast<std::uint64_t>(process.pid());
    session_->start_attach(request);
    const json report = wait(20);
    EXPECT_EQ(report["state"], "stopped") << report.dump();
    EXPECT_EQ(report["pid"], process.pid()) << report.dump();

    const json stack = session_->backtrace(std::nullopt, 32, true)["threads"][0];
    EXPECT_TRUE(has_spin_frame(stack)) << stack.dump();

    const json ended = session_->terminate(std::nullopt);
    EXPECT_EQ(ended["state"], "detached") << ended.dump();
    EXPECT_TRUE(process.alive());
}

/// Writes a core file of @p process: with LLDB's process save-core where this LLDB can (macOS,
/// newer LLVM releases on Linux), else with gdb's gcore. Returns its path, or an empty string.
std::string write_core(const WaitingProcess& process) {
    const std::string base = std::string(MCP_LLDB_TEST_DIR) + "/sample-core";
    {
        Session saver;
        AttachRequest request;
        request.pid = static_cast<std::uint64_t>(process.pid());
        saver.start_attach(request);
        for (int i = 0; i < 20 && !saver.wait_for_stop(1); ++i) {
        }
        const std::string core = base + ".lldb";
        std::filesystem::remove(core);
        const json saved = saver.command("process save-core " + core);
        saver.terminate("detach");
        if (saved.value("succeeded", false) && std::filesystem::exists(core)) {
            return core;
        }
    }
    const std::string core = base + "." + std::to_string(process.pid());
    std::filesystem::remove(core);
    const std::string dump =
        "gcore -o " + base + " " + std::to_string(process.pid()) + " >/dev/null 2>&1";
    if (std::system(dump.c_str()) == 0 && std::filesystem::exists(core)) {
        return core;
    }
    return {};
}

TEST_F(SessionTest, LoadsACoreFile) {
    WaitingProcess process;
    ASSERT_GT(process.pid(), 0);
    const std::string core = write_core(process);
    ASSERT_FALSE(core.empty()) << "Neither LLDB nor gcore (from gdb) could write a core file";

    const json report = session_->load_core(sample_program(), core);
    EXPECT_EQ(report["state"], "stopped") << report.dump();
    EXPECT_EQ(report["core_file"], core) << report.dump();
    EXPECT_TRUE(has_spin_frame(session_->backtrace(std::nullopt, 32, false))) << report.dump();
    EXPECT_THROW(session_->start_continue(), ToolError);
    EXPECT_THROW(session_->terminate(std::nullopt), ToolError);
    EXPECT_EQ(session_->status()["target"]["core_file"], core);
    std::filesystem::remove(core);
}
#endif  // _WIN32

TEST_F(SessionTest, CommandRunsLldbCommands) {
    session_->create_target(sample_program(), std::nullopt);
    const json out = session_->command("image list");
    EXPECT_TRUE(out["succeeded"].get<bool>()) << out.dump();
    EXPECT_NE(out["output"].get<std::string>().find("sample-program"), std::string::npos) << out.dump();
    const json bad = session_->command("no-such-command");
    EXPECT_FALSE(bad["succeeded"].get<bool>());
}

}  // namespace
}  // namespace mcp_lldb
