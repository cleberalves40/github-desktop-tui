#pragma once

#include <string>
#include <vector>

struct ProcessResult {
    int exit_code = -1;
    std::string output;
};

class ProcessRunner {
public:
    static ProcessResult run(const std::vector<std::string>& arguments,
                             const std::string& working_directory = {});
    static ProcessResult run_interactive(const std::vector<std::string>& arguments,
                                         const std::string& working_directory = {});
};