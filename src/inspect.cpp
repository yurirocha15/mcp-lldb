// Session members that read a target or a stopped process: threads, frames, values, memory,
// registers, code and symbols, and breakpoints.

#include "describe.hpp"
#include "session.hpp"

#if __has_include(<lldb/API/SBWatchpointOptions.h>)
#include <lldb/API/SBWatchpointOptions.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace mcp_lldb {

namespace {

constexpr std::uint32_t g_MAX_MEMORY_READ = 16384;
constexpr std::uint32_t g_MAX_BACKTRACE = 1000;
constexpr std::uint32_t g_MAX_INSTRUCTIONS = 1024;
constexpr std::uint32_t g_MAX_SOURCE_CONTEXT = 200;
constexpr std::uint32_t g_MAX_LOOKUP_MATCHES = 50;
constexpr std::uint32_t g_MAX_TYPE_FIELDS = 64;
constexpr std::uint32_t g_MAX_LOCATIONS = 20;
constexpr std::uint32_t g_MAX_MODULES = 1000;
constexpr std::uint32_t g_BYTES_PER_LINE = 16;
/// The error code LLDB gives an expression that ran but produced no value, such as a call of a
/// void function (lldb's UserExpression::kNoResult).
constexpr std::uint32_t g_EXPRESSION_NO_RESULT = 0x1001;

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

std::optional<std::uint64_t> parse_number(const std::string& text) {
    if (text.empty() || text.front() == '-') {
        return std::nullopt;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 0);
    if (end == nullptr || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(value);
}

/// The section and offset of @p address: a load address in a process, or a file address before
/// the program runs.
lldb::SBAddress resolve(std::uint64_t address, lldb::SBTarget target) {
    lldb::SBProcess process = target.GetProcess();
    if (process.IsValid() && !is_finished(process.GetState())) {
        return target.ResolveLoadAddress(address);
    }
    return target.ResolveFileAddress(address);
}

std::uint64_t load_address(lldb::SBAddress address, lldb::SBTarget target) {
    const lldb::addr_t loaded = address.GetLoadAddress(target);
    return loaded != LLDB_INVALID_ADDRESS ? loaded : address.GetFileAddress();
}

nlohmann::json location_json(lldb::SBAddress address, lldb::SBTarget target) {
    nlohmann::json out;
    out["address"] = hex_address(load_address(address, target));
    lldb::SBSymbolContext context = address.GetSymbolContext(lldb::eSymbolContextEverything);
    if (const lldb::SBModule module = context.GetModule(); module.IsValid()) {
        out["module"] = text_of(module.GetFileSpec().GetFilename());
    }
    if (lldb::SBFunction function = context.GetFunction(); function.IsValid()) {
        out["function"] = text_of(function.GetDisplayName());
        const std::uint64_t start = load_address(function.GetStartAddress(), target);
        const std::uint64_t here = load_address(address, target);
        if (here > start) {
            out["offset"] = here - start;
        }
    } else if (lldb::SBSymbol symbol = context.GetSymbol(); symbol.IsValid()) {
        out["symbol"] = text_of(symbol.GetDisplayName());
        const std::uint64_t start = load_address(symbol.GetStartAddress(), target);
        const std::uint64_t here = load_address(address, target);
        if (here > start) {
            out["offset"] = here - start;
        }
    }
    if (const lldb::SBLineEntry line = context.GetLineEntry(); line.IsValid() && line.GetLine() != 0) {
        out["file"] = path_of(line.GetFileSpec());
        out["line"] = line.GetLine();
    }
    return out;
}

nlohmann::json breakpoint_json(lldb::SBBreakpoint breakpoint, lldb::SBTarget target) {
    nlohmann::json out;
    out["id"] = breakpoint.GetID();
    out["enabled"] = breakpoint.IsEnabled();
    out["hit_count"] = breakpoint.GetHitCount();
    if (const char* condition = breakpoint.GetCondition(); condition != nullptr && *condition != '\0') {
        out["condition"] = condition;
    }
    if (breakpoint.GetIgnoreCount() != 0) {
        out["ignore_count"] = breakpoint.GetIgnoreCount();
    }
    if (breakpoint.IsOneShot()) {
        out["one_shot"] = true;
    }
    nlohmann::json locations = nlohmann::json::array();
    const std::uint32_t count = static_cast<std::uint32_t>(breakpoint.GetNumLocations());
    for (std::uint32_t i = 0; i < std::min(count, g_MAX_LOCATIONS); ++i) {
        lldb::SBBreakpointLocation location = breakpoint.GetLocationAtIndex(i);
        if (!location.IsValid()) {
            continue;
        }
        nlohmann::json entry = location_json(location.GetAddress(), target);
        entry["resolved"] = location.IsResolved();
        locations.push_back(std::move(entry));
    }
    out["locations"] = std::move(locations);
    if (count > g_MAX_LOCATIONS) {
        out["more_locations"] = count - g_MAX_LOCATIONS;
    }
    if (count == 0) {
        out["pending"] = true;
        out["note"] =
            "No code matches yet; the breakpoint resolves if a library with matching code loads.";
    }
    return out;
}

nlohmann::json instruction_json(lldb::SBInstruction instruction, lldb::SBTarget target,
                                std::uint64_t pc) {
    nlohmann::json out;
    const std::uint64_t address = load_address(instruction.GetAddress(), target);
    out["address"] = hex_address(address);
    std::string text = text_of(instruction.GetMnemonic(target));
    if (const char* operands = instruction.GetOperands(target);
        operands != nullptr && *operands != '\0') {
        text += " ";
        text += operands;
    }
    out["text"] = std::move(text);
    if (const char* comment = instruction.GetComment(target); comment != nullptr && *comment != '\0') {
        out["comment"] = comment;
    }
    if (address == pc) {
        out["current"] = true;
    }
    return out;
}

std::string hex_dump(std::uint64_t base, const std::vector<unsigned char>& bytes) {
    std::ostringstream out;
    std::array<char, 8> byte_text{};
    for (std::size_t offset = 0; offset < bytes.size(); offset += g_BYTES_PER_LINE) {
        out << hex_address(base + offset) << ": ";
        std::string ascii;
        for (std::size_t i = 0; i < g_BYTES_PER_LINE; ++i) {
            if (offset + i < bytes.size()) {
                const unsigned char byte = bytes[offset + i];
                std::snprintf(byte_text.data(), byte_text.size(), "%02x ", byte);
                out << byte_text.data();
                ascii += std::isprint(byte) != 0 ? static_cast<char>(byte) : '.';
            } else {
                out << "   ";
            }
        }
        out << " " << ascii << "\n";
    }
    return out.str();
}

}  // namespace

std::uint64_t Session::resolve_address(const std::string& text, const FrameRef& where) {
    const std::string trimmed = trim(text);
    if (const auto number = parse_number(trimmed)) {
        return *number;
    }
    lldb::SBTarget target = require_target();
    lldb::SBExpressionOptions options;
    options.SetUnwindOnError(true);
    options.SetIgnoreBreakpoints(true);
    lldb::SBValue value;
    lldb::SBProcess process = target.GetProcess();
    if (process.IsValid() && is_stopped(process.GetState())) {
        value = frame_for(where).EvaluateExpression(trimmed.c_str(), options);
    } else {
        value = target.EvaluateExpression(trimmed.c_str(), options);
    }
    if (!value.IsValid() || value.GetError().Fail()) {
        throw ToolError(
            "Cannot resolve address '" + trimmed +
            "': " + text_of(value.IsValid() ? value.GetError().GetCString() : "invalid expression"));
    }
    const auto type_class = value.GetType().GetCanonicalType().GetTypeClass();
    constexpr auto aggregate =
        lldb::eTypeClassArray | lldb::eTypeClassStruct | lldb::eTypeClassClass | lldb::eTypeClassUnion;
    if ((type_class & aggregate) != 0) {
        const lldb::addr_t address = value.GetLoadAddress();
        if (address == LLDB_INVALID_ADDRESS) {
            throw ToolError("'" + trimmed + "' has no address");
        }
        return address;
    }
    return value.GetValueAsUnsigned();
}

nlohmann::json Session::threads() {
    lldb::SBProcess process = require_stopped_process();
    nlohmann::json list = nlohmann::json::array();
    for (std::uint32_t i = 0; i < process.GetNumThreads(); ++i) {
        list.push_back(thread_json(process.GetThreadAtIndex(i)));
    }
    nlohmann::json out;
    out["threads"] = std::move(list);
    if (lldb::SBThread selected = process.GetSelectedThread(); selected.IsValid()) {
        out["selected"] = selected.GetIndexID();
    }
    return out;
}

nlohmann::json Session::backtrace(std::optional<std::uint32_t> thread_id, std::uint32_t count,
                                  bool all_threads) {
    lldb::SBProcess process = require_stopped_process();
    count = std::clamp<std::uint32_t>(count, 1, g_MAX_BACKTRACE);
    const auto one = [&](lldb::SBThread thread) {
        nlohmann::json frames = nlohmann::json::array();
        const std::uint32_t total = thread.GetNumFrames();
        for (std::uint32_t i = 0; i < std::min(total, count); ++i) {
            frames.push_back(frame_json(thread.GetFrameAtIndex(i)));
        }
        nlohmann::json out = thread_json(thread);
        out.erase("frame");
        out["frames"] = std::move(frames);
        out["total_frames"] = total;
        return out;
    };
    if (all_threads) {
        nlohmann::json list = nlohmann::json::array();
        for (std::uint32_t i = 0; i < process.GetNumThreads(); ++i) {
            list.push_back(one(process.GetThreadAtIndex(i)));
        }
        return nlohmann::json{{"threads", std::move(list)}};
    }
    return one(thread_for(process, thread_id));
}

nlohmann::json Session::variables(const FrameRef& where, const std::string& scope, std::uint32_t depth,
                                  std::uint32_t max_children, const std::optional<std::string>& name) {
    lldb::SBFrame frame = frame_for(where);
    const ValueLimits limits{depth, std::max<std::uint32_t>(max_children, 1), 2000};
    nlohmann::json out;
    out["frame"] = frame_json(frame);
    if (name) {
        lldb::SBValue value = frame.GetValueForVariablePath(name->c_str());
        if (!value.IsValid()) {
            value = frame.FindVariable(name->c_str());
        }
        if (!value.IsValid()) {
            throw ToolError("No variable '" + *name + "' in this frame");
        }
        out["variable"] = value_json(value, limits);
        return out;
    }
    const bool arguments = scope == "locals" || scope == "args" || scope == "all";
    const bool locals = scope == "locals" || scope == "all";
    const bool statics = scope == "statics" || scope == "all";
    if (!arguments && !locals && !statics) {
        throw ToolError("scope is locals, args, statics or all");
    }
    const lldb::SBValueList list = frame.GetVariables(arguments, locals, statics, true);
    nlohmann::json values = nlohmann::json::array();
    std::uint32_t nodes = 0;
    for (std::uint32_t i = 0; i < list.GetSize(); ++i) {
        if (nodes >= limits.max_nodes) {
            out["truncated"] = true;
            break;
        }
        values.push_back(value_json(list.GetValueAtIndex(i), limits, nodes));
    }
    out["variables"] = std::move(values);
    return out;
}

nlohmann::json Session::evaluate(const std::string& expression, const FrameRef& where,
                                 std::uint32_t depth, std::uint32_t timeout_ms) {
    lldb::SBTarget target = require_target();
    lldb::SBExpressionOptions options;
    options.SetTimeoutInMicroSeconds(timeout_ms * 1000U);
    options.SetTryAllThreads(true);
    options.SetUnwindOnError(true);
    options.SetIgnoreBreakpoints(true);
    lldb::SBValue value;
    lldb::SBProcess process = target.GetProcess();
    nlohmann::json out;
    out["expression"] = expression;
    if (process.IsValid() && is_stopped(process.GetState())) {
        lldb::SBFrame frame = frame_for(where);
        out["frame"] = frame_json(frame);
        value = frame.EvaluateExpression(expression.c_str(), options);
    } else {
        value = target.EvaluateExpression(expression.c_str(), options);
    }
    if (!value.IsValid()) {
        throw ToolError("The expression produced no value");
    }
    const lldb::SBError error = value.GetError();
    if (error.Fail()) {
        if (error.GetError() == g_EXPRESSION_NO_RESULT) {
            out["result"] = nlohmann::json{{"type", "void"}};
            return out;
        }
        throw ToolError("Expression failed: " + text_of(error.GetCString()));
    }
    out["result"] = value_json(value, ValueLimits{depth, 64, 2000});
    return out;
}

nlohmann::json Session::read_memory(const std::string& address, std::uint32_t size,
                                    const FrameRef& where) {
    lldb::SBProcess process = require_stopped_process();
    const std::uint64_t start = resolve_address(address, where);
    size = std::clamp<std::uint32_t>(size, 1, g_MAX_MEMORY_READ);
    std::vector<unsigned char> bytes(size);
    lldb::SBError error;
    const std::size_t read = process.ReadMemory(start, bytes.data(), bytes.size(), error);
    if (read == 0) {
        throw ToolError("Cannot read memory at " + hex_address(start) + ": " +
                        text_of(error.GetCString()));
    }
    bytes.resize(read);
    nlohmann::json out;
    out["address"] = hex_address(start);
    out["bytes"] = read;
    out["dump"] = hex_dump(start, bytes);
    if (read < size) {
        out["note"] =
            "Only " + std::to_string(read) + " of " + std::to_string(size) + " bytes are readable";
    }
    return out;
}

nlohmann::json Session::registers(const FrameRef& where, bool all_sets) {
    lldb::SBFrame frame = frame_for(where);
    const lldb::SBValueList sets = frame.GetRegisters();
    nlohmann::json out = nlohmann::json::array();
    for (std::uint32_t i = 0; i < sets.GetSize(); ++i) {
        if (!all_sets && i > 0) {
            break;
        }
        lldb::SBValue set = sets.GetValueAtIndex(i);
        nlohmann::json registers = nlohmann::json::object();
        for (std::uint32_t r = 0; r < set.GetNumChildren(); ++r) {
            lldb::SBValue reg = set.GetChildAtIndex(r);
            if (const char* value = reg.GetValue(); value != nullptr) {
                registers[text_of(reg.GetName())] = value;
            }
        }
        out.push_back({{"set", text_of(set.GetName())}, {"registers", std::move(registers)}});
    }
    return nlohmann::json{{"frame", frame_json(frame)}, {"register_sets", std::move(out)}};
}

nlohmann::json Session::disassemble(const std::optional<std::string>& address,
                                    const std::optional<std::string>& function, std::uint32_t count,
                                    const FrameRef& where) {
    lldb::SBTarget target = require_target();
    count = std::clamp<std::uint32_t>(count, 1, g_MAX_INSTRUCTIONS);
    lldb::SBInstructionList instructions;
    std::uint64_t pc = LLDB_INVALID_ADDRESS;
    nlohmann::json out;

    if (function) {
        lldb::SBSymbolContextList matches =
            target.FindFunctions(function->c_str(), lldb::eFunctionNameTypeAuto);
        for (std::uint32_t i = 0; i < matches.GetSize() && !instructions.IsValid(); ++i) {
            lldb::SBSymbolContext context = matches.GetContextAtIndex(i);
            if (lldb::SBFunction fn = context.GetFunction(); fn.IsValid()) {
                instructions = fn.GetInstructions(target);
                out["function"] = text_of(fn.GetDisplayName());
            } else if (lldb::SBSymbol symbol = context.GetSymbol(); symbol.IsValid()) {
                instructions = symbol.GetInstructions(target);
                out["function"] = text_of(symbol.GetDisplayName());
            }
        }
        if (!instructions.IsValid()) {
            throw ToolError("No function named " + *function);
        }
    } else if (address) {
        const std::uint64_t start = resolve_address(*address, where);
        instructions = target.ReadInstructions(resolve(start, target), count);
    } else {
        lldb::SBFrame frame = frame_for(where);
        pc = frame.GetPC();
        out["frame"] = frame_json(frame);
        if (lldb::SBFunction fn = frame.GetFunction(); fn.IsValid()) {
            instructions = fn.GetInstructions(target);
        } else if (lldb::SBSymbol symbol = frame.GetSymbol(); symbol.IsValid()) {
            instructions = symbol.GetInstructions(target);
        }
        if (!instructions.IsValid() || instructions.GetSize() == 0) {
            instructions = target.ReadInstructions(frame.GetPCAddress(), count);
        }
    }

    const std::uint32_t total = static_cast<std::uint32_t>(instructions.GetSize());
    std::uint32_t first = 0;
    if (pc != LLDB_INVALID_ADDRESS && total > count) {
        // Show the instructions around the pc: a quarter before it, the rest after.
        for (std::uint32_t i = 0; i < total; ++i) {
            if (load_address(instructions.GetInstructionAtIndex(i).GetAddress(), target) == pc) {
                first = i > count / 4 ? i - count / 4 : 0;
                break;
            }
        }
        first = std::min(first, total - count);
    }
    nlohmann::json list = nlohmann::json::array();
    for (std::uint32_t i = first; i < std::min(total, first + count); ++i) {
        list.push_back(instruction_json(instructions.GetInstructionAtIndex(i), target, pc));
    }
    out["instructions"] = std::move(list);
    if (first + count < total) {
        out["more_instructions"] = total - (first + count);
    }
    return out;
}

nlohmann::json Session::source(const std::optional<std::string>& file,
                               std::optional<std::uint32_t> line, std::uint32_t context,
                               const FrameRef& where) {
    std::string path;
    std::optional<std::uint32_t> current;
    if (file) {
        path = *file;
        if (!std::ifstream(path).good() && target_.IsValid()) {
            // A file name as debug information records it: find its full path.
            lldb::SBSymbolContextList units =
                target_.FindCompileUnits(lldb::SBFileSpec(path.c_str(), false));
            for (std::uint32_t i = 0; i < units.GetSize(); ++i) {
                const std::string candidate =
                    path_of(units.GetContextAtIndex(i).GetCompileUnit().GetFileSpec());
                if (!candidate.empty() && std::ifstream(candidate).good()) {
                    path = candidate;
                    break;
                }
            }
        }
    } else {
        lldb::SBFrame frame = frame_for(where);
        const lldb::SBLineEntry entry = frame.GetLineEntry();
        if (!entry.IsValid() || entry.GetLine() == 0) {
            throw ToolError("The frame has no source line; disassemble shows its code");
        }
        path = path_of(entry.GetFileSpec());
        current = entry.GetLine();
        if (!line) {
            line = entry.GetLine();
        }
    }
    std::ifstream in(path);
    if (!in) {
        throw ToolError("Cannot read source file " + path);
    }
    context = std::min(context, g_MAX_SOURCE_CONTEXT);
    const std::uint32_t center = line.value_or(context + 1);
    const std::uint32_t first = center > context ? center - context : 1;
    const std::uint32_t last = center + context;
    std::ostringstream text;
    std::string content;
    std::uint32_t number = 0;
    std::array<char, 16> prefix{};
    while (std::getline(in, content)) {
        ++number;
        if (number < first) {
            continue;
        }
        if (number > last) {
            break;
        }
        const bool here = current && *current == number;
        std::snprintf(prefix.data(), prefix.size(), "%s%5u  ", here ? "=>" : "  ", number);
        text << prefix.data() << content << "\n";
    }
    if (number < first) {
        throw ToolError(path + " has only " + std::to_string(number) + " lines");
    }
    nlohmann::json out;
    out["file"] = path;
    out["first_line"] = first;
    out["last_line"] = std::min(last, number);
    out["text"] = text.str();
    return out;
}

nlohmann::json Session::lookup(const std::optional<std::string>& name,
                               const std::optional<std::string>& address) {
    lldb::SBTarget target = require_target();
    if (address) {
        const std::uint64_t value = resolve_address(*address, {});
        return location_json(resolve(value, target), target);
    }
    if (!name) {
        throw ToolError("Give name or address");
    }
    nlohmann::json out;
    nlohmann::json functions = nlohmann::json::array();
    lldb::SBSymbolContextList matches =
        target.FindFunctions(name->c_str(), lldb::eFunctionNameTypeAuto);
    for (std::uint32_t i = 0; i < std::min(matches.GetSize(), g_MAX_LOOKUP_MATCHES); ++i) {
        lldb::SBSymbolContext context = matches.GetContextAtIndex(i);
        if (lldb::SBFunction fn = context.GetFunction(); fn.IsValid()) {
            nlohmann::json entry = location_json(fn.GetStartAddress(), target);
            entry["function"] = text_of(fn.GetDisplayName());
            if (const char* type = fn.GetType().GetDisplayTypeName(); type != nullptr) {
                entry["signature"] = type;
            }
            functions.push_back(std::move(entry));
        } else if (lldb::SBSymbol symbol = context.GetSymbol(); symbol.IsValid()) {
            nlohmann::json entry = location_json(symbol.GetStartAddress(), target);
            entry["symbol"] = text_of(symbol.GetDisplayName());
            functions.push_back(std::move(entry));
        }
    }
    out["functions"] = std::move(functions);

    nlohmann::json globals = nlohmann::json::array();
    const lldb::SBValueList variables = target.FindGlobalVariables(name->c_str(), g_MAX_LOOKUP_MATCHES);
    for (std::uint32_t i = 0; i < variables.GetSize(); ++i) {
        lldb::SBValue value = variables.GetValueAtIndex(i);
        nlohmann::json entry = value_json(value, ValueLimits{0, 1, 1});
        if (const lldb::addr_t loaded = value.GetLoadAddress(); loaded != LLDB_INVALID_ADDRESS) {
            entry["address"] = hex_address(loaded);
        }
        globals.push_back(std::move(entry));
    }
    out["globals"] = std::move(globals);

    nlohmann::json types = nlohmann::json::array();
    lldb::SBTypeList found = target.FindTypes(name->c_str());
    for (std::uint32_t i = 0; i < std::min(found.GetSize(), 10U); ++i) {
        lldb::SBType type = found.GetTypeAtIndex(i);
        nlohmann::json entry;
        entry["name"] = text_of(type.GetDisplayTypeName());
        entry["size"] = type.GetByteSize();
        nlohmann::json fields = nlohmann::json::array();
        for (std::uint32_t f = 0; f < std::min(type.GetNumberOfFields(), g_MAX_TYPE_FIELDS); ++f) {
            lldb::SBTypeMember field = type.GetFieldAtIndex(f);
            fields.push_back({{"name", text_of(field.GetName())},
                              {"type", text_of(field.GetType().GetDisplayTypeName())},
                              {"offset", field.GetOffsetInBytes()}});
        }
        if (!fields.empty()) {
            entry["fields"] = std::move(fields);
        }
        types.push_back(std::move(entry));
    }
    out["types"] = std::move(types);
    if (out["functions"].empty() && out["globals"].empty() && out["types"].empty()) {
        throw ToolError("Nothing named " + *name + " in the target");
    }
    return out;
}

nlohmann::json Session::modules() {
    lldb::SBTarget target = require_target();
    nlohmann::json list = nlohmann::json::array();
    const std::uint32_t count = target.GetNumModules();
    for (std::uint32_t i = 0; i < std::min(count, g_MAX_MODULES); ++i) {
        lldb::SBModule module = target.GetModuleAtIndex(i);
        nlohmann::json entry;
        entry["path"] = path_of(module.GetFileSpec());
        if (const char* uuid = module.GetUUIDString(); uuid != nullptr) {
            entry["uuid"] = uuid;
        }
        entry["has_debug_info"] = module.GetNumCompileUnits() > 0;
        const lldb::addr_t header = module.GetObjectFileHeaderAddress().GetLoadAddress(target);
        if (header != LLDB_INVALID_ADDRESS) {
            entry["load_address"] = hex_address(header);
        }
        list.push_back(std::move(entry));
    }
    nlohmann::json out;
    out["modules"] = std::move(list);
    if (count > g_MAX_MODULES) {
        out["more_modules"] = count - g_MAX_MODULES;
    }
    return out;
}

nlohmann::json Session::set_breakpoint(const BreakpointRequest& request) {
    lldb::SBTarget target = require_target();
    lldb::SBBreakpoint breakpoint;
    if (request.file && request.line) {
        breakpoint = target.BreakpointCreateByLocation(request.file->c_str(), *request.line);
    } else if (request.function) {
        breakpoint = target.BreakpointCreateByName(request.function->c_str(),
                                                   request.module ? request.module->c_str() : nullptr);
    } else if (request.regex) {
        breakpoint = target.BreakpointCreateByRegex(request.regex->c_str(),
                                                    request.module ? request.module->c_str() : nullptr);
    } else if (request.address) {
        breakpoint = target.BreakpointCreateByAddress(resolve_address(*request.address, {}));
    } else {
        throw ToolError("Give file and line, function, regex or address");
    }
    if (!breakpoint.IsValid()) {
        throw ToolError("LLDB did not create the breakpoint");
    }
    if (request.condition) {
        breakpoint.SetCondition(request.condition->c_str());
    }
    if (request.ignore_count) {
        breakpoint.SetIgnoreCount(*request.ignore_count);
    }
    if (request.one_shot) {
        breakpoint.SetOneShot(true);
    }
    return breakpoint_json(breakpoint, target);
}

nlohmann::json Session::list_breakpoints() {
    lldb::SBTarget target = require_target();
    nlohmann::json breakpoints = nlohmann::json::array();
    for (std::uint32_t i = 0; i < target.GetNumBreakpoints(); ++i) {
        breakpoints.push_back(breakpoint_json(target.GetBreakpointAtIndex(i), target));
    }
    nlohmann::json watchpoints = nlohmann::json::array();
    for (std::uint32_t i = 0; i < target.GetNumWatchpoints(); ++i) {
        lldb::SBWatchpoint watchpoint = target.GetWatchpointAtIndex(i);
        nlohmann::json entry;
        entry["id"] = watchpoint.GetID();
        entry["address"] = hex_address(watchpoint.GetWatchAddress());
        entry["size"] = watchpoint.GetWatchSize();
        entry["enabled"] = watchpoint.IsEnabled();
        entry["hit_count"] = watchpoint.GetHitCount();
        if (const char* condition = watchpoint.GetCondition();
            condition != nullptr && *condition != '\0') {
            entry["condition"] = condition;
        }
        watchpoints.push_back(std::move(entry));
    }
    return nlohmann::json{{"breakpoints", std::move(breakpoints)},
                          {"watchpoints", std::move(watchpoints)}};
}

nlohmann::json Session::delete_breakpoint(std::int64_t id) {
    lldb::SBTarget target = require_target();
    if (id <= 0 || !target.BreakpointDelete(static_cast<lldb::break_id_t>(id))) {
        throw ToolError("No breakpoint " + std::to_string(id));
    }
    return nlohmann::json{{"deleted", id}};
}

nlohmann::json Session::set_watchpoint(const WatchpointRequest& request) {
    lldb::SBTarget target = require_target();
    require_stopped_process();
    if (!request.read && !request.write) {
        throw ToolError("Watch reads, writes or both");
    }
    lldb::SBError error;
    lldb::SBWatchpoint watchpoint;
    if (request.variable) {
        lldb::SBFrame frame = frame_for(FrameRef{request.thread, request.frame});
        lldb::SBValue value = frame.GetValueForVariablePath(request.variable->c_str());
        if (!value.IsValid() || value.GetError().Fail()) {
            throw ToolError("No variable '" + *request.variable + "' in this frame");
        }
        watchpoint = value.Watch(true, request.read, request.write, error);
    } else if (request.address) {
        if (request.size == 0) {
            throw ToolError("Give size, the number of bytes to watch (1, 2, 4 or 8)");
        }
        const std::uint64_t address =
            resolve_address(*request.address, FrameRef{request.thread, request.frame});
#if __has_include(<lldb/API/SBWatchpointOptions.h>)
        lldb::SBWatchpointOptions options;
        options.SetWatchpointTypeRead(request.read);
        options.SetWatchpointTypeWrite(request.write ? lldb::eWatchpointWriteTypeOnModify
                                                     : lldb::eWatchpointWriteTypeDisabled);
        watchpoint = target.WatchpointCreateByAddress(address, request.size, options, error);
#else
        watchpoint = target.WatchAddress(address, request.size, request.read, request.write, error);
#endif
    } else {
        throw ToolError("Give variable or address");
    }
    if (error.Fail() || !watchpoint.IsValid()) {
        throw ToolError("Could not set the watchpoint: " +
                        text_of(error.Fail() ? error.GetCString() : "LLDB refused it"));
    }
    nlohmann::json out;
    out["id"] = watchpoint.GetID();
    out["address"] = hex_address(watchpoint.GetWatchAddress());
    out["size"] = watchpoint.GetWatchSize();
    return out;
}

nlohmann::json Session::delete_watchpoint(std::int64_t id) {
    lldb::SBTarget target = require_target();
    if (id <= 0 || !target.DeleteWatchpoint(static_cast<lldb::watch_id_t>(id))) {
        throw ToolError("No watchpoint " + std::to_string(id));
    }
    return nlohmann::json{{"deleted", id}};
}

}  // namespace mcp_lldb
