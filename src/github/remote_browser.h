#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace github {

struct RepositoryContent {
    std::string name;
    std::string path;
    bool is_directory = false;
    std::size_t size = 0;
};

std::vector<RepositoryContent> parse_repository_contents(const std::string& output);
std::string encode_repository_path(const std::string& path);
std::string parent_repository_path(const std::string& path);

}  // namespace github