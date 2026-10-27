#include "git/repository.h"
#include "github/command_parser.h"
#include "process/process_runner.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() /
                       "github-desktop-tui-test.XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto created = mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("Could not create temporary directory.");
        }
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::string& path() const {
        return path_;
    }

private:
    std::string path_;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ProcessResult git(const std::string& repository_path,
                  const std::vector<std::string>& arguments) {
    auto command = std::vector<std::string>{"git", "-C", repository_path};
    command.insert(command.end(), arguments.begin(), arguments.end());
    return ProcessRunner::run(command);
}

void require_git(const std::string& repository_path,
                 const std::vector<std::string>& arguments) {
    const auto result = git(repository_path, arguments);
    require(result.exit_code == 0,
            "git command failed: " + result.output);
}

void write_file(const std::filesystem::path& path, const std::string& contents,
                bool append = false) {
    std::ofstream file(path, append ? std::ios::app : std::ios::trunc);
    require(file.good(), "Could not open test file: " + path.string());
    file << contents;
    require(file.good(), "Could not write test file: " + path.string());
}

const FileChange& find_change(const RepositorySnapshot& snapshot,
                              const std::string& path) {
    for (const auto& change : snapshot.changes) {
        if (change.path == path) {
            return change;
        }
    }
    throw std::runtime_error("Missing changed path: " + path);
}

