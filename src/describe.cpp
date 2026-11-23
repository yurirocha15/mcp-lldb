#include "describe.hpp"

#include <array>
#include <cstdio>

namespace mcp_lldb {

namespace {

constexpr std::size_t g_PATH_BUFFER = 4096;
constexpr std::size_t g_STOP_DESCRIPTION_BUFFER = 1024;

nlohmann::json value_json_at(lldb::SBValue value, const ValueLimits& limits, std::uint32_t depth,
                             std::uint32_t& nodes) {
    ++nodes;
    nlohmann::json out;
    out["name"] = text_of(value.GetName());
    const char* type = value.GetDisplayTypeName();
    out["type"] = text_of(type != nullptr ? type : value.GetTypeName());
    if (const char* text = value.GetValue(); text != nullptr) {
        out["value"] = text;
    }
    if (const char* summary = value.GetSummary(); summary != nullptr) {
        out["summary"] = summary;
    }
    if (const lldb::SBError error = value.GetError(); error.Fail()) {
        out["error"] = text_of(error.GetCString());
        return out;
    }
    if (!value.MightHaveChildren()) {
        return out;
    }
    // A null pointer has no children to expand.
    if (value.GetType().IsPointerType() && value.GetValueAsUnsigned(1) == 0) {
        return out;
    }
    const std::uint32_t count = value.GetNumChildren(limits.max_children + 1);
    if (count == 0) {
        return out;
    }
    if (depth >= limits.depth || nodes >= limits.max_nodes) {
        out["has_children"] = true;
        return out;
    }
    nlohmann::json children = nlohmann::json::array();
    const std::uint32_t shown = std::min(count, limits.max_children);
    for (std::uint32_t i = 0; i < shown; ++i) {
        if (nodes >= limits.max_nodes) {
            out["more_children"] = true;
            break;
        }
        lldb::SBValue child = value.GetChildAtIndex(i);
        if (child.IsValid()) {
            children.push_back(value_json_at(child, limits, depth + 1, nodes));
        }
    }
    out["children"] = std::move(children);
    if (count > limits.max_children) {
        out["more_children"] = true;
    }
    return out;
}

}  // namespace

std::string text_of(const char* text) { return text != nullptr ? std::string(text) : std::string(); }

std::string hex_address(std::uint64_t address) {
    std::array<char, 24> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "0x%016llx", static_cast<unsigned long long>(address));
    return buffer.data();
}

std::string path_of(const lldb::SBFileSpec& file) {
    if (!file.IsValid()) {
        return {};
    }
    std::array<char, g_PATH_BUFFER> buffer{};
    const auto length = file.GetPath(buffer.data(), buffer.size());
    return length > 0 ? std::string(buffer.data()) : std::string();
}

std::string state_name(lldb::StateType state) {
    switch (state) {
        case lldb::eStateInvalid:
            return "invalid";
        case lldb::eStateUnloaded:
            return "unloaded";
        case lldb::eStateConnected:
            return "connected";
        case lldb::eStateAttaching:
            return "attaching";
        case lldb::eStateLaunching:
            return "launching";
        case lldb::eStateStopped:
            return "stopped";
        case lldb::eStateRunning:
            return "running";
        case lldb::eStateStepping:
            return "stepping";
        case lldb::eStateCrashed:
            return "crashed";
        case lldb::eStateDetached:
            return "detached";
        case lldb::eStateExited:
            return "exited";
        case lldb::eStateSuspended:
            return "suspended";
    }
    return "unknown";
}

std::string stop_reason_name(lldb::StopReason reason) {
    switch (reason) {
        case lldb::eStopReasonInvalid:
            return "invalid";
        case lldb::eStopReasonNone:
            return "none";
        case lldb::eStopReasonTrace:
            return "trace";
        case lldb::eStopReasonBreakpoint:
            return "breakpoint";
        case lldb::eStopReasonWatchpoint:
            return "watchpoint";
        case lldb::eStopReasonSignal:
            return "signal";
        case lldb::eStopReasonException:
            return "exception";
        case lldb::eStopReasonExec:
            return "exec";
        case lldb::eStopReasonPlanComplete:
            return "step_complete";
        case lldb::eStopReasonThreadExiting:
            return "thread_exiting";
        case lldb::eStopReasonInstrumentation:
            return "instrumentation";
        default:
            return "other";
    }
}

bool is_finished(lldb::StateType state) {
    return state == lldb::eStateExited || state == lldb::eStateDetached ||
           state == lldb::eStateInvalid || state == lldb::eStateUnloaded;
}

bool is_stopped(lldb::StateType state) {
    return state == lldb::eStateStopped || state == lldb::eStateCrashed ||
           state == lldb::eStateSuspended;
}

nlohmann::json frame_json(lldb::SBFrame frame) {
    nlohmann::json out;
    out["index"] = frame.GetFrameID();
    out["pc"] = hex_address(frame.GetPC());
    const char* function = frame.GetDisplayFunctionName();
    out["function"] = text_of(function != nullptr ? function : frame.GetFunctionName());
    if (frame.IsInlined()) {
        out["inlined"] = true;
    }
    if (const lldb::SBModule module = frame.GetModule(); module.IsValid()) {
        out["module"] = text_of(module.GetFileSpec().GetFilename());
    }
    if (const lldb::SBLineEntry line = frame.GetLineEntry(); line.IsValid() && line.GetLine() != 0) {
        out["file"] = path_of(line.GetFileSpec());
        out["line"] = line.GetLine();
        if (line.GetColumn() != 0) {
            out["column"] = line.GetColumn();
        }
    }
    return out;
}

nlohmann::json thread_json(lldb::SBThread thread) {
    nlohmann::json out;
    out["thread"] = thread.GetIndexID();
    out["tid"] = thread.GetThreadID();
    if (const char* name = thread.GetName(); name != nullptr) {
        out["name"] = name;
    }
    if (const char* queue = thread.GetQueueName(); queue != nullptr) {
        out["queue"] = queue;
    }
    const lldb::StopReason reason = thread.GetStopReason();
    out["stop_reason"] = stop_reason_name(reason);
    if (reason != lldb::eStopReasonNone && reason != lldb::eStopReasonInvalid) {
        std::array<char, g_STOP_DESCRIPTION_BUFFER> buffer{};
        thread.GetStopDescription(buffer.data(), buffer.size());
        if (buffer[0] != '\0') {
            out["stop_description"] = buffer.data();
        }
    }
    if (thread.GetNumFrames() > 0) {
        out["frame"] = frame_json(thread.GetFrameAtIndex(0));
    }
    return out;
}

nlohmann::json value_json(lldb::SBValue value, const ValueLimits& limits, std::uint32_t& nodes) {
    return value_json_at(value, limits, 0, nodes);
}

nlohmann::json value_json(lldb::SBValue value, const ValueLimits& limits) {
    std::uint32_t nodes = 0;
    return value_json_at(value, limits, 0, nodes);
}

}  // namespace mcp_lldb
