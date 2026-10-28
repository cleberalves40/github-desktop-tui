#include "github/syntax_highlighter.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace github {
namespace {

std::string lowercase(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(static_cast<char>(std::tolower(character)));
    }
    return result;
}

void append(StyledLine& line, std::string_view text, SyntaxStyle style) {
    if (text.empty()) {
        return;
    }
    if (!line.empty() && line.back().style == style) {
        line.back().text.append(text);
    } else {
        line.push_back({std::string(text), style});
    }
}

bool is_identifier_start(unsigned char character) {
    return std::isalpha(character) || character == '_' || character >= 0x80;
}

bool is_identifier_character(unsigned char character) {
    return std::isalnum(character) || character == '_' || character >= 0x80;
}

SyntaxStyle identifier_style(std::string_view identifier) {
    static const std::unordered_set<std::string_view> keywords = {
        "if", "else", "for", "while", "do", "switch", "case", "break",
        "continue", "return", "class", "struct", "enum", "namespace", "using",
        "typedef", "public", "private", "protected", "static", "const", "constexpr",
        "auto", "def", "async", "await", "import", "from", "as", "pass", "raise",
        "try", "except", "finally", "with", "lambda", "yield", "new", "delete",
        "this", "self", "pub", "fn", "let", "mut", "impl", "trait", "mod", "use",
        "crate", "match", "loop", "go", "package", "func", "var", "defer", "interface",
        "type", "map", "chan", "select", "range", "make", "nil", "null", "nullptr",
        "None", "True", "False", "true", "false", "throw", "throws", "catch", "finally",
        "when", "is", "in", "object", "val", "fun", "extends", "implements", "export",
        "default", "async", "await", "yield", "global", "nonlocal", "del", "assert",
    };
    static const std::unordered_set<std::string_view> types = {
        "void", "bool", "boolean", "byte", "short", "int", "long", "float", "double",
        "char", "string", "String", "str", "usize", "isize", "i8", "i16", "i32", "i64",
        "u8", "u16", "u32", "u64", "f32", "f64", "size_t", "BigInt",
    };
    if (types.count(identifier) != 0) {
        return SyntaxStyle::Type;
    }
    return keywords.count(identifier) != 0 ? SyntaxStyle::Keyword : SyntaxStyle::Plain;
}

StyledLine highlight_source_line(std::string_view text, bool& in_block_comment,
                                 char& multiline_quote, std::size_t& multiline_quote_size) {
    StyledLine result;
    std::size_t index = 0;
    while (index < text.size()) {
        if (in_block_comment) {
            const auto end = text.find("*/", index);
            if (end == std::string_view::npos) {
                append(result, text.substr(index), SyntaxStyle::Comment);
                break;
            }
            append(result, text.substr(index, end + 2 - index), SyntaxStyle::Comment);
            index = end + 2;
            in_block_comment = false;
            continue;
        }
        if (multiline_quote != '\0') {
            std::string delimiter(multiline_quote_size, multiline_quote);
            const auto end = text.find(delimiter, index);
            if (end == std::string_view::npos) {
                append(result, text.substr(index), SyntaxStyle::String);
                break;
            }
            append(result, text.substr(index, end + delimiter.size() - index), SyntaxStyle::String);
            index = end + delimiter.size();
            multiline_quote = '\0';
            multiline_quote_size = 0;
            continue;
        }

        if (text.compare(index, 2, "//") == 0 || text[index] == '#') {
            append(result, text.substr(index), SyntaxStyle::Comment);
            break;
        }
        if (text.compare(index, 2, "/*") == 0) {
            in_block_comment = true;
            continue;
        }

        const char character = text[index];
        if (character == '\'' || character == '"' || character == '`') {
            std::size_t delimiter_size = 1;
            if (index + 2 < text.size() && text[index + 1] == character &&
                text[index + 2] == character) {
                delimiter_size = 3;
            }
            std::string delimiter(delimiter_size, character);
            std::size_t end = index + delimiter_size;
            bool escaped = false;
            for (; end < text.size(); ++end) {
                if (text[end] == '\\' && !escaped) {
                    escaped = true;
                    continue;
                }
                if (!escaped && text.compare(end, delimiter_size, delimiter) == 0) {
                    end += delimiter_size;
                    break;
                }
                escaped = false;
            }
            append(result, text.substr(index, end - index), SyntaxStyle::String);
            if (end == text.size() && text.size() - index >= delimiter_size &&
                text.compare(text.size() - delimiter_size, delimiter_size, delimiter) != 0 &&
                delimiter_size == 3) {
                multiline_quote = character;
                multiline_quote_size = delimiter_size;
            }
            index = end;
            continue;
        }

        const auto byte = static_cast<unsigned char>(character);
        if (std::isdigit(byte)) {
            auto end = index + 1;
            while (end < text.size()) {
                const auto next = static_cast<unsigned char>(text[end]);
                if (!std::isalnum(next) && text[end] != '.' && text[end] != '_') {
                    break;
                }
                ++end;
            }
            append(result, text.substr(index, end - index), SyntaxStyle::Number);
            index = end;
            continue;
        }
        if (is_identifier_start(byte)) {
            auto end = index + 1;
            while (end < text.size() &&
                   is_identifier_character(static_cast<unsigned char>(text[end]))) {
                ++end;
            }
            const auto identifier = text.substr(index, end - index);
            append(result, identifier, identifier_style(identifier));
            index = end;
            continue;
        }

        append(result, text.substr(index, 1), SyntaxStyle::Plain);
        ++index;
    }
    return result;
}

