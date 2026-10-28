#pragma once

#include <string>
#include <vector>

namespace github {

enum class SyntaxStyle {
    Plain,
    Keyword,
    Type,
    String,
    Comment,
    Number,
    Heading,
    Code,
    Emphasis,
    Link,
};

struct SyntaxSpan {
    std::string text;
    SyntaxStyle style = SyntaxStyle::Plain;
};

using StyledLine = std::vector<SyntaxSpan>;

std::vector<StyledLine> highlight_file(const std::string& path,
                                       const std::string& content);

}  // namespace github