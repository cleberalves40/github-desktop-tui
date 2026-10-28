#include "github/remote_browser.h"

#include <charconv>
#include <sstream>
#include <string_view>

namespace github {
namespace {

int hex_value(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

std::string decode_uri_component(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const int high = hex_value(value[index + 1]);
            const int low = hex_value(value[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        decoded.push_back(value[index]);
    }
    return decoded;
}

}  // namespace

std::vector<RepositoryContent> parse_repository_contents(const std::string& output) {
    std::vector<RepositoryContent> contents;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        std::string_view fields[4];
        std::size_t start = 0;
        bool valid = true;
        for (std::size_t index = 0; index < 3; ++index) {
            const auto separator = line.find('\t', start);
            if (separator == std::string::npos) {
                valid = false;
                break;
            }
            fields[index] = std::string_view(line).substr(start, separator - start);
            start = separator + 1;
        }
        if (!valid || line.find('\t', start) != std::string::npos) {
            continue;
        }
        fields[3] = std::string_view(line).substr(start);
        if (fields[1] != "file" && fields[1] != "dir") {
            continue;
        }

        RepositoryContent content;
        content.name = decode_uri_component(fields[0]);
        content.path = decode_uri_component(fields[2]);
        content.is_directory = fields[1] == "dir";
        const auto size_result = std::from_chars(
            fields[3].data(), fields[3].data() + fields[3].size(), content.size);
        if (size_result.ec != std::errc{} || size_result.ptr != fields[3].data() + fields[3].size()) {
            content.size = 0;
        }
        contents.push_back(std::move(content));
    }
    return contents;
}

std::string encode_repository_path(const std::string& path) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(path.size());
    for (const unsigned char character : path) {
        const bool unreserved = (character >= 'a' && character <= 'z') ||
                                (character >= 'A' && character <= 'Z') ||
                                (character >= '0' && character <= '9') ||
                                character == '-' || character == '_' ||
                                character == '.' || character == '~';
        if (unreserved || character == '/') {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[character >> 4]);
            encoded.push_back(hex[character & 0x0F]);
        }
    }
    return encoded;
}

std::string parent_repository_path(const std::string& path) {
    const auto separator = path.find_last_of('/');
    return separator == std::string::npos ? std::string{} : path.substr(0, separator);
}

}  // namespace github