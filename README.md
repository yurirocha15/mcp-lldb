# mcp-lldb

An [MCP](https://modelcontextprotocol.io) server that gives AI agents a debugger. It drives
LLDB through its C++ API: it runs a program, stops it at breakpoints, watchpoints, signals and
crashes, and answers with structured data — stop reasons, stack frames, variables, memory,
registers, disassembly and source.

It is built on [mcp-cpp-sdk](https://github.com/yurirocha15/mcp-cpp-sdk) and links `liblldb`
directly: no Python, and no debugger command-line output to parse.

```text
agent: launch {"program": "./sample-program", "args": ["crash"]}
  ->   {"state": "stopped",
        "thread": {"stop_reason": "signal",
                   "stop_description": "signal SIGSEGV: address not mapped to object (fault address: 0x0)",
                   "frame": {"function": "read_x(Point const*)", "file": ".../sample.cpp", "line": 33}},
        "output": "starting crash\n"}
agent: variables {"scope": "args"}
  ->   {"variables": [{"name": "point", "type": "const Point *", "value": "0x0000000000000000"}]}
```

## Features

- **25 tools** for the whole debugging loop: load a program or a core file, launch or attach,
  breakpoints (file and line, function, regex, address; conditions, ignore counts, one-shot),
  watchpoints, continue, step, run to a line, and inspection of threads, backtraces, variables,
  expressions, memory, registers, disassembly, source, symbols and modules.
- **Execution tools wait for the stop.** `launch`, `continue`, `step` and the others return why
  and where the process stopped, or how it exited, with the output it printed meanwhile. A
  process that keeps running is reported as running after a timeout you choose. While a tool
  waits it reports progress, and cancelling the call pauses the process.
- **Values as data.** Variables come back as typed trees with depth and size limits; containers
  such as `std::vector` show their elements.
- **The program's output stays out of the protocol.** The debuggee writes to its own terminal;
  the server collects that output and returns it in stop reports and through the `output` tool.
- **Resources** (`lldb://status`, `lldb://breakpoints`, `lldb://output`) with change
  notifications, and **prompts** for crash analysis.
- **stdio and Streamable HTTP**, with a bearer token for HTTP.

## Build

Requirements: CMake 3.20 or newer, a C++20 compiler, the LLDB development files (LLVM 18 or
newer), and the dependencies of mcp-cpp-sdk: Boost headers, nlohmann/json and OpenSSL. CMake
downloads mcp-cpp-sdk itself.

Ubuntu 24.04:

```bash
sudo apt install cmake g++ liblldb-18-dev lldb-18 libboost-dev nlohmann-json3-dev libssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

macOS with Homebrew:

```bash
brew install cmake llvm boost nlohmann-json openssl
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLDB_ROOT="$(brew --prefix llvm)" \
      -DOPENSSL_ROOT_DIR="$(brew --prefix openssl)"
cmake --build build
```

The server is `build/mcp-lldb`. CMake looks for LLDB under `LLDB_ROOT`, then in the prefix of
each `llvm-config` it finds (newest first), then in the usual LLVM locations; set `LLDB_ROOT`
to pick one, such as `-DLLDB_ROOT=/usr/lib/llvm-20`.

Without system packages, [Conan](https://conan.io) provides the dependencies (LLDB still comes
from the system):

```bash
conan install . --build=missing -s compiler.cppstd=20
cmake --preset conan-release
cmake --build --preset conan-release
```

## Use it from an MCP client

Claude Code:

```bash
claude mcp add lldb -- /path/to/mcp-lldb
```

Claude Desktop, Cursor and other clients that read an `mcpServers` file:

```json
{
  "mcpServers": {
    "lldb": { "command": "/path/to/mcp-lldb" }
  }
}
```

Over HTTP, start the server and point the client at `http://127.0.0.1:9695/mcp`:

```bash
mcp-lldb --transport=http --port=9695 --token="$(openssl rand -hex 16)"
```

Then ask, for example: *"./build/app crashes when given input.txt. Find out why."* The
`debug_crash`, `analyze_core` and `investigate` prompts start such a session.

## Tools

| Tool | What it does |
|---|---|
| `create_target` | Load a program without running it, or a program and its core file |
| `launch` | Start the program (arguments, environment, working directory, stdin file) and wait for the first stop |
| `attach` | Attach to a process by pid or name, or wait for one to start |
| `continue` | Resume and wait for the next stop or the exit |
| `step` | Step over, into or out, or by machine instruction |
| `run_to` | Continue to a source line |
| `interrupt` | Pause a running process |
| `terminate` | Kill the process or detach from it |
| `status` | The target, the process state and where it stopped |
| `set_breakpoint` | By file and line, function, regex or address; with a condition, ignore count or one-shot |
| `list_breakpoints` | Breakpoints and watchpoints with their locations and hit counts |
| `delete_breakpoint` | Delete a breakpoint or a watchpoint |
| `set_watchpoint` | Stop when a variable or an address range is written or read |
| `threads` | Every thread with its stop reason and location |
| `backtrace` | The call stack of one thread or of all threads |
| `variables` | A frame's arguments, locals and statics, or one variable path |
| `evaluate` | Evaluate an expression in a frame, including function calls |
| `read_memory` | A hex dump of process memory |
| `registers` | General-purpose registers, or every register set |
| `disassemble` | Instructions around the pc, of a function, or from an address |
| `source` | Source lines around the current line or any line of a file |
| `lookup` | Functions, globals and types by name, or the function and line of an address |
| `modules` | The executable and shared libraries, with load addresses |
| `output` | What the program wrote to standard output and standard error |
| `command` | Run any LLDB command line (turned off by `--no-command-tool`) |

Threads are named by LLDB's index IDs (`thread #1`, `#2`...), and frames by their index in the
backtrace. Without `thread` and `frame`, inspection tools use the thread that stopped and its
innermost frame.

## Options

| Option | Default | Meaning |
|---|---|---|
| `--transport=stdio\|http` | `stdio` | How clients connect |
| `--host=<address>` | `127.0.0.1` | HTTP listen address |
| `--port=<n>` | `9695` | HTTP listen port |
| `--token=<token>` | `$MCP_LLDB_TOKEN` | Bearer token HTTP clients must send; required off loopback |
| `--no-command-tool` | | Do not offer the `command` tool |

## Security

The server runs programs, attaches to processes and evaluates expressions with the permissions
of the user who started it: an agent connected to it can run code as that user. Over HTTP it
listens on the loopback interface by default and refuses to listen elsewhere without a token.
Run it only for clients you trust.

## Platforms

Developed and tested on Linux (Ubuntu 24.04 with LLVM 18). CI builds and tests it on Ubuntu with
LLVM 18 and 22 and on macOS with Homebrew's LLVM. Windows is not supported yet.

On Linux, attaching to a process the server did not start needs ptrace permission for it (see
`/proc/sys/kernel/yama/ptrace_scope`).

## Development

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests debug a small program in `tests/fixtures/` with a real LLDB, both through the session
API and through the server binary over stdio. The core-file test writes its core with LLDB where
it can and otherwise with `gcore`, so on Linux with LLVM releases that cannot save cores it needs
gdb installed. Format with `pre-commit run --all-files`.

## License

Apache License 2.0. See [LICENSE](LICENSE).