bool is_source_extension(std::string_view extension) {
    static const std::unordered_set<std::string_view> extensions = {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm",
        ".py", ".pyw", ".js", ".mjs", ".cjs", ".jsx", ".ts", ".tsx", ".rs", ".go",
        ".java", ".kt", ".kts", ".swift", ".sh", ".bash", ".zsh", ".json", ".yaml",
        ".yml", ".toml", ".css", ".scss", ".html", ".htm", ".xml", ".sql",
    };
    return extensions.count(extension) != 0;
}

bool is_markdown_extension(std::string_view extension) {
    return extension == ".md" || extension == ".markdown" || extension == ".mdown" ||
           extension == ".mkd" || extension == ".mdx";
}

bool is_supported_fence_language(std::string_view language) {
    static const std::unordered_set<std::string_view> languages = {
        "c", "cpp", "c++", "csharp", "cs", "css", "go", "golang", "html", "java",
        "javascript", "js", "json", "jsx", "kotlin", "kt", "lua", "python", "py", "rust",
        "rs", "shell", "sh", "sql", "swift", "toml", "typescript", "ts", "tsx", "xml",
        "yaml", "yml", "bash", "zsh",
    };
    return languages.count(language) != 0;
}

bool markdown_fence(std::string_view line, char& marker, std::size_t& length,
                    std::string& language) {
    const auto first = line.find_first_not_of(' ');
    if (first == std::string_view::npos || first > 3 ||
        (line[first] != '`' && line[first] != '~')) {
        return false;
    }
    const char candidate = line[first];
    auto end = first;
    while (end < line.size() && line[end] == candidate) {
        ++end;
    }
    if (end - first < 3) {
        return false;
    }
    marker = candidate;
    length = end - first;
    language = lowercase(line.substr(end));
    const auto language_start = language.find_first_not_of(" \t");
    language = language_start == std::string::npos ? std::string{} : language.substr(language_start);
    const auto language_end = language.find_first_of(" \t");
    if (language_end != std::string::npos) {
        language.resize(language_end);
    }
    return true;
}

