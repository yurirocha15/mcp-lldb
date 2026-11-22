find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0)
  set(INSTALL_GTEST
      OFF
      CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
find_package(Threads REQUIRED)

# The program the tests debug, built without optimization and with debug
# information whatever the build type.
add_executable(sample-program fixtures/sample.cpp)
target_link_libraries(sample-program PRIVATE Threads::Threads)
if(NOT MSVC)
  target_compile_options(sample-program PRIVATE -O0 -g)
endif()

set(_fixture_definitions
    MCP_LLDB_SAMPLE_PROGRAM="$<TARGET_FILE:sample-program>"
    MCP_LLDB_SAMPLE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sample.cpp"
    MCP_LLDB_BINARY="$<TARGET_FILE:mcp-lldb>"
    MCP_LLDB_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}")

add_executable(mcp-lldb-tests options_test.cpp session_test.cpp server_test.cpp
                              fixture.cpp)
target_link_libraries(mcp-lldb-tests PRIVATE mcp-lldb-core GTest::gtest_main)
target_compile_definitions(mcp-lldb-tests PRIVATE ${_fixture_definitions})
add_dependencies(mcp-lldb-tests sample-program mcp-lldb)
if(LLDB_LIBRARY_DIR AND NOT WIN32)
  set_target_properties(mcp-lldb-tests PROPERTIES BUILD_RPATH
                                                  "${LLDB_LIBRARY_DIR}")
endif()

include(GoogleTest)
gtest_discover_tests(
  mcp-lldb-tests DISCOVERY_TIMEOUT 60
  PROPERTIES
  TIMEOUT 120)
