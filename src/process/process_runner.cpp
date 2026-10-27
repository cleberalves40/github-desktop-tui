#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "process/process_runner.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {

int add_working_directory(posix_spawn_file_actions_t* actions,
                          const std::string& working_directory) {
#if defined(__APPLE__) || defined(__linux__)
    return posix_spawn_file_actions_addchdir_np(actions, working_directory.c_str());
#else
    return posix_spawn_file_actions_addchdir(actions, working_directory.c_str());
#endif
}

}  // namespace

ProcessResult ProcessRunner::run(const std::vector<std::string>& arguments,
                                 const std::string& working_directory) {
    if (arguments.empty()) {
        throw std::invalid_argument("Cannot run a process without arguments.");
    }

    int output_pipe[2];
    if (pipe(output_pipe) == -1) {
        throw std::runtime_error("Failed to create process output pipe.");
    }

    posix_spawn_file_actions_t actions;
    int spawn_error = posix_spawn_file_actions_init(&actions);
    if (spawn_error != 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        throw std::runtime_error(std::string("Failed to prepare process: ") +
                                 std::strerror(spawn_error));
    }

    if (!working_directory.empty()) {
        spawn_error = add_working_directory(&actions, working_directory);
    }
    if (spawn_error == 0) {
        spawn_error = posix_spawn_file_actions_adddup2(
            &actions, output_pipe[1], STDOUT_FILENO);
    }
    if (spawn_error == 0) {
        spawn_error = posix_spawn_file_actions_adddup2(
            &actions, output_pipe[1], STDERR_FILENO);
    }
    if (spawn_error == 0) {
        spawn_error = posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
    }
    if (spawn_error == 0) {
        spawn_error = posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    pid_t child = -1;
    if (spawn_error == 0) {
        spawn_error = posix_spawnp(&child, argv[0], &actions, nullptr,
                                   argv.data(), environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_error != 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        throw std::runtime_error(std::string("Failed to start process: ") +
                                 std::strerror(spawn_error));
    }

    close(output_pipe[1]);
    ProcessResult result;
    constexpr std::size_t max_output_size = 4 * 1024 * 1024;
    bool output_truncated = false;
    char buffer[4096];
    while (true) {
        const ssize_t count = read(output_pipe[0], buffer, sizeof(buffer));
        if (count > 0) {
            const auto count_size = static_cast<std::size_t>(count);
            const auto remaining = max_output_size - result.output.size();
            const auto append_size = std::min(count_size, remaining);
            result.output.append(buffer, append_size);
            output_truncated = output_truncated || append_size < count_size;
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(output_pipe[0]);
    if (output_truncated) {
        result.output += "\n[output truncated after 4 MiB]\n";
    }

    int status = 0;
    while (waitpid(child, &status, 0) == -1) {
        if (errno != EINTR) {
            throw std::runtime_error("Failed while waiting for process.");
        }
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

ProcessResult ProcessRunner::run_interactive(const std::vector<std::string>& arguments,
                                             const std::string& working_directory) {
    if (arguments.empty()) {
        throw std::invalid_argument("Cannot run a process without arguments.");
    }

    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    const int init_error = posix_spawn_file_actions_init(&actions);
    if (init_error != 0) {
        throw std::runtime_error(std::string("Failed to prepare interactive process: ") +
                                 std::strerror(init_error));
    }

    int spawn_error = 0;
    if (!working_directory.empty()) {
        spawn_error = add_working_directory(&actions, working_directory);
    }

    pid_t child = -1;
    if (spawn_error == 0) {
        spawn_error = posix_spawnp(&child, argv[0], &actions, nullptr,
                                   argv.data(), environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_error != 0) {
        throw std::runtime_error(std::string("Failed to start interactive process: ") +
                                 std::strerror(spawn_error));
    }

    int status = 0;
    while (waitpid(child, &status, 0) == -1) {
        if (errno != EINTR) {
            throw std::runtime_error("Failed while waiting for interactive process.");
        }
    }

    ProcessResult result;
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}