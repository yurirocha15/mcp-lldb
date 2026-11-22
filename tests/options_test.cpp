#include "options.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace mcp_lldb {
namespace {

ParseResult parse(std::vector<std::string> args) {
    args.insert(args.begin(), "mcp-lldb");
    return parse_options(args);
}

class OptionsTest : public ::testing::Test {
   protected:
    void SetUp() override {
#ifdef _WIN32
        _putenv_s("MCP_LLDB_TOKEN", "");
#else
        unsetenv("MCP_LLDB_TOKEN");
#endif
    }
};

TEST_F(OptionsTest, DefaultsToStdioWithTheCommandTool) {
    const auto result = parse({});
    ASSERT_TRUE(result.options);
    EXPECT_EQ(result.options->transport, Transport::eStdio);
    EXPECT_TRUE(result.options->command_tool);
    EXPECT_EQ(result.options->host, "127.0.0.1");
    EXPECT_EQ(result.options->port, 9695);
}

TEST_F(OptionsTest, ParsesHttpOptions) {
    const auto result = parse({"--transport=http", "--port=8123", "--no-command-tool", "--token=abc"});
    ASSERT_TRUE(result.options);
    EXPECT_EQ(result.options->transport, Transport::eHttp);
    EXPECT_EQ(result.options->port, 8123);
    EXPECT_FALSE(result.options->command_tool);
    EXPECT_EQ(result.options->token, "abc");
}

TEST_F(OptionsTest, RejectsBadValues) {
    for (const auto& args : std::vector<std::vector<std::string>>{{"--transport=ws"},
                                                                  {"--port=0"},
                                                                  {"--port=70000"},
                                                                  {"--port=12x"},
                                                                  {"--bogus"},
                                                                  {"--host="}}) {
        const auto result = parse(args);
        EXPECT_FALSE(result.options) << args.front();
        EXPECT_EQ(result.exit_code, 2) << args.front();
    }
}

TEST_F(OptionsTest, HelpAndVersionExitSuccessfully) {
    const auto help = parse({"--help"});
    EXPECT_FALSE(help.options);
    EXPECT_EQ(help.exit_code, 0);
    EXPECT_NE(help.message.find("--transport"), std::string::npos);

    const auto version = parse({"--version"});
    EXPECT_FALSE(version.options);
    EXPECT_EQ(version.exit_code, 0);
    EXPECT_EQ(version.message.rfind("mcp-lldb ", 0), 0U);
}

TEST_F(OptionsTest, ListeningBeyondLoopbackNeedsAToken) {
    const auto refused = parse({"--transport=http", "--host=0.0.0.0"});
    EXPECT_FALSE(refused.options);
    EXPECT_NE(refused.message.find("token"), std::string::npos);

    const auto allowed = parse({"--transport=http", "--host=0.0.0.0", "--token=secret"});
    EXPECT_TRUE(allowed.options);

    const auto loopback = parse({"--transport=http", "--host=localhost"});
    EXPECT_TRUE(loopback.options);
}

TEST_F(OptionsTest, ReadsTheTokenFromTheEnvironment) {
#ifdef _WIN32
    _putenv_s("MCP_LLDB_TOKEN", "from-env");
#else
    setenv("MCP_LLDB_TOKEN", "from-env", 1);
#endif
    const auto result = parse({"--transport=http", "--host=0.0.0.0"});
    ASSERT_TRUE(result.options);
    EXPECT_EQ(result.options->token, "from-env");
}

}  // namespace
}  // namespace mcp_lldb
