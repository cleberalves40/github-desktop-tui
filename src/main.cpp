#include "git/repository.h"
#include "github/command_parser.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

enum class View { Home, Changes, History, Branches };
enum class InputMode { None, CommitMessage, NewBranch, ConfirmDelete, GhCommand };

struct RemoteRepository {
    std::string name;
    std::string visibility;
    std::string url;
};

std::string status_code(const FileChange& change) {
    if (change.is_untracked()) {
        return "??";
    }
    return std::string(1, change.index_status) + change.worktree_status;
}

ftxui::Color status_color(const FileChange& change) {
    if (change.is_untracked()) {
        return ftxui::Color::Yellow;
    }
    if (change.is_conflicted() || change.index_status == 'D' || change.worktree_status == 'D') {
        return ftxui::Color::Red;
    }
    if (change.is_staged() && !change.is_unstaged()) {
        return ftxui::Color::Green;
    }
    return ftxui::Color::Cyan;
}

ftxui::Elements render_diff(const std::string& diff) {
    ftxui::Elements lines;
    std::istringstream input(diff);
    std::string line;
    while (std::getline(input, line)) {
        auto element = ftxui::text(line);
        if (!line.empty() && line[0] == '+') {
            element = element | ftxui::color(ftxui::Color::Green);
        } else if (!line.empty() && line[0] == '-') {
            element = element | ftxui::color(ftxui::Color::Red);
        } else if (line.rfind("diff --", 0) == 0 ||
                   line.rfind("@@", 0) == 0 ||
                   line.rfind("Staged changes", 0) == 0 ||
                   line.rfind("Unstaged changes", 0) == 0) {
            element = element | ftxui::color(ftxui::Color::Cyan);
        }
        lines.push_back(std::move(element));
    }
    if (lines.empty()) {
        lines.push_back(ftxui::text("Select a changed file to inspect its diff."));
    }
    return lines;
}