void run_tests() {
    TemporaryDirectory temporary;
    const auto repository_path = temporary.path();

    const auto invalid = Repository::inspect(repository_path);
    require(!invalid.is_repository, "A non-repository directory was accepted.");

    require_git(repository_path, {"init", "-q", "-b", "main"});
    require_git(repository_path, {"config", "user.name", "TUI Test"});
    require_git(repository_path, {"config", "user.email", "tui-test@example.invalid"});

    write_file(std::filesystem::path(repository_path) / "tracked file.txt", "base\n");
    require_git(repository_path, {"add", "--", "tracked file.txt"});
    require_git(repository_path, {"commit", "-qm", "initial"});

    write_file(std::filesystem::path(repository_path) / "tracked file.txt", "changed\n", true);
    write_file(std::filesystem::path(repository_path) / "new file.txt", "untracked content\n");

    auto snapshot = Repository::inspect(repository_path);
    require(snapshot.is_repository, "The initialized repository was not detected.");
    require(snapshot.changes.size() == 2, "Expected two changed paths.");
    const auto& tracked = find_change(snapshot, "tracked file.txt");
    const auto& untracked = find_change(snapshot, "new file.txt");
    require(tracked.is_unstaged(), "Tracked modification was not marked unstaged.");
    require(untracked.is_untracked(), "New path with spaces was not marked untracked.");
    require(Repository::diff(repository_path, tracked).find("+changed") != std::string::npos,
            "Tracked diff did not include the changed line.");
    require(Repository::diff(repository_path, untracked).find("untracked content") !=
                std::string::npos,
            "Untracked diff did not include file contents.");

    require(Repository::toggle_stage(repository_path, tracked).exit_code == 0,
            "Could not stage tracked change.");
    require(Repository::toggle_stage(repository_path, untracked).exit_code == 0,
            "Could not stage untracked file.");
    snapshot = Repository::inspect(repository_path);
    require(find_change(snapshot, "tracked file.txt").is_staged(),
            "Tracked change was not staged.");
    require(find_change(snapshot, "new file.txt").is_staged(),
            "Untracked file was not staged.");

    require(Repository::toggle_stage(repository_path,
                                     find_change(snapshot, "new file.txt")).exit_code == 0,
            "Could not unstage new file.");
    snapshot = Repository::inspect(repository_path);
    require(find_change(snapshot, "new file.txt").is_untracked(),
            "Unstaging a new file did not restore its untracked state.");
    require(Repository::toggle_stage(repository_path,
                                     find_change(snapshot, "new file.txt")).exit_code == 0,
            "Could not stage new file again.");
    require(Repository::commit(repository_path, "phase 6 test commit").exit_code == 0,
            "Could not create test commit.");

    auto history = Repository::history(repository_path);
    require(!history.empty(), "Commit history is empty.");
    require(history.front().subject == "phase 6 test commit",
            "History returned an unexpected top commit.");
    const auto detail = Repository::commit_detail(repository_path, history.front().id);
    require(detail.find("phase 6 test commit") != std::string::npos,
            "Commit detail omitted the subject.");

    std::error_code rename_error;
    std::filesystem::rename(std::filesystem::path(repository_path) / "tracked file.txt",
                            std::filesystem::path(repository_path) / "renamed file.txt",
                            rename_error);
    require(!rename_error, "Could not rename tracked test file.");
    require_git(repository_path, {"add", "-A"});
    snapshot = Repository::inspect(repository_path);
    require(find_change(snapshot, "renamed file.txt").is_staged(),
            "Rename destination with spaces was not parsed.");
    require(Repository::commit(repository_path, "rename test file").exit_code == 0,
            "Could not commit test rename.");

        write_file(std::filesystem::path(repository_path) / "large-output.bin",
                           std::string(4 * 1024 * 1024 + 128, 'x'));
        require_git(repository_path, {"add", "--", "large-output.bin"});
        require_git(repository_path, {"commit", "-qm", "large output fixture"});
        const auto large_output = ProcessRunner::run(
                {"git", "-C", repository_path, "show", "HEAD:large-output.bin"});
        require(large_output.output.find("[output truncated after 4 MiB]") != std::string::npos,
                        "Process output was not bounded at 4 MiB.");

    const auto parsed = github::parse_command("gh pr list --search \"needs review\"");
    require(parsed.error.empty(), "Valid quoted gh command was rejected.");
    require(parsed.arguments == std::vector<std::string>{
                                   "gh", "pr", "list", "--search", "needs review"},
            "Quoted gh arguments were not preserved.");
    const auto escaped = github::parse_command("issue list --search needs\\ review");
    require(escaped.arguments.size() == 5 && escaped.arguments.back() == "needs review",
            "Escaped whitespace was not parsed as part of one argument.");
    const auto shell_literal = github::parse_command(
        "--version;touch /tmp/should-not-execute");
    require(shell_literal.arguments.size() == 3 &&
                shell_literal.arguments[1] == "--version;touch",
            "Shell metacharacters were not preserved as literal arguments.");
    require(!github::parse_command("pr list \"unfinished").error.empty(),
            "Unclosed quote was not reported.");
    const auto forced = github::parse_command("!gh auth refresh");
    require(forced.error.empty() && forced.force_interactive,
            "Explicit interactive prefix was not recognized.");
    require(github::needs_interactive_terminal({"gh", "auth", "login"}),
            "gh auth login was not classified as interactive.");
    require(!github::needs_interactive_terminal({"gh", "pr", "list"}),
            "Non-interactive PR listing was classified as interactive.");

    const auto base_branch = Repository::inspect(repository_path).branch;
    require(Repository::create_branch(repository_path, "invalid branch").exit_code != 0,
            "Invalid branch name was accepted.");
    require(Repository::create_branch(repository_path, "feature/phase6").exit_code == 0,
            "Could not create feature branch.");
    write_file(std::filesystem::path(repository_path) / "renamed file.txt",
               "feature line\n", true);
    require_git(repository_path, {"add", "--", "renamed file.txt"});
    require_git(repository_path, {"commit", "-qm", "feature change"});
    require(Repository::switch_branch(repository_path, base_branch).exit_code == 0,
            "Could not switch back to base branch.");
    const auto comparison = Repository::compare_branch(repository_path, base_branch,
                                                       "feature/phase6");
    require(comparison.find("feature line") != std::string::npos,
            "Branch comparison omitted the feature change.");
    require(Repository::delete_branch(repository_path, "feature/phase6").exit_code != 0,
            "Unmerged branch was deleted by the safe delete operation.");

    require(Repository::create_branch(repository_path, "merged-branch").exit_code == 0,
            "Could not create merged branch for delete test.");
    require(Repository::switch_branch(repository_path, base_branch).exit_code == 0,
            "Could not return to base branch.");
    require(Repository::delete_branch(repository_path, "merged-branch").exit_code == 0,
            "Could not delete a merged branch.");

    require(Repository::create_branch(repository_path, "conflict-side").exit_code == 0,
            "Could not create conflict side branch.");
    write_file(std::filesystem::path(repository_path) / "conflict.txt", "side branch\n");
    require_git(repository_path, {"add", "--", "conflict.txt"});
    require_git(repository_path, {"commit", "-qm", "side conflict commit"});
    require(Repository::switch_branch(repository_path, base_branch).exit_code == 0,
            "Could not switch to base for conflict test.");
    write_file(std::filesystem::path(repository_path) / "conflict.txt", "base branch\n");
    require_git(repository_path, {"add", "--", "conflict.txt"});
    require_git(repository_path, {"commit", "-qm", "base conflict commit"});
    require(git(repository_path, {"merge", "conflict-side"}).exit_code != 0,
            "Expected merge conflict was not produced.");
    snapshot = Repository::inspect(repository_path);
    require(snapshot.has_conflicts, "Merge conflict was not detected in repository status.");
    require(find_change(snapshot, "conflict.txt").is_conflicted(),
            "Conflicted path was not classified as unmerged.");
}

}  // namespace

int main() {
    try {
        run_tests();
        std::cout << "Repository integration tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Repository integration tests failed: " << error.what() << '\n';
        return 1;
    }
}