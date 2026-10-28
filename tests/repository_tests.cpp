#include "git/repository.h"
#include "github/command_parser.h"
#include "github/remote_browser.h"
#include "github/syntax_highlighter.h"
#include "process/process_runner.h"
#include "ui/theme.h"

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

void test_remote_sync() {
    TemporaryDirectory temporary;
    const auto root = std::filesystem::path(temporary.path());
    const auto remote = root / "remote.git";
    const auto seed = root / "seed";
    const auto peer = root / "peer";
        std::filesystem::create_directories(remote);
    std::filesystem::create_directories(seed);
    require_git(remote.string(), {"init", "--bare", "-q", "--initial-branch=main"});
    require_git(seed.string(), {"init", "-q", "-b", "main"});
    require_git(seed.string(), {"config", "user.name", "TUI Test"});
    require_git(seed.string(), {"config", "user.email", "tui-test@example.invalid"});
    write_file(seed / "README.md", "initial\n");
    require_git(seed.string(), {"add", "README.md"});
    require_git(seed.string(), {"commit", "-qm", "initial"});
    require_git(seed.string(), {"remote", "add", "origin", remote.string()});
    require_git(seed.string(), {"push", "-u", "origin", "main"});
    require(ProcessRunner::run({"git", "clone", remote.string(), peer.string()}).exit_code == 0,
            "Could not clone local test remote.");
    require_git(peer.string(), {"config", "user.name", "TUI Test"});
    require_git(peer.string(), {"config", "user.email", "tui-test@example.invalid"});

    write_file(seed / "README.md", "remote update\n", true);
    require_git(seed.string(), {"add", "README.md"});
    require_git(seed.string(), {"commit", "-qm", "remote update"});
    require(Repository::push(seed.string()).exit_code == 0,
            "Repository push did not publish to the configured upstream.");

    const auto local_head = git(peer.string(), {"rev-parse", "HEAD"}).output;
    require(Repository::fetch(peer.string()).exit_code == 0,
            "Repository fetch did not update remote-tracking refs.");
    const auto fetched_head = git(peer.string(), {"rev-parse", "origin/main"}).output;
    require(local_head != fetched_head, "Fetch unexpectedly changed the local branch.");
    require(Repository::pull(peer.string()).exit_code == 0,
            "Fast-forward pull failed for a clean working tree.");
    require(git(peer.string(), {"rev-parse", "HEAD"}).output == fetched_head,
            "Pull did not fast-forward the local branch.");

    write_file(peer / "README.md", "local edit\n", true);
    write_file(seed / "next.txt", "next remote commit\n");
    require_git(seed.string(), {"add", "next.txt"});
    require_git(seed.string(), {"commit", "-qm", "next remote commit"});
    require(Repository::push(seed.string()).exit_code == 0,
            "Could not publish second remote test commit.");
    const auto blocked_pull = Repository::pull(peer.string());
    require(blocked_pull.exit_code != 0 &&
                blocked_pull.output.find("Commit or stash") != std::string::npos,
            "Pull did not protect local uncommitted changes.");
    require(git(peer.string(), {"diff", "--", "README.md"}).output.find("local edit") !=
                std::string::npos,
            "Blocked pull changed the local edit.");
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
    require(app::available_themes().size() == 4 &&
                app::parse_theme_name("nOrD") == app::ThemeName::Nord &&
                !app::parse_theme_name("unknown"),
            "Theme names were not parsed correctly.");
    {
        TemporaryDirectory config_directory;
        const auto preference = std::filesystem::path(config_directory.path()) /
                                "config" / "theme";
        require(!app::load_theme_preference(preference),
                "A missing theme preference should use the default.");
        require(app::save_theme_preference(preference, app::ThemeName::Catppuccin),
                "Could not save the selected theme.");
        require(app::load_theme_preference(preference) == app::ThemeName::Catppuccin,
                "The selected theme was not persisted.");
    }

        const auto source_lines = github::highlight_file(
                "src/example.cpp", "int answer = 42; // comment\nconst char* text = \"hello\";\n");
        require(source_lines.size() == 2, "Source preview lines were not preserved.");
        bool has_source_type = false;
        bool has_source_number = false;
        for (const auto& span : source_lines[0]) {
                has_source_type = has_source_type || span.style == github::SyntaxStyle::Type;
                has_source_number = has_source_number || span.style == github::SyntaxStyle::Number;
        }
        require(has_source_type && has_source_number,
                        "Source keywords and numbers were not highlighted.");
        require(source_lines[0].back().style == github::SyntaxStyle::Comment,
                        "Source comments were not highlighted.");
        bool has_source_string = false;
        for (const auto& span : source_lines[1]) {
                has_source_string = has_source_string || span.style == github::SyntaxStyle::String;
        }
        require(has_source_string,
                        "Source strings were not highlighted.");

        const auto markdown_lines = github::highlight_file(
                "README.md", "# Heading\nInline `code` and **strong**\n```cpp\nreturn 42;\n```\n");
        require(markdown_lines.size() == 5 &&
                                markdown_lines[0][0].style == github::SyntaxStyle::Heading,
                        "Markdown heading was not highlighted.");
        require(markdown_lines[1][1].style == github::SyntaxStyle::Code &&
                                markdown_lines[1].back().style == github::SyntaxStyle::Emphasis,
                        "Markdown inline code and emphasis were not highlighted.");
        require(markdown_lines[3][0].style == github::SyntaxStyle::Keyword,
                        "Markdown fenced source code was not highlighted.");

        const auto plain_lines = github::highlight_file("notes.txt", "plain text\n");
        require(plain_lines.size() == 1 && plain_lines[0].size() == 1 &&
                                plain_lines[0][0].style == github::SyntaxStyle::Plain,
                        "Unknown file types should remain plain text.");

    const auto contents = github::parse_repository_contents(
        "docs%20and%20notes\tdir\tdocs%20and%20notes\t0\n"
        "hello%20world.txt\tfile\tdocs%20and%20notes/hello%20world.txt\t42\n");
    require(contents.size() == 2, "Remote repository contents were not parsed.");
    require(contents[0].is_directory && contents[0].path == "docs and notes",
            "Remote directory entry was parsed incorrectly.");
    require(!contents[1].is_directory && contents[1].name == "hello world.txt" &&
                contents[1].size == 42,
            "Remote file entry with spaces was parsed incorrectly.");
    require(github::encode_repository_path("docs and notes/a#b.txt") ==
                "docs%20and%20notes/a%23b.txt",
            "Remote path was not URL-encoded correctly.");
    require(github::parent_repository_path("docs/subdir") == "docs" &&
                github::parent_repository_path("docs") == "",
            "Remote parent path was not calculated correctly.");

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
        const auto conflict_pull = Repository::pull(repository_path);
        require(conflict_pull.exit_code != 0 &&
                                conflict_pull.output.find("Resolve merge conflicts") != std::string::npos,
                        "Pull did not protect a repository with unresolved conflicts.");

        test_remote_sync();
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