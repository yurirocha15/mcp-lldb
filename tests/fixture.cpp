#include "fixture.hpp"

#include <fstream>
#include <stdexcept>

namespace mcp_lldb::test {

std::uint32_t line_of(const std::string& name) {
    std::ifstream in(sample_source());
    const std::string marker = "// MARK:" + name;
    std::string text;
    std::uint32_t number = 0;
    while (std::getline(in, text)) {
        ++number;
        const auto at = text.find(marker);
        if (at != std::string::npos && at + marker.size() == text.size()) {
            return number;
        }
    }
    throw std::runtime_error("No line marked " + name + " in " + sample_source());
}

}  // namespace mcp_lldb::test
