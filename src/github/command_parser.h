#pragma once

#include <string>
#include <vector>

namespace github {

struct ParsedCommand {
    std::vector<std::string> arguments;
    std::string error;
    bool force_interactive = false;
};

ParsedCommand parse_command(const std::string& input);
bool needs_interactive_terminal(const std::vector<std::string>& arguments);

}  // namespace github