StyledLine highlight_markdown_line(std::string_view line) {
    StyledLine result;
    const auto first = line.find_first_not_of(' ');
    if (first != std::string_view::npos && first <= 3 && line[first] == '#') {
        auto marker_end = first;
        while (marker_end < line.size() && line[marker_end] == '#') {
            ++marker_end;
        }
        if (marker_end - first <= 6 &&
            (marker_end == line.size() || line[marker_end] == ' ')) {
            append(result, line, SyntaxStyle::Heading);
            return result;
        }
    }

    std::size_t index = 0;
    while (index < line.size()) {
        if (line[index] == '`') {
            auto delimiter_end = index;
            while (delimiter_end < line.size() && line[delimiter_end] == '`') {
                ++delimiter_end;
            }
            const auto delimiter = line.substr(index, delimiter_end - index);
            const auto closing = line.find(delimiter, delimiter_end);
            if (closing != std::string_view::npos) {
                append(result, line.substr(index, closing + delimiter.size() - index),
                       SyntaxStyle::Code);
                index = closing + delimiter.size();
                continue;
            }
        }
        if (index + 1 < line.size() &&
            ((line[index] == '*' && line[index + 1] == '*') ||
             (line[index] == '_' && line[index + 1] == '_'))) {
            const char delimiter[3] = {line[index], line[index], '\0'};
            const auto closing = line.find(delimiter, index + 2);
            if (closing != std::string_view::npos) {
                append(result, line.substr(index, closing + 2 - index), SyntaxStyle::Emphasis);
                index = closing + 2;
                continue;
            }
        }
        if (line[index] == '[') {
            const auto label_end = line.find("](", index + 1);
            const auto link_end = label_end == std::string_view::npos
                                      ? std::string_view::npos
                                      : line.find(')', label_end + 2);
            if (link_end != std::string_view::npos) {
                append(result, line.substr(index, link_end + 1 - index), SyntaxStyle::Link);
                index = link_end + 1;
                continue;
            }
        }
        const auto next = line.find_first_of("`*_ [", index + 1);
        if (next == std::string_view::npos) {
            append(result, line.substr(index), SyntaxStyle::Plain);
            break;
        }
        append(result, line.substr(index, next - index), SyntaxStyle::Plain);
        index = next;
    }
    return result;
}

}  // namespace

std::vector<StyledLine> highlight_file(const std::string& path,
                                       const std::string& content) {
    const auto filename_start = path.find_last_of('/');
    const auto filename = lowercase(path.substr(filename_start == std::string::npos
                                                    ? 0
                                                    : filename_start + 1));
    const auto extension_start = filename.find_last_of('.');
    const auto extension = extension_start == std::string::npos
                               ? std::string{}
                               : filename.substr(extension_start);
    const bool markdown = is_markdown_extension(extension);
    const bool source = is_source_extension(extension);

    std::vector<StyledLine> lines;
    std::istringstream input(content);
    std::string text;
    bool in_block_comment = false;
    char multiline_quote = '\0';
    std::size_t multiline_quote_size = 0;
    bool in_fence = false;
    char fence_marker = '\0';
    std::size_t fence_length = 0;
    bool fence_has_highlight = false;
    while (std::getline(input, text)) {
        if (markdown) {
            char marker = '\0';
            std::size_t length = 0;
            std::string language;
            if (!in_fence && markdown_fence(text, marker, length, language)) {
                in_fence = true;
                fence_marker = marker;
                fence_length = length;
                fence_has_highlight = is_supported_fence_language(language);
                lines.push_back({{text, SyntaxStyle::Comment}});
                continue;
            }
            if (in_fence) {
                std::size_t closing_length = 0;
                const auto first = text.find_first_not_of(' ');
                if (first != std::string::npos && first <= 3 && text[first] == fence_marker) {
                    while (first + closing_length < text.size() &&
                           text[first + closing_length] == fence_marker) {
                        ++closing_length;
                    }
                }
                if (closing_length >= fence_length) {
                    in_fence = false;
                    in_block_comment = false;
                    multiline_quote = '\0';
                    multiline_quote_size = 0;
                    lines.push_back({{text, SyntaxStyle::Comment}});
                } else if (fence_has_highlight) {
                    lines.push_back(highlight_source_line(text, in_block_comment,
                                                          multiline_quote, multiline_quote_size));
                } else {
                    lines.push_back({{text, SyntaxStyle::Code}});
                }
                continue;
            }
            lines.push_back(highlight_markdown_line(text));
        } else if (source) {
            lines.push_back(highlight_source_line(text, in_block_comment,
                                                  multiline_quote, multiline_quote_size));
        } else {
            lines.push_back({{text, SyntaxStyle::Plain}});
        }
    }
    return lines;
}

}  // namespace github