void erase_last_character(std::string& text) {
    if (text.empty()) {
        return;
    }
    text.pop_back();
    while (!text.empty() &&
           (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
        text.pop_back();
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc > 2) {
        return 2;
    }

    const std::string path = argc == 2 ? argv[1] : "";
    auto screen = ftxui::ScreenInteractive::Fullscreen();
    std::mutex state_mutex;
    std::string repository_path = path;
    RepositorySnapshot snapshot;
    std::vector<RemoteRepository> remote_repositories;
    std::vector<BranchInfo> branch_list;
    std::vector<CommitInfo> commit_list;
    std::string diff_text;
    std::string detail_text;
    std::string detail_title;
    std::string commit_message;
    std::string new_branch_name;
    std::string pending_delete_branch;
    std::string gh_input;
    std::string gh_output;
    std::string gh_status = "GitHub: checking authentication...";
    std::string remote_error;
    std::vector<std::string> gh_history;
    std::size_t gh_history_index = 0;
    std::size_t remote_repository_index = 0;
    std::string status_message = "Loading repository...";
    std::size_t selected_change_index = 0;
    std::size_t selected_branch_index = 0;
    std::size_t selected_commit_index = 0;
    View current_view = path.empty() ? View::Home : View::Changes;
    InputMode input_mode = InputMode::None;
    bool busy = false;
    bool gh_authenticated = false;
    std::thread worker;

    auto run_background = [&](std::function<void()> operation) {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (busy) {
                return;
            }
            busy = true;
            status_message = "Working...";
        }
        if (worker.joinable()) {
            worker.join();
        }
        worker = std::thread([&, operation = std::move(operation)] {
            try {
                operation();
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(state_mutex);
                status_message = error.what();
            }
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                busy = false;
            }
            screen.PostEvent(ftxui::Event::Custom);
        });
    };

    auto refresh_data = [&](const std::string& notice) {
        std::string active_path;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            active_path = repository_path;
        }
        auto fresh = Repository::inspect(active_path);
        std::string previous_path;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (selected_change_index < snapshot.changes.size()) {
                previous_path = snapshot.changes[selected_change_index].path;
            }
        }

        std::size_t next_index = 0;
        if (!previous_path.empty()) {
            const auto previous = std::find_if(
                fresh.changes.begin(), fresh.changes.end(),
                [&](const FileChange& change) { return change.path == previous_path; });
            if (previous != fresh.changes.end()) {
                next_index = static_cast<std::size_t>(previous - fresh.changes.begin());
            }
        }

        std::string next_diff;
        if (fresh.is_repository && !fresh.changes.empty()) {
            next_diff = Repository::diff(fresh.path, fresh.changes[next_index]);
        }

        std::lock_guard<std::mutex> lock(state_mutex);
        snapshot = std::move(fresh);
        selected_change_index = next_index;
        diff_text = std::move(next_diff);
        status_message = snapshot.is_repository ? notice : snapshot.error;
    };

    auto refresh_navigation = [&](const std::string& notice) {
        std::string repository_path;
        std::string current_branch;
        std::string previous_branch;
        std::string previous_commit;
        View active_view;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            repository_path = snapshot.path;
            current_branch = snapshot.branch;
            active_view = current_view;
            if (selected_branch_index < branch_list.size()) {
                previous_branch = branch_list[selected_branch_index].name;
            }
            if (selected_commit_index < commit_list.size()) {
                previous_commit = commit_list[selected_commit_index].id;
            }
        }
        if (repository_path.empty()) {
            return;
        }

        auto next_branches = Repository::branches(repository_path);
        auto next_commits = Repository::history(repository_path);
        std::size_t next_branch_index = 0;
        const auto branch = std::find_if(
            next_branches.begin(), next_branches.end(), [&](const BranchInfo& item) {
                return item.name == previous_branch;
            });
        if (branch != next_branches.end()) {
            next_branch_index = static_cast<std::size_t>(branch - next_branches.begin());
        } else {
            const auto current = std::find_if(
                next_branches.begin(), next_branches.end(),
                [](const BranchInfo& item) { return item.is_current; });
            if (current != next_branches.end()) {
                next_branch_index = static_cast<std::size_t>(current - next_branches.begin());
            }
        }

        std::size_t next_commit_index = 0;
        const auto commit = std::find_if(
            next_commits.begin(), next_commits.end(), [&](const CommitInfo& item) {
                return item.id == previous_commit;
            });
        if (commit != next_commits.end()) {
            next_commit_index = static_cast<std::size_t>(commit - next_commits.begin());
        }

        std::string next_detail;
        std::string next_title;
        if (active_view == View::History && !next_commits.empty()) {
            const auto& selected = next_commits[next_commit_index];
            next_title = selected.short_id + "  " + selected.subject;
            next_detail = Repository::commit_detail(repository_path, selected.id);
        } else if (active_view == View::Branches && !next_branches.empty()) {
            const auto& selected = next_branches[next_branch_index];
            next_title = "Branch: " + selected.name;
            next_detail = selected.is_current
                              ? "This is the current branch.\n\nSelect another branch and press Enter to switch."
                              : "Press Enter to switch to this branch.\nPress v to compare it with " +
                                    (current_branch.empty() ? "HEAD" : current_branch) + ".";
        }

        std::lock_guard<std::mutex> lock(state_mutex);
        branch_list = std::move(next_branches);
        commit_list = std::move(next_commits);
        selected_branch_index = next_branch_index;
        selected_commit_index = next_commit_index;
        if (active_view != View::Changes) {
            detail_text = std::move(next_detail);
            detail_title = std::move(next_title);
        }
        status_message = notice;
    };

    auto refresh_gh_auth = [&](const std::string& repository_path) {
        bool authenticated = false;
        std::vector<RemoteRepository> repositories;
        std::string error;
        try {
            authenticated = ProcessRunner::run(
                                {"gh", "auth", "status"}, repository_path).exit_code == 0;
        } catch (const std::exception&) {
            authenticated = false;
        }
        if (authenticated) {
            try {
                const auto result = ProcessRunner::run(
                    {"gh", "repo", "list", "--limit", "100", "--json",
                     "nameWithOwner,isPrivate,url", "--template",
                     "{{range .}}{{.nameWithOwner}}\t{{if .isPrivate}}private{{else}}public{{end}}\t{{.url}}\n{{end}}"},
                    repository_path);
                if (result.exit_code == 0) {
                    std::istringstream rows(result.output);
                    std::string row;
                    while (std::getline(rows, row)) {
                        const auto first_tab = row.find('\t');
                        const auto second_tab = first_tab == std::string::npos
                                                    ? std::string::npos
                                                    : row.find('\t', first_tab + 1);
                        if (first_tab == std::string::npos || second_tab == std::string::npos) {
                            continue;
                        }
                        repositories.push_back({row.substr(0, first_tab),
                                                 row.substr(first_tab + 1,
                                                            second_tab - first_tab - 1),
                                                 row.substr(second_tab + 1)});
                    }
                } else {
                    error = result.output;
                }
            } catch (const std::exception& exception) {
                error = exception.what();
            }
        }
        std::lock_guard<std::mutex> lock(state_mutex);
        gh_authenticated = authenticated;
        gh_status = authenticated ? "GitHub: authenticated" : "GitHub: not authenticated";
        remote_repositories = std::move(repositories);
        remote_repository_index = 0;
        remote_error = std::move(error);
    };

    auto run_gh_command = [&](std::vector<std::string> arguments,
                              bool force_interactive = false) {
        std::string repository_path;
        std::ostringstream command_label;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (index != 0) {
                command_label << ' ';
            }
            command_label << arguments[index];
        }
        const auto label = command_label.str();
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            repository_path = snapshot.path;
            detail_title = label;
            detail_text = "Running " + label + "...";
            gh_output = detail_text;
        }

        if (force_interactive || github::needs_interactive_terminal(arguments)) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                busy = true;
                status_message = "Terminal handed to " + label + ".";
            }
            ProcessResult result;
            try {
                screen.WithRestoredIO([&] {
                    result = ProcessRunner::run_interactive(arguments, repository_path);
                })();
            } catch (const std::exception& error) {
                result.exit_code = 127;
                result.output = error.what();
            }
            if (arguments.size() > 1 && arguments[1] == "auth") {
                refresh_gh_auth(repository_path);
            }
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                busy = false;
                detail_text = result.output.empty()
                                  ? "Interactive command exited with code " +
                                        std::to_string(result.exit_code) + "."
                                  : result.output;
                    gh_output = detail_text;
                status_message = label + " exited with code " +
                                 std::to_string(result.exit_code) + ".";
            }
            screen.PostEvent(ftxui::Event::Custom);
            return;
        }

        run_background([&, arguments = std::move(arguments), repository_path, label] {
            const auto result = ProcessRunner::run(arguments, repository_path);
            const auto output = result.output.empty()
                                    ? "Command completed with no output."
                                    : result.output;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                detail_title = label;
                detail_text = output;
                gh_output = output;
                status_message = label + " exited with code " +
                                 std::to_string(result.exit_code) + ".";
            }
            if (arguments.size() > 1 && arguments[1] == "auth") {
                refresh_gh_auth(repository_path);
            }
            if (arguments.size() > 2 && arguments[1] == "pr" &&
                arguments[2] == "checkout" && result.exit_code == 0) {
                refresh_data("Checked out pull request branch.");
                refresh_navigation("Checked out pull request branch.");
            }
        });
    };

    auto app = ftxui::Renderer([&] {
        RepositorySnapshot current;
        std::string current_diff;
        std::string current_detail;
        std::string current_detail_title;
        std::string current_message;
        std::string current_branch_input;
        std::string current_pending_delete;
        std::string current_gh_input;
        std::string current_gh_output;
        std::string current_gh_status;
        std::string current_notice;
        std::vector<BranchInfo> current_branches;
        std::vector<CommitInfo> current_commits;
        std::vector<RemoteRepository> current_remote_repositories;
        std::size_t change_selection = 0;
        std::size_t branch_selection = 0;
        std::size_t commit_selection = 0;
        std::size_t remote_selection = 0;
        std::string current_remote_error;
        View view = View::Changes;
        InputMode mode = InputMode::None;
        bool is_busy = false;
        bool is_gh_authenticated = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            current = snapshot;
            current_diff = diff_text;
            current_detail = detail_text;
            current_detail_title = detail_title;
            current_message = commit_message;
            current_branch_input = new_branch_name;
            current_pending_delete = pending_delete_branch;
            current_gh_input = gh_input;
            current_gh_output = gh_output;
            current_gh_status = gh_status;
            current_notice = status_message;
            current_branches = branch_list;
            current_commits = commit_list;
            current_remote_repositories = remote_repositories;
            change_selection = selected_change_index;
            branch_selection = selected_branch_index;
            commit_selection = selected_commit_index;
            remote_selection = remote_repository_index;
            current_remote_error = remote_error;
            view = current_view;
            mode = input_mode;
            is_busy = busy;
            is_gh_authenticated = gh_authenticated;
        }

        if (view == View::Home) {
            ftxui::Elements repository_rows;
            for (std::size_t index = 0; index < current_remote_repositories.size(); ++index) {
                const auto& repository = current_remote_repositories[index];
                const auto icon = repository.visibility == "private" ? "\uf023 " : "\uf0ac ";
                auto row = ftxui::text(icon + repository.name);
                if (index == remote_selection) {
                    row = row | ftxui::bgcolor(ftxui::Color::GrayDark) |
                          ftxui::color(ftxui::Color::White);
                }
                repository_rows.push_back(std::move(row));
            }
            if (repository_rows.empty()) {
                repository_rows.push_back(ftxui::text(
                    is_gh_authenticated ? "No repositories found." : "Sign in to GitHub to list repositories."));
            }
            const auto selection = remote_selection < current_remote_repositories.size()
                                       ? current_remote_repositories[remote_selection].name
                                       : std::string("No repository selected");
            const auto repository_list = ftxui::vbox({
                ftxui::text("\uf07c  REPOSITORIES") | ftxui::bold,
                ftxui::separator(),
                ftxui::vbox(std::move(repository_rows)) | ftxui::yframe | ftxui::flex,
            }) | ftxui::border | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 36) | ftxui::flex;
            const auto repository_detail = ftxui::vbox({
                ftxui::text("\uf09b  GITHUB") | ftxui::bold,
                ftxui::separator(),
                ftxui::text(is_gh_authenticated ? "Select a repository to clone."
                                                : "Connect your GitHub account."),
                ftxui::text(selection),
                ftxui::text("\uf0c5  Clone location: ~/src/OWNER/REPOSITORY"),
                ftxui::text(current_remote_error),
            }) | ftxui::border | ftxui::flex;
            ftxui::Elements output_lines;
            std::istringstream output(current_gh_output);
            std::string output_line;
            while (std::getline(output, output_line)) {
                output_lines.push_back(ftxui::text(output_line));
            }
            if (output_lines.empty()) {
                output_lines.push_back(ftxui::text("GitHub CLI output will appear here."));
            }
            const auto cli_panel = ftxui::vbox({
                ftxui::hbox({
                    ftxui::text("\uf120  GITHUB CLI") | ftxui::bold,
                    ftxui::filler(),
                    ftxui::text(is_busy ? "Working..." : current_notice),
                }),
                ftxui::separator(),
                ftxui::vbox(std::move(output_lines)) | ftxui::yframe | ftxui::flex,
                ftxui::text(mode == InputMode::GhCommand
                                ? ":gh> " + current_gh_input + "_"
                                : ": command  a auth  A login"),
            }) | ftxui::border | ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, 7) |
                ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 10);
            return ftxui::vbox({
                ftxui::hbox({
                    ftxui::text("\uf09b  GitHub Desktop TUI") | ftxui::bold,
                    ftxui::filler(),
                    ftxui::text(current_gh_status) |
                        ftxui::color(is_gh_authenticated ? ftxui::Color::Green
                                                         : ftxui::Color::Yellow),
                }),
                ftxui::hbox({repository_list, repository_detail}) | ftxui::flex,
                cli_panel,
                ftxui::text("A Sign in  r Refresh  j/k Navigate  Enter Clone  : CLI  q Quit"),
            }) | ftxui::border;
        }

        if (!current.is_repository) {
            return ftxui::vbox({
                       ftxui::text("GitHub Desktop TUI") | ftxui::bold,
                       ftxui::separator(),
                       ftxui::text(current.error.empty() ? "Not a Git repository." : current.error),
                       ftxui::text("Usage: github-desktop-tui [repository-path]"),
                       ftxui::text("Press H to return home.  q Quit"),
                   }) |
                   ftxui::border;
        }

            ftxui::Elements rows;
            std::size_t selection = 0;
            std::string panel_title;
            if (view == View::Changes) {
                panel_title = "CHANGES";
                selection = change_selection;
                for (std::size_t index = 0; index < current.changes.size(); ++index) {
                    const auto& change = current.changes[index];
                    auto row = ftxui::text("\uf15b " + status_code(change) + "  " + change.path) |
                               ftxui::color(status_color(change));
                    if (index == selection) {
                        row = row | ftxui::bgcolor(ftxui::Color::GrayDark);
                    }
                    rows.push_back(std::move(row));
            }
                if (rows.empty()) {
                    rows.push_back(ftxui::text("Working tree clean."));
                }
            } else if (view == View::History) {
                panel_title = "HISTORY";
                selection = commit_selection;
                for (std::size_t index = 0; index < current_commits.size(); ++index) {
                    const auto& commit = current_commits[index];
                    auto row = ftxui::text("\uf1da " + commit.short_id + "  " +
                                           commit.date + "  " + commit.subject);
                    if (index == selection) {
                        row = row | ftxui::bgcolor(ftxui::Color::GrayDark);
                    }
                    rows.push_back(std::move(row));
                }
                if (rows.empty()) {
                    rows.push_back(ftxui::text("No commits yet."));
                }
            } else {
                panel_title = "BRANCHES";
                selection = branch_selection;
                for (std::size_t index = 0; index < current_branches.size(); ++index) {
                    const auto& branch = current_branches[index];
                    auto row = ftxui::text(std::string(branch.is_current ? "\ue0a0 " : "  ") +
                                           branch.name);
                    if (index == selection) {
                        row = row | ftxui::bgcolor(ftxui::Color::GrayDark);
                    }
                    rows.push_back(std::move(row));
                }
                if (rows.empty()) {
                    rows.push_back(ftxui::text("No local branches."));
                }
        }

            auto list_panel = ftxui::vbox({
                                     ftxui::text(panel_title) | ftxui::bold,
                                 ftxui::separator(),
                                     ftxui::vbox(std::move(rows)) | ftxui::yframe | ftxui::flex,
                             }) |
                             ftxui::border |
                             ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 26) |
                             ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 42) | ftxui::flex;

        std::string right_title;
        std::string right_text;
        if (view == View::Changes) {
              right_title = current.changes.empty()
                              ? "DIFF"
                              : "DIFF  " + current.changes[change_selection].path;
              right_text = current_diff;
        } else {
            right_title = current_detail_title;
            right_text = current_detail;
        }
        auto detail_panel = ftxui::vbox({
                              ftxui::text(right_title) | ftxui::bold,
                              ftxui::separator(),
                              ftxui::vbox(render_diff(right_text)) | ftxui::yframe | ftxui::flex,
                          }) |
                          ftxui::border | ftxui::flex;

        const auto branch = current.branch.empty() ? "(detached HEAD)" : current.branch;
        const auto page_name = view == View::Changes ? "\uf044  Changes"
                       : view == View::History ? "\uf1da  History"
                                   : "\ue0a0  Branches";
        const auto commit_label = mode == InputMode::CommitMessage
                                      ? "Commit message > "
                                      : "Commit message   ";
        const auto message_display = current_message.empty() && mode != InputMode::CommitMessage
                                         ? "(press i to write)"
                                         : current_message +
                                               (mode == InputMode::CommitMessage ? "_" : "");
        std::string footer;
        if (mode == InputMode::CommitMessage) {
            footer = "Type message  Enter Commit  Esc Cancel";
        } else if (mode == InputMode::NewBranch) {
            footer = "New branch: " + current_branch_input + "_  Enter Create  Esc Cancel";
        } else if (mode == InputMode::ConfirmDelete) {
            footer = "Delete " + current_pending_delete + "?  y Confirm  n/Esc Cancel";
        } else if (mode == InputMode::GhCommand) {
            footer = "Enter Run  Up/Down Command history  Esc Cancel";
        } else if (view == View::Changes) {
            footer = "1 Changes  2 History  3 Branches  j/k Select  Space/s Stage  i Commit  r Refresh  q Quit";
        } else if (view == View::History) {
            footer = "1 Changes  2 History  3 Branches  j/k Select commit  r Refresh  q Quit";
        } else if (view == View::Branches) {
            footer = "1 Changes  2 History  3 Branches  j/k Select  Enter Switch  c Create  d Delete  v Compare";
        } else {
            footer = "A: Authenticate with GitHub  q: Quit";
        }

        auto content = ftxui::Terminal::Size().dimx < 90
                           ? ftxui::vbox({
                                 list_panel | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 8),
                                 detail_panel | ftxui::flex,
                             }) |
                                 ftxui::flex
                           : ftxui::hbox({list_panel, detail_panel}) | ftxui::flex;

        ftxui::Elements cli_output_lines;
        std::istringstream cli_output(current_gh_output);
        std::string cli_output_line;
        while (std::getline(cli_output, cli_output_line)) {
            cli_output_lines.push_back(ftxui::text(cli_output_line));
        }
        if (cli_output_lines.empty()) {
            cli_output_lines.push_back(ftxui::text("GitHub CLI output will appear here."));
        }
        const auto cli_panel = ftxui::vbox({
            ftxui::hbox({
                ftxui::text("\uf120  GITHUB CLI") | ftxui::bold,
                ftxui::filler(),
                ftxui::text(is_busy ? "Working..." : current_notice),
            }),
            ftxui::separator(),
            ftxui::vbox(std::move(cli_output_lines)) | ftxui::yframe | ftxui::flex,
            ftxui::text(mode == InputMode::GhCommand
                            ? ":gh> " + current_gh_input + "_"
                            : "gh> : command  a auth  A login  p PR list  P PR create"),
        }) | ftxui::border | ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, 7) |
            ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 10);

        return ftxui::vbox({
                   ftxui::hbox({
                       ftxui::text("\uf09b  GitHub Desktop TUI") | ftxui::bold,
                       ftxui::filler(),
                       ftxui::text("\ue0a0 " + branch),
                   }),
                   ftxui::hbox({
                       ftxui::text("\uf044 1 Changes") |
                           (view == View::Changes ? ftxui::bold | ftxui::bgcolor(ftxui::Color::GrayDark)
                                                  : ftxui::nothing),
                       ftxui::text("  \uf1da 2 History") |
                           (view == View::History ? ftxui::bold | ftxui::bgcolor(ftxui::Color::GrayDark)
                                                  : ftxui::nothing),
                       ftxui::text("  \ue0a0 3 Branches") |
                           (view == View::Branches ? ftxui::bold | ftxui::bgcolor(ftxui::Color::GrayDark)
                                                   : ftxui::nothing),
                       ftxui::filler(),
                       ftxui::text(page_name),
                   }),
                   ftxui::hbox({
                       ftxui::text(current.path),
                       ftxui::filler(),
                       ftxui::text(current_gh_status) |
                           ftxui::color(is_gh_authenticated ? ftxui::Color::Green
                                                            : ftxui::Color::Yellow),
                   }),
                   ftxui::text(current.status),
                   current.has_conflicts
                       ? ftxui::text("Merge conflicts detected. Resolve them before committing.") |
                             ftxui::color(ftxui::Color::Red)
                       : ftxui::text(""),
                   ftxui::separator(),
                   content,
                   ftxui::separator(),
                   ftxui::text(commit_label + message_display),
                   cli_panel,
                   ftxui::text(footer),
               }) |
               ftxui::border;
    });

    app |= ftxui::CatchEvent([&](ftxui::Event event) {
        bool is_busy = false;
        bool is_editing = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            is_busy = busy;
            is_editing = input_mode != InputMode::None;
        }

        if (event == ftxui::Event::Escape) {
            if (is_editing) {
                std::lock_guard<std::mutex> lock(state_mutex);
                input_mode = InputMode::None;
                pending_delete_branch.clear();
                status_message = "Action cancelled.";
            } else {
                screen.ExitLoopClosure()();
            }
            return true;
        }
        if (event == ftxui::Event::Character('q') && !is_editing) {
            screen.ExitLoopClosure()();
            return true;
        }
        if (is_busy) {
            return true;
        }

        if (current_view == View::Home) {
            if (event == ftxui::Event::Character(':')) {
                std::lock_guard<std::mutex> lock(state_mutex);
                input_mode = InputMode::GhCommand;
                gh_history_index = gh_history.size();
                status_message = "Enter a gh command; shell syntax is not evaluated.";
                return true;
            }
            if (event == ftxui::Event::Character('A')) {
                run_gh_command({"gh", "auth", "login"});
                return true;
            }
            if (event == ftxui::Event::Character('a')) {
                run_gh_command({"gh", "auth", "status"});
                return true;
            }
            if (event == ftxui::Event::Character('r')) {
                run_background([&] {
                    {
                        std::lock_guard<std::mutex> lock(state_mutex);
                    }
                    refresh_gh_auth({});
                    std::lock_guard<std::mutex> lock(state_mutex);
                    status_message = "Repository list refreshed.";
                });
                return true;
            }
            if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k') ||
                event == ftxui::Event::ArrowDown || event == ftxui::Event::Character('j')) {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (!remote_repositories.empty()) {
                    if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k')) {
                        if (remote_repository_index > 0) {
                            --remote_repository_index;
                        }
                    } else if (remote_repository_index + 1 < remote_repositories.size()) {
                        ++remote_repository_index;
                    }
                }
                return true;
            }
            if (event == ftxui::Event::Return) {
                RemoteRepository selected;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    if (!gh_authenticated || remote_repository_index >= remote_repositories.size()) {
                        return true;
                    }
                    selected = remote_repositories[remote_repository_index];
                }
                run_background([&, selected] {
                    const char* home = std::getenv("HOME");
                    const auto destination = std::filesystem::path(home == nullptr ? "." : home) /
                                             "src" / selected.name;
                    std::filesystem::create_directories(destination.parent_path());
                    const auto result = ProcessRunner::run(
                        {"gh", "repo", "clone", selected.name, destination.string()});
                    if (result.exit_code != 0) {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        status_message = result.output.empty() ? "Repository clone failed."
                                                               : result.output;
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        repository_path = destination.string();
                        current_view = View::Changes;
                    }
                    refresh_data("Cloned " + selected.name + ".");
                    refresh_navigation("Cloned " + selected.name + ".");
                });
                return true;
            }
            return true;
        }

        if (input_mode == InputMode::ConfirmDelete) {
            if (event == ftxui::Event::Character('n') || event == ftxui::Event::Character('N')) {
                std::lock_guard<std::mutex> lock(state_mutex);
                input_mode = InputMode::None;
                pending_delete_branch.clear();
                status_message = "Branch deletion cancelled.";
                return true;
            }
            if (event == ftxui::Event::Character('y') || event == ftxui::Event::Character('Y')) {
                std::string branch_name;
                std::string repository_path;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    branch_name = pending_delete_branch;
                    repository_path = snapshot.path;
                    input_mode = InputMode::None;
                    pending_delete_branch.clear();
                }
                run_background([&, branch_name, repository_path] {
                    const auto result = Repository::delete_branch(repository_path, branch_name);
                    if (result.exit_code != 0) {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        status_message = result.output.empty() ? "Branch could not be deleted."
                                                               : result.output;
                        return;
                    }
                    refresh_data("Deleted branch " + branch_name + ".");
                    refresh_navigation("Deleted branch " + branch_name + ".");
                });
            }
            return true;
        }

        if (input_mode == InputMode::NewBranch) {
            if (event == ftxui::Event::Return) {
                std::string branch_name;
                std::string repository_path;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    branch_name = new_branch_name;
                    repository_path = snapshot.path;
                    input_mode = InputMode::None;
                }
                run_background([&, branch_name, repository_path] {
                    const auto result = Repository::create_branch(repository_path, branch_name);
                    if (result.exit_code != 0) {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        status_message = result.output.empty() ? "Branch could not be created."
                                                               : result.output;
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        new_branch_name.clear();
                        selected_branch_index = branch_list.size();
                    }
                    refresh_data("Created and switched to " + branch_name + ".");
                    refresh_navigation("Created and switched to " + branch_name + ".");
                });
                return true;
            }
            if (event == ftxui::Event::Backspace) {
                std::lock_guard<std::mutex> lock(state_mutex);
                erase_last_character(new_branch_name);
                return true;
            }
            if (event.is_character()) {
                std::lock_guard<std::mutex> lock(state_mutex);
                new_branch_name += event.character();
                return true;
            }
            return true;
        }

        if (input_mode == InputMode::CommitMessage) {
            if (event == ftxui::Event::Return) {
                std::string message;
                std::string repository_path;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    input_mode = InputMode::None;
                    message = commit_message;
                    repository_path = snapshot.path;
                }
                run_background([&, message, repository_path] {
                    const auto result = Repository::commit(repository_path, message);
                    if (result.exit_code != 0) {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        status_message = result.output.empty() ? "Commit failed." : result.output;
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        commit_message.clear();
                    }
                    refresh_data("Commit created.");
                    refresh_navigation("Commit created.");
                });
                return true;
            }
            if (event == ftxui::Event::Backspace) {
                std::lock_guard<std::mutex> lock(state_mutex);
                erase_last_character(commit_message);
                return true;
            }
            if (event.is_character()) {
                std::lock_guard<std::mutex> lock(state_mutex);
                commit_message += event.character();
                return true;
            }
            return true;
        }

        if (input_mode == InputMode::GhCommand) {
            if (event == ftxui::Event::Return) {
                std::string command_line;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    command_line = gh_input;
                }
                auto parsed = github::parse_command(command_line);
                if (!parsed.error.empty()) {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    gh_output = parsed.error;
                    detail_title = "gh command";
                    detail_text = parsed.error;
                    status_message = parsed.error;
                    input_mode = InputMode::None;
                    return true;
                }
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    if (!command_line.empty() &&
                        (gh_history.empty() || gh_history.back() != command_line)) {
                        gh_history.push_back(command_line);
                    }
                    gh_history_index = gh_history.size();
                    gh_input.clear();
                    input_mode = InputMode::None;
                }
                run_gh_command(std::move(parsed.arguments), parsed.force_interactive);
                return true;
            }
            if (event == ftxui::Event::Backspace) {
                std::lock_guard<std::mutex> lock(state_mutex);
                erase_last_character(gh_input);
                gh_history_index = gh_history.size();
                return true;
            }
            if (event == ftxui::Event::ArrowUp) {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (gh_history_index > 0) {
                    --gh_history_index;
                    gh_input = gh_history[gh_history_index];
                }
                return true;
            }
            if (event == ftxui::Event::ArrowDown) {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (gh_history_index + 1 < gh_history.size()) {
                    ++gh_history_index;
                    gh_input = gh_history[gh_history_index];
                } else {
                    gh_history_index = gh_history.size();
                    gh_input.clear();
                }
                return true;
            }
            if (event.is_character()) {
                std::lock_guard<std::mutex> lock(state_mutex);
                gh_input += event.character();
                gh_history_index = gh_history.size();
                return true;
            }
            return true;
        }

        if (event == ftxui::Event::Character('i')) {
            if (current_view != View::Changes) {
                return true;
            }
            std::lock_guard<std::mutex> lock(state_mutex);
            input_mode = InputMode::CommitMessage;
            status_message = "Editing commit message.";
            return true;
        }
        if (event == ftxui::Event::Character('1') ||
            event == ftxui::Event::Character('2') ||
            event == ftxui::Event::Character('3')) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                current_view = event == ftxui::Event::Character('1') ? View::Changes
                               : event == ftxui::Event::Character('2') ? View::History
                                                                       : View::Branches;
            }
            run_background([&] { refresh_navigation("View changed."); });
            return true;
        }
        if (event == ftxui::Event::Character('h')) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                current_view = View::History;
            }
            run_background([&] { refresh_navigation("History."); });
            return true;
        }
        if (event == ftxui::Event::Character('H')) {
            std::lock_guard<std::mutex> lock(state_mutex);
            current_view = View::Home;
            status_message = "GitHub Home.";
            return true;
        }
        if (event == ftxui::Event::Character('b')) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                current_view = View::Branches;
            }
            run_background([&] { refresh_navigation("Branches."); });
            return true;
        }
        if (event == ftxui::Event::Character(':')) {
            std::lock_guard<std::mutex> lock(state_mutex);
            input_mode = InputMode::GhCommand;
            gh_history_index = gh_history.size();
            status_message = "Enter a gh command; shell syntax is not evaluated.";
            return true;
        }
        if (event == ftxui::Event::Character('a')) {
            run_gh_command({"gh", "auth", "status"});
            return true;
        }
        if (event == ftxui::Event::Character('A')) {
            run_gh_command({"gh", "auth", "login"});
            return true;
        }
        if (event == ftxui::Event::Character('p')) {
            run_gh_command({"gh", "pr", "list", "--limit", "20"});
            return true;
        }
        if (event == ftxui::Event::Character('P')) {
            run_gh_command({"gh", "pr", "create"});
            return true;
        }
        if (event == ftxui::Event::Character('r')) {
            run_background([&] {
                refresh_data("Refreshed.");
                refresh_navigation("Refreshed.");
            });
            return true;
        }
        if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k') ||
            event == ftxui::Event::ArrowDown || event == ftxui::Event::Character('j')) {
            View view;
            std::string repository_path;
            FileChange selected_change;
            CommitInfo selected_commit;
            BranchInfo selected_branch;
            std::string current_branch;
            bool has_change = false;
            bool has_commit = false;
            bool has_branch = false;
            const bool move_up = event == ftxui::Event::ArrowUp ||
                                 event == ftxui::Event::Character('k');
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                view = current_view;
                repository_path = snapshot.path;
                current_branch = snapshot.branch;
                if (view == View::Changes && !snapshot.changes.empty()) {
                    if (move_up) {
                        if (selected_change_index > 0) {
                            --selected_change_index;
                        }
                    } else if (selected_change_index + 1 < snapshot.changes.size()) {
                        ++selected_change_index;
                    }
                    selected_change = snapshot.changes[selected_change_index];
                    has_change = true;
                } else if (view == View::History && !commit_list.empty()) {
                    if (move_up) {
                        if (selected_commit_index > 0) {
                            --selected_commit_index;
                        }
                    } else if (selected_commit_index + 1 < commit_list.size()) {
                        ++selected_commit_index;
                    }
                    selected_commit = commit_list[selected_commit_index];
                    has_commit = true;
                } else if (view == View::Branches && !branch_list.empty()) {
                    if (move_up) {
                        if (selected_branch_index > 0) {
                            --selected_branch_index;
                        }
                    } else if (selected_branch_index + 1 < branch_list.size()) {
                        ++selected_branch_index;
                    }
                    selected_branch = branch_list[selected_branch_index];
                    has_branch = true;
                }
            }
            if (has_change) {
                run_background([&, repository_path, selected_change] {
                    auto next_diff = Repository::diff(repository_path, selected_change);
                    std::lock_guard<std::mutex> lock(state_mutex);
                    diff_text = std::move(next_diff);
                    status_message = "Selected " + selected_change.path;
                });
            } else if (has_commit) {
                run_background([&, repository_path, selected_commit] {
                    auto next_detail = Repository::commit_detail(repository_path, selected_commit.id);
                    std::lock_guard<std::mutex> lock(state_mutex);
                    detail_title = selected_commit.short_id + "  " + selected_commit.subject;
                    detail_text = std::move(next_detail);
                    status_message = "Selected commit " + selected_commit.short_id;
                });
            } else if (has_branch) {
                std::lock_guard<std::mutex> lock(state_mutex);
                detail_title = "Branch: " + selected_branch.name;
                detail_text = selected_branch.is_current
                                  ? "This is the current branch.\n\nSelect another branch and press Enter to switch."
                                  : "Press Enter to switch to this branch.\nPress v to compare it with " +
                                        (current_branch.empty() ? "HEAD" : current_branch) + ".";
                status_message = "Selected branch " + selected_branch.name;
            }
            return true;
        }
        if (event == ftxui::Event::Character('c') && current_view == View::Branches) {
            std::lock_guard<std::mutex> lock(state_mutex);
            new_branch_name.clear();
            input_mode = InputMode::NewBranch;
            status_message = "Enter a branch name.";
            return true;
        }
        if (event == ftxui::Event::Character('d') && current_view == View::Branches) {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (selected_branch_index >= branch_list.size()) {
                return true;
            }
            if (branch_list[selected_branch_index].is_current) {
                status_message = "Switch to another branch before deleting this one.";
                return true;
            }
            pending_delete_branch = branch_list[selected_branch_index].name;
            input_mode = InputMode::ConfirmDelete;
            status_message = "Confirm branch deletion.";
            return true;
        }
        if (event == ftxui::Event::Character('v') && current_view == View::Branches) {
            std::string repository_path;
            std::string current_branch;
            std::string other_branch;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (selected_branch_index >= branch_list.size()) {
                    return true;
                }
                repository_path = snapshot.path;
                current_branch = snapshot.branch;
                other_branch = branch_list[selected_branch_index].name;
                detail_title = "Compare " +
                               (current_branch.empty() ? "HEAD" : current_branch) +
                               "..." + other_branch;
                detail_text = "Comparing branches...";
            }
            run_background([&, repository_path, current_branch, other_branch] {
                auto comparison = Repository::compare_branch(repository_path, current_branch,
                                                             other_branch);
                std::lock_guard<std::mutex> lock(state_mutex);
                detail_text = std::move(comparison);
                status_message = "Branch comparison ready.";
            });
            return true;
        }
        if (event == ftxui::Event::Return && current_view == View::Branches) {
            std::string repository_path;
            std::string branch_name;
            bool already_current = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (selected_branch_index >= branch_list.size()) {
                    return true;
                }
                branch_name = branch_list[selected_branch_index].name;
                repository_path = snapshot.path;
                already_current = branch_list[selected_branch_index].is_current;
            }
            if (already_current) {
                std::lock_guard<std::mutex> lock(state_mutex);
                status_message = "Already on " + branch_name + ".";
                return true;
            }
            run_background([&, repository_path, branch_name] {
                const auto result = Repository::switch_branch(repository_path, branch_name);
                if (result.exit_code != 0) {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    status_message = result.output.empty() ? "Branch switch failed." : result.output;
                    return;
                }
                refresh_data("Switched to " + branch_name + ".");
                refresh_navigation("Switched to " + branch_name + ".");
            });
            return true;
        }
        if (event == ftxui::Event::Character(' ') || event == ftxui::Event::Character('s')) {
            if (current_view != View::Changes) {
                return true;
            }
            FileChange selected_change;
            std::string repository_path;
            bool has_selection = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (selected_change_index < snapshot.changes.size()) {
                    selected_change = snapshot.changes[selected_change_index];
                    repository_path = snapshot.path;
                    has_selection = true;
                }
            }
            if (has_selection) {
                run_background([&, repository_path, selected_change] {
                    const auto result = Repository::toggle_stage(repository_path, selected_change);
                    if (result.exit_code != 0) {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        status_message = result.output.empty() ? "Stage operation failed."
                                                               : result.output;
                        return;
                    }
                    refresh_data(selected_change.is_staged() && !selected_change.is_unstaged()
                                     ? "File unstaged."
                                     : "File staged.");
                });
            }
            return true;
        }
        return false;
    });

    run_background([&] {
        if (!path.empty()) {
            refresh_data("Ready.");
            refresh_navigation("Ready.");
        }
        refresh_gh_auth(path);
        if (path.empty()) {
            std::lock_guard<std::mutex> lock(state_mutex);
            status_message = gh_authenticated ? "Ready to browse GitHub repositories."
                                             : "Sign in to GitHub to continue.";
        }
    });
    screen.Loop(app);
    if (worker.joinable()) {
        worker.join();
    }
    return 0;
}