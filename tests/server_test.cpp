// End-to-end tests of the server binary over stdio.

#include "fixture.hpp"

#include <mcp/mcp.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <set>
#include <thread>

namespace mcp_lldb {
namespace {

using nlohmann::json;
using test::line_of;
using test::sample_program;
using test::server_binary;

class ServerTest : public ::testing::Test {
   protected:
    void connect(std::vector<std::string> extra_args = {}) {
        std::vector<std::string> command{server_binary()};
        command.insert(command.end(), extra_args.begin(), extra_args.end());
        client_ = std::make_unique<mcp::Client>(mcp::Endpoint::stdio(command));
        client_->sync().connect("mcp-lldb-test", "1.0.0");
    }

    void TearDown() override { client_.reset(); }

    json call(const std::string& name, const json& args = json::object()) {
        const mcp::CallToolResult result = client_->sync().call_tool(name, args);
        EXPECT_FALSE(result.isError.value_or(false)) << name << ": " << mcp::result_text(result);
        return result.structuredContent.value_or(json::object());
    }

    std::string call_error(const std::string& name, const json& args = json::object()) {
        const mcp::CallToolResult result = client_->sync().call_tool(name, args);
        EXPECT_TRUE(result.isError.value_or(false)) << name << " succeeded";
        return mcp::result_text(result);
    }

    std::unique_ptr<mcp::Client> client_;
};

TEST_F(ServerTest, ListsEveryToolWithAnnotations) {
    connect();
    const auto listed = client_->sync().list_tools();
    std::set<std::string> names;
    for (const auto& tool : listed.tools) {
        names.insert(tool.name);
        ASSERT_TRUE(tool.annotations.has_value()) << tool.name;
        EXPECT_TRUE(tool.annotations->readOnlyHint.has_value()) << tool.name;
        EXPECT_FALSE(tool.description.value_or("").empty()) << tool.name;
    }
    for (const char* expected :
         {"create_target",  "launch",         "attach",           "continue",
          "step",           "run_to",         "interrupt",        "terminate",
          "status",         "set_breakpoint", "list_breakpoints", "delete_breakpoint",
          "set_watchpoint", "threads",        "backtrace",        "variables",
          "evaluate",       "read_memory",    "registers",        "disassemble",
          "source",         "lookup",         "modules",          "output",
          "command"}) {
        EXPECT_EQ(names.count(expected), 1U) << expected;
    }
}

TEST_F(ServerTest, CommandToolCanBeTurnedOff) {
    connect({"--no-command-tool"});
    for (const auto& tool : client_->sync().list_tools().tools) {
        EXPECT_NE(tool.name, "command");
    }
}

TEST_F(ServerTest, DebugsAProgramOverStdio) {
    connect();
    call("create_target", {{"program", sample_program()}});
    const json breakpoint =
        call("set_breakpoint", {{"file", "sample.cpp"}, {"line", line_of("add_body")}});
    EXPECT_EQ(breakpoint["locations"].size(), 1U) << breakpoint.dump();

    // The program prints before it stops; that output must not reach the server's stdout, which
    // carries MCP messages.
    const json stop = call("launch", {});
    EXPECT_EQ(stop["state"], "stopped") << stop.dump();
    EXPECT_EQ(stop["thread"]["stop_reason"], "breakpoint") << stop.dump();
    EXPECT_EQ(stop["output"], "starting normal\n") << stop.dump();

    const json vars = call("variables", {{"scope", "args"}});
    EXPECT_EQ(vars["variables"].size(), 2U) << vars.dump();

    const json value = call("evaluate", {{"expression", "a * 10 + b"}});
    EXPECT_EQ(value["result"]["value"], "34") << value.dump();

    const json end = call("continue", {});
    EXPECT_EQ(end["state"], "exited") << end.dump();
    EXPECT_EQ(end["exit_status"], 7) << end.dump();
}

TEST_F(ServerTest, WatchpointHitsKeepTheProtocolIntact) {
    // LLDB prints watchpoint hits (old and new value) to its output, which must not be the
    // server's stdout.
    connect();
    call("create_target", {{"program", sample_program()}});
    call("set_breakpoint", {{"file", "sample.cpp"}, {"line", line_of("call_add")}});
    call("launch", {});
    call("set_watchpoint", {{"variable", "g_counter"}});
    const json hit = call("continue", {});
    EXPECT_EQ(hit["thread"]["stop_reason"], "watchpoint") << hit.dump();
    const json status = call("status");
    EXPECT_EQ(status["process"]["state"], "stopped") << status.dump();
}

TEST_F(ServerTest, ReportsMistakesAsToolErrors) {
    connect();
    EXPECT_NE(call_error("threads").find("No target"), std::string::npos);
    EXPECT_NE(call_error("create_target", {{"program", "/no/such/program"}}).find("Could not load"),
              std::string::npos);
}

TEST_F(ServerTest, CancellingContinuePausesTheProcess) {
    connect();
    call("create_target", {{"program", sample_program()}});
    const json running = call("launch", {{"args", {"spin"}}, {"timeout_seconds", 1}});
    ASSERT_EQ(running["state"], "running") << running.dump();

    mcp::CancellationToken token;
    std::promise<void> progressed;
    std::atomic<bool> once{false};
    mcp::RequestOptions options;
    options.cancellation = token;
    options.on_progress = [&](const mcp::ProgressNotificationParams&) {
        if (!once.exchange(true)) {
            progressed.set_value();
        }
    };
    auto waiting = std::async(std::launch::async, [&] {
        try {
            client_->sync().call_tool("continue", {{"timeout_seconds", 600}}, options);
        } catch (const std::exception&) {
            // A cancelled call ends with an exception.
        }
    });
    ASSERT_EQ(progressed.get_future().wait_for(std::chrono::seconds(20)), std::future_status::ready);
    token.cancel("test");
    ASSERT_EQ(waiting.wait_for(std::chrono::seconds(20)), std::future_status::ready);

    // The server pauses the process when the call is cancelled.
    std::string state;
    for (int i = 0; i < 20 && state != "stopped"; ++i) {
        state = call("status")["process"]["state"].get<std::string>();
        if (state != "stopped") {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    EXPECT_EQ(state, "stopped");
}

TEST_F(ServerTest, NotifiesSubscribersWhenBreakpointsChange) {
    connect();
    call("create_target", {{"program", sample_program()}});
    mcp::SubscriptionFilter filter;
    filter.resourceSubscriptions = {"lldb://breakpoints"};
    mcp::Subscription subscription = client_->sync().listen(filter);
    ASSERT_EQ(subscription.acknowledged().resourceSubscriptions, filter.resourceSubscriptions);

    call("set_breakpoint", {{"function", "add"}});
    auto event = std::async(std::launch::async, [&] { return subscription.sync().next(); });
    ASSERT_EQ(event.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    const auto received = event.get();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->method, "notifications/resources/updated");
    EXPECT_EQ(received->params.value("uri", ""), "lldb://breakpoints") << received->params.dump();
    subscription.sync().close();
}

TEST_F(ServerTest, ServesResourcesAndPrompts) {
    connect();
    call("create_target", {{"program", sample_program()}});
    const auto status = client_->sync().read_resource("lldb://status");
    ASSERT_EQ(status.contents.size(), 1U);
    const auto* text = std::get_if<mcp::TextResourceContents>(&status.contents[0]);
    ASSERT_NE(text, nullptr);
    EXPECT_NE(text->text.find("sample-program"), std::string::npos) << text->text;

    const auto prompt = client_->sync().get_prompt("debug_crash", {{"program", "./app"}});
    ASSERT_FALSE(prompt.messages.empty());
}

}  // namespace
}  // namespace mcp_lldb
