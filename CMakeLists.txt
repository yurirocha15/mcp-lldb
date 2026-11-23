cmake_minimum_required(VERSION 3.20)

project(
  mcp-lldb
  VERSION 0.1.0
  DESCRIPTION
    "MCP server that gives AI agents a debugger, through LLDB's C++ API"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      Release
      CACHE STRING "Build type" FORCE)
endif()

option(MCP_LLDB_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
set(MCP_LLDB_SDK_GIT_TAG
    "e7d0758ec8f4a2c901d3d4c9a2b16c300973a2b0"
    CACHE STRING "mcp-cpp-sdk commit or tag to fetch")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(LLDB 18 REQUIRED)

# Link the SDK statically, so the server needs only liblldb at run time.
set(MCP_CPP_SDK_BUILD_SHARED
    OFF
    CACHE BOOL "" FORCE)
include(FetchContent)
FetchContent_Declare(
  mcp-cpp-sdk
  GIT_REPOSITORY https://github.com/yurirocha15/mcp-cpp-sdk.git
  GIT_TAG ${MCP_LLDB_SDK_GIT_TAG})
FetchContent_MakeAvailable(mcp-cpp-sdk)

configure_file(
  src/version.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/mcp_lldb/version.hpp" @ONLY)

add_library(mcp-lldb-core STATIC src/describe.cpp src/inspect.cpp
                                 src/options.cpp src/server.cpp src/session.cpp)
target_include_directories(
  mcp-lldb-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
                       "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(mcp-lldb-core PUBLIC mcp::sdk_static LLDB::LLDB)
# Debian and Ubuntu install liblldb apart from lldb-server, which LLDB uses to
# start processes; pass the server the lldb-server path of the LLVM installation
# it builds against. macOS uses Apple's debugserver, which the server locates at
# run time.
if(NOT APPLE
   AND LLDB_LIBRARY_DIR
   AND EXISTS "${LLDB_LIBRARY_DIR}/../bin/lldb-server")
  get_filename_component(_mcp_lldb_server
                         "${LLDB_LIBRARY_DIR}/../bin/lldb-server" ABSOLUTE)
  target_compile_definitions(mcp-lldb-core
                             PRIVATE MCP_LLDB_SERVER_HINT="${_mcp_lldb_server}")
endif()
if(MSVC)
  target_compile_options(mcp-lldb-core PRIVATE /W4)
else()
  target_compile_options(mcp-lldb-core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(mcp-lldb src/main.cpp)
target_link_libraries(mcp-lldb PRIVATE mcp-lldb-core)

# Find liblldb at run time where it was found at build time, so that a binary
# built against a versioned LLVM directory (such as /usr/lib/llvm-18/lib) runs
# without LD_LIBRARY_PATH.
if(LLDB_LIBRARY_DIR AND NOT WIN32)
  set_target_properties(mcp-lldb PROPERTIES BUILD_RPATH "${LLDB_LIBRARY_DIR}"
                                            INSTALL_RPATH "${LLDB_LIBRARY_DIR}")
endif()

include(GNUInstallDirs)
install(TARGETS mcp-lldb RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(MCP_LLDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
