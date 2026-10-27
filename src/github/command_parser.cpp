#include "github/command_parser.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace github {

ParsedCommand parse_command(const std::string& input) {
    ParsedCommand result;
    std::string command_input = input;
    const auto first_non_space = std::find_if_not(
        command_input.begin(), command_input.end(), [](unsigned char character) {
            return std::isspace(character) != 0;
        });
    if (first_non_space != command_input.end() && *first_non_space == '!') {
        result.force_interactive = true;
        command_input.erase(static_cast<std::size_t>(first_non_space - command_input.begin()), 1);
    }

    std::string token;
    char quote = '\0';
    bool escaped = false;
    bool token_started = false;
    for (const char character : command_input) {
        if (escaped) {
            token.push_back(character);
            escaped = false;
            token_started = true;
            continue;
        }
        if (character == '\\' && quote != '\'') {
            escaped = true;
            token_started = true;
            continue;
        }
        if (quote != '\0') {
            if (character == quote) {
                quote = '\0';
            } else {
                token.push_back(character);
            }
            token_started = true;
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = character;
            token_started = true;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (token_started) {
                result.arguments.push_back(std::move(token));
                token.clear();
                token_started = false;
            }
            continue;
        }
        token.push_back(character);
        token_started = true;
    }

    if (escaped) {
        result.error = "Command cannot end with an escape character.";
        return result;
    }
    if (quote != '\0') {
        result.error = "Unclosed quote in gh command.";
        return result;
    }
    if (token_started) {
        result.arguments.push_back(std::move(token));
    }
    if (!result.arguments.empty() && result.arguments.front() == "gh") {
        result.arguments.erase(result.arguments.begin());
    }
    if (result.arguments.empty()) {
        result.error = "Enter a gh subcommand, for example: pr list.";
        return result;
    }

    result.arguments.insert(result.arguments.begin(), "gh");
    return result;
}

bool needs_interactive_terminal(const std::vector<std::string>& arguments) {
    if (arguments.size() < 3 || arguments[0] != "gh") {
        return false;
    }
    if (arguments[1] == "auth" && arguments[2] == "login") {
        return true;
    }
        return (arguments[1] == "pr" || arguments[1] == "issue" ||
            arguments[1] == "repo" || arguments[1] == "release") &&
           arguments[2] == "create";
}

}  // namespace github