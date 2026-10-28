#include "git/repository.h"
#include "github/command_parser.h"
#include "github/remote_browser.h"
#include "github/syntax_highlighter.h"
#include "ui/theme.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

enum class View { Home, Changes, History, Branches, RemoteFiles };
enum class InputMode { None, CommitMessage, NewBranch, ConfirmDelete, GhCommand, ThemePicker };

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

ftxui::Color status_color(const FileChange& change, const app::ThemePalette& palette) {
    if (change.is_untracked()) {
        return palette.warning;
    }
    if (change.is_conflicted() || change.index_status == 'D' || change.worktree_status == 'D') {
        return palette.error;
    }
    if (change.is_staged() && !change.is_unstaged()) {
        return palette.success;
    }
    return palette.accent;
}

ftxui::Elements render_diff(const std::string& diff, const app::ThemePalette& palette) {
    ftxui::Elements lines;
    std::istringstream input(diff);
    std::string line;
    while (std::getline(input, line)) {
        auto element = ftxui::text(line);
        if (!line.empty() && line[0] == '+') {
            element = element | ftxui::color(palette.added);
        } else if (!line.empty() && line[0] == '-') {
            element = element | ftxui::color(palette.removed);
        } else if (line.rfind("diff --", 0) == 0 ||
                   line.rfind("@@", 0) == 0 ||
                   line.rfind("Staged changes", 0) == 0 ||
                   line.rfind("Unstaged changes", 0) == 0) {
            element = element | ftxui::color(palette.accent);
        }
        lines.push_back(std::move(element));
    }
    if (lines.empty()) {
        lines.push_back(ftxui::text("Select a changed file to inspect its diff."));
    }
    return lines;
}

ftxui::Element render_syntax_line(const github::StyledLine& line,
                                  const app::ThemePalette& palette) {
    ftxui::Elements segments;
    for (const auto& span : line) {
        auto element = ftxui::text(span.text);
        switch (span.style) {
            case github::SyntaxStyle::Keyword:
                element = element | ftxui::color(palette.keyword) | ftxui::bold;
                break;
            case github::SyntaxStyle::Type:
                element = element | ftxui::color(palette.type);
                break;
            case github::SyntaxStyle::String:
            case github::SyntaxStyle::Code:
                element = element | ftxui::color(palette.string);
                break;
            case github::SyntaxStyle::Comment:
                element = element | ftxui::color(palette.comment);
                break;
            case github::SyntaxStyle::Number:
                element = element | ftxui::color(palette.number);
                break;
            case github::SyntaxStyle::Heading:
                element = element | ftxui::color(palette.heading) | ftxui::bold;
                break;
            case github::SyntaxStyle::Emphasis:
                element = element | ftxui::color(palette.emphasis) | ftxui::bold;
                break;
            case github::SyntaxStyle::Link:
                element = element | ftxui::color(palette.link);
                break;
            case github::SyntaxStyle::Plain:
                break;
        }
        segments.push_back(std::move(element));
    }
    return segments.empty() ? ftxui::text("") : ftxui::hbox(std::move(segments));
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

std::size_t visible_window_start(std::size_t selection, std::size_t visible_rows) {
    return selection >= visible_rows ? selection - visible_rows + 1 : 0;
}

std::filesystem::path theme_preference_path() {
    if (const char* config_home = std::getenv("XDG_CONFIG_HOME");
        config_home != nullptr && config_home[0] != '\0') {
        return std::filesystem::path(config_home) / "github-desktop-tui" / "theme";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "github-desktop-tui" / "theme";
    }
    return {};
}

std::size_t theme_index(app::ThemeName theme) {
    const auto& themes = app::available_themes();
    const auto found = std::find(themes.begin(), themes.end(), theme);
    return found == themes.end() ? 0 : static_cast<std::size_t>(found - themes.begin());
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "github-desktop-tui " << GITHUB_DESKTOP_TUI_VERSION << '\n';
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: github-desktop-tui [repository-path]\n"
                     "       github-desktop-tui --version\n"
                     "       github-desktop-tui --help\n";
        return 0;
    }
    if (argc > 2) {
        return 2;
    }

    const std::string path = argc == 2 ? argv[1] : "";
    auto screen = ftxui::ScreenInteractive::Fullscreen();
    screen.TrackMouse(true);
    const auto theme_path = theme_preference_path();
    auto active_theme = app::load_theme_preference(theme_path).value_or(app::ThemeName::Selenized);
    auto theme_selection = theme_index(active_theme);
    std::mutex state_mutex;
    std::string repository_path = path;
    RepositorySnapshot snapshot;
    std::vector<RemoteRepository> remote_repositories;
    std::vector<github::RepositoryContent> remote_entries;
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
    std::string remote_repository_name;
    std::string remote_directory_path;
    std::string remote_file_path;
    std::string remote_file_content;
    std::string remote_browser_error;
    std::vector<std::string> gh_history;
    std::size_t gh_history_index = 0;
    std::size_t remote_repository_index = 0;
    std::size_t remote_entry_index = 0;
    std::size_t remote_preview_scroll = 0;
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

    auto refresh_remote_directory = [&](const std::string& repository_name,
                                         const std::string& directory_path) {
        std::string endpoint = "repos/" + repository_name + "/contents";
        if (!directory_path.empty()) {
            endpoint += "/" + github::encode_repository_path(directory_path);
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            remote_repository_name = repository_name;
            remote_directory_path = directory_path;
            remote_entries.clear();
            remote_entry_index = 0;
            remote_file_path.clear();
            remote_file_content.clear();
            remote_preview_scroll = 0;
            remote_browser_error.clear();
            current_view = View::RemoteFiles;
        }
        run_background([&, repository_name, directory_path, endpoint] {
            const auto result = ProcessRunner::run({
                "gh", "api", "--paginate", "--jq",
                ".[] | [(.name | @uri), .type, (.path | @uri), (.size | tostring)] | @tsv",
                endpoint,
            });
            if (result.exit_code != 0) {
                const auto error = result.output.empty() ? "Could not list remote contents."
                                                         : result.output;
                std::lock_guard<std::mutex> lock(state_mutex);
                remote_browser_error = error;
                gh_output = error;
                status_message = error;
                return;
            }
            auto contents = github::parse_repository_contents(result.output);
            std::lock_guard<std::mutex> lock(state_mutex);
            remote_entries = std::move(contents);
            remote_entry_index = 0;
            remote_browser_error.clear();
            gh_output = "Loaded " + std::to_string(remote_entries.size()) + " entries from " +
                        repository_name + (directory_path.empty() ? "" : "/" + directory_path);
            status_message = remote_entries.empty() ? "Directory is empty."
                                                    : "Remote directory loaded.";
        });
    };

    auto load_remote_file = [&](const std::string& repository_name,
                                const std::string& file_path) {
        const auto endpoint = "repos/" + repository_name + "/contents/" +
                              github::encode_repository_path(file_path);
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            remote_file_path = file_path;
            remote_file_content = "Loading file...";
            remote_preview_scroll = 0;
            remote_browser_error.clear();
        }
        run_background([&, repository_name, file_path, endpoint] {
            const auto result = ProcessRunner::run(
                {"gh", "api", "-H", "Accept: application/vnd.github.raw+json", endpoint});
            std::string content;
            std::string error;
            if (result.exit_code != 0) {
                error = result.output.empty() ? "Could not read remote file." : result.output;
            } else if (result.output.find('\0') != std::string::npos) {
                error = "Binary file preview is not available in the terminal.";
            } else {
                content = result.output;
                constexpr std::size_t preview_limit = 512 * 1024;
                if (content.size() > preview_limit) {
                    content.resize(preview_limit);
                    content += "\n\n[Preview truncated at 512 KiB]";
                }
            }
            std::lock_guard<std::mutex> lock(state_mutex);
            remote_file_content = error.empty() ? std::move(content) : error;
            remote_browser_error = std::move(error);
            gh_output = remote_browser_error.empty() ? "Read " + repository_name + "/" + file_path
                                                     : remote_browser_error;
            status_message = remote_browser_error.empty() ? "Remote file loaded."
                                                          : "Could not load remote file.";
        });
    };

    auto open_remote_entry = [&](std::size_t index) {
        std::string repository_name;
        github::RepositoryContent selected;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (index >= remote_entries.size()) {
                return;
            }
            remote_entry_index = index;
            repository_name = remote_repository_name;
            selected = remote_entries[index];
        }
        if (selected.is_directory) {
            refresh_remote_directory(repository_name, selected.path);
        } else {
            load_remote_file(repository_name, selected.path);
        }
    };

    auto browse_selected_remote_repository = [&] {
        std::string selected_repository;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!gh_authenticated || remote_repository_index >= remote_repositories.size()) {
                return;
            }
            selected_repository = remote_repositories[remote_repository_index].name;
        }
        refresh_remote_directory(selected_repository, {});
    };

    auto clone_selected_remote_repository = [&] {
        RemoteRepository selected;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!gh_authenticated || remote_repository_index >= remote_repositories.size()) {
                return;
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
    };

    auto switch_view = [&](View target) {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if ((target == View::Changes || target == View::History || target == View::Branches) &&
                !snapshot.is_repository) {
                return;
            }
            if (target == View::RemoteFiles && remote_repository_name.empty()) {
                return;
            }
            current_view = target;
            status_message = target == View::Home ? "GitHub Home."
                             : target == View::RemoteFiles ? "Remote repository contents."
                                                           : "View changed.";
        }
        if (target == View::History || target == View::Branches) {
            run_background([&] { refresh_navigation("View changed."); });
        }
    };

    auto cycle_view = [&](bool reverse) {
        std::vector<View> views = {View::Home};
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (snapshot.is_repository) {
                views.push_back(View::Changes);
                views.push_back(View::History);
                views.push_back(View::Branches);
            }
            if (!remote_repository_name.empty()) {
                views.push_back(View::RemoteFiles);
            }
            const auto current = std::find(views.begin(), views.end(), current_view);
            if (current == views.end()) {
                current_view = views.front();
            } else {
                auto index = static_cast<std::size_t>(current - views.begin());
                index = reverse ? (index + views.size() - 1) % views.size()
                                : (index + 1) % views.size();
                current_view = views[index];
            }
            status_message = "View changed.";
        }
        View target;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            target = current_view;
        }
        if (target == View::History || target == View::Branches) {
            run_background([&] { refresh_navigation("View changed."); });
        }
    };

    auto run_git_remote_action = [&](char action) {
        std::string active_path;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            active_path = snapshot.path;
        }
        if (active_path.empty()) {
            return;
        }
        run_background([&, active_path, action] {
            ProcessResult result;
            std::string operation;
            std::string success_message;
            if (action == 'f') {
                operation = "fetch";
                success_message = "Fetched remote updates.";
                result = Repository::fetch(active_path);
            } else if (action == 'l') {
                operation = "pull";
                success_message = "Pulled remote updates.";
                result = Repository::pull(active_path);
            } else {
                operation = "push";
                success_message = "Pushed commits to the upstream.";
                result = Repository::push(active_path);
            }
            if (result.exit_code != 0) {
                std::lock_guard<std::mutex> lock(state_mutex);
                const auto output = result.output.empty()
                                        ? "Git " + operation + " failed."
                                        : result.output;
                gh_output = output;
                detail_title = "git " + operation;
                detail_text = output;
                status_message = output;
                return;
            }
            refresh_data(success_message);
            refresh_navigation(success_message);
            std::lock_guard<std::mutex> lock(state_mutex);
            gh_output = result.output.empty() ? success_message : result.output;
            detail_title = "git " + operation;
            detail_text = gh_output;
        });
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
        std::vector<github::RepositoryContent> current_remote_entries;
        std::size_t change_selection = 0;
        std::size_t branch_selection = 0;
        std::size_t commit_selection = 0;
        std::size_t remote_selection = 0;
        std::size_t remote_entry_selection = 0;
        std::size_t current_remote_preview_scroll = 0;
        std::string current_remote_error;
        std::string current_remote_repository_name;
        std::string current_remote_directory_path;
        std::string current_remote_file_path;
        std::string current_remote_file_content;
        std::string current_remote_browser_error;
        app::ThemeName current_theme = app::ThemeName::Selenized;
        std::size_t current_theme_selection = 0;
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
            current_remote_entries = remote_entries;
            change_selection = selected_change_index;
            branch_selection = selected_branch_index;
            commit_selection = selected_commit_index;
            remote_selection = remote_repository_index;
            remote_entry_selection = remote_entry_index;
            current_remote_preview_scroll = remote_preview_scroll;
            current_remote_error = remote_error;
            current_remote_repository_name = remote_repository_name;
            current_remote_directory_path = remote_directory_path;
            current_remote_file_path = remote_file_path;
            current_remote_file_content = remote_file_content;
            current_remote_browser_error = remote_browser_error;
            current_theme = active_theme;
            current_theme_selection = theme_selection;
            view = current_view;
            mode = input_mode;
            is_busy = busy;
            is_gh_authenticated = gh_authenticated;
        }

        const auto displayed_theme = mode == InputMode::ThemePicker
                                         ? app::available_themes()[current_theme_selection]
                                         : current_theme;
        const auto palette = app::theme_palette(displayed_theme);

        if (mode == InputMode::ThemePicker) {
            ftxui::Elements theme_rows;
            const auto& themes = app::available_themes();
            for (std::size_t index = 0; index < themes.size(); ++index) {
                const auto option_palette = app::theme_palette(themes[index]);
                auto row = ftxui::hbox({
                    ftxui::text(std::to_string(index + 1) + "  "),
                    ftxui::text(app::theme_name(themes[index])) | ftxui::bold,
                    ftxui::filler(),
                    ftxui::text(" kw ") | ftxui::color(option_palette.keyword),
                    ftxui::text(" type ") | ftxui::color(option_palette.type),
                    ftxui::text(" string ") | ftxui::color(option_palette.string),
                    ftxui::text(" # ") | ftxui::color(option_palette.comment),
                });
                if (index == current_theme_selection) {
                    row = row | ftxui::bgcolor(palette.selection_background) |
                          ftxui::color(palette.selection_foreground);
                }
                theme_rows.push_back(std::move(row));
            }
            return ftxui::vbox({
                       ftxui::text("GitHub Desktop TUI") | ftxui::bold,
                       ftxui::separator(),
                       ftxui::text("COLOR THEME") | ftxui::bold | ftxui::color(palette.accent),
                       ftxui::text("Choose a palette for the entire interface and code preview."),
                       ftxui::separator(),
                       ftxui::vbox(std::move(theme_rows)),
                       ftxui::separator(),
                       ftxui::text("j/k or arrows Select  1-4 Choose  Enter Apply  Esc Cancel"),
                   }) |
                   ftxui::color(palette.foreground) | ftxui::bgcolor(palette.background) |
                   ftxui::border;
        }

        auto navigation_tab = [&](const std::string& label, View target) {
            auto element = ftxui::text(label);
            if (view == target) {
                element = element | ftxui::bold | ftxui::bgcolor(palette.selection_background) |
                          ftxui::color(palette.selection_foreground);
            }
            return element;
        };
        ftxui::Elements navigation_items = {
            navigation_tab("\uf015 Home", View::Home),
        };
        if (current.is_repository) {
            navigation_items.push_back(ftxui::text("  "));
            navigation_items.push_back(navigation_tab("\uf044 Changes", View::Changes));
            navigation_items.push_back(ftxui::text("  "));
            navigation_items.push_back(navigation_tab("\uf1da History", View::History));
            navigation_items.push_back(ftxui::text("  "));
            navigation_items.push_back(navigation_tab("\ue0a0 Branches", View::Branches));
        }
        if (!current_remote_repository_name.empty()) {
            navigation_items.push_back(ftxui::text("  "));
            navigation_items.push_back(navigation_tab("\uf07c Remote Files", View::RemoteFiles));
        }
        const auto navigation_tabs = ftxui::hbox(std::move(navigation_items));

        if (view == View::Home) {
            ftxui::Elements repository_rows;
            const auto visible_rows = static_cast<std::size_t>(
                std::max(1, ftxui::Terminal::Size().dimy - 16));
            const auto first_row = visible_window_start(remote_selection, visible_rows);
            const auto last_row = std::min(current_remote_repositories.size(),
                                           first_row + visible_rows);
            for (std::size_t index = first_row; index < last_row; ++index) {
                const auto& repository = current_remote_repositories[index];
                const auto icon = repository.visibility == "private" ? "\uf023 " : "\uf0ac ";
                auto row = ftxui::text(icon + repository.name);
                if (index == remote_selection) {
                      row = row | ftxui::bgcolor(palette.selection_background) |
                          ftxui::color(palette.selection_foreground);
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
                ftxui::text("\uf07c  Browse files  [v]"),
                ftxui::text("\uf0c5  Clone repository  [Enter]"),
                ftxui::text("Location: ~/src/OWNER/REPOSITORY"),
                ftxui::text(current_remote_error),
            }) | ftxui::border | ftxui::flex;
            ftxui::Elements output_lines;
            std::istringstream output(current_gh_output);
            std::string output_line;
            while (std::getline(output, output_line)) {
                output_lines.push_back(ftxui::text(output_line));
            }
            if (output_lines.empty()) {
                output_lines.push_back(ftxui::text("Git and GitHub results will appear here."));
            }
            const auto cli_panel = ftxui::vbox({
                ftxui::hbox({
                    ftxui::text("\uf120  GIT & GITHUB OUTPUT") | ftxui::bold,
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
                        ftxui::color(is_gh_authenticated ? palette.success : palette.warning),
                }),
                navigation_tabs,
                ftxui::hbox({repository_list, repository_detail}) | ftxui::flex,
                cli_panel,
                ftxui::text("A Sign in  r Refresh  j/k or mouse select  v Browse  Enter Clone  t Theme  : CLI  q Quit"),
            }) | ftxui::color(palette.foreground) | ftxui::bgcolor(palette.background) |
                ftxui::border;
        }

        if (view == View::RemoteFiles) {
            ftxui::Elements remote_rows;
            const auto visible_rows = static_cast<std::size_t>(
                std::max(1, ftxui::Terminal::Size().dimy - 11));
            const auto first_row = visible_window_start(remote_entry_selection, visible_rows);
            const auto last_row = std::min(current_remote_entries.size(), first_row + visible_rows);
            for (std::size_t index = first_row; index < last_row; ++index) {
                const auto& entry = current_remote_entries[index];
                auto row = ftxui::text(std::string(entry.is_directory ? "\uf07b  " : "\uf15b  ") +
                                       entry.name);
                if (index == remote_entry_selection) {
                      row = row | ftxui::bgcolor(palette.selection_background) |
                          ftxui::color(palette.selection_foreground);
                }
                remote_rows.push_back(std::move(row));
            }
            if (remote_rows.empty()) {
                remote_rows.push_back(ftxui::text(current_remote_browser_error.empty()
                                                      ? "Directory is empty."
                                                      : current_remote_browser_error));
            }
            const auto remote_list = ftxui::vbox({
                ftxui::text("\uf07c  " + current_remote_repository_name) | ftxui::bold,
                ftxui::separator(),
                ftxui::vbox(std::move(remote_rows)) | ftxui::yframe | ftxui::flex,
            }) | ftxui::border | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 32) |
                ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 48) | ftxui::flex;

            ftxui::Elements preview_lines;
            const auto all_preview_lines = github::highlight_file(current_remote_file_path,
                                                                  current_remote_file_content);
            if (all_preview_lines.empty()) {
                preview_lines.push_back(ftxui::text("Select a file to preview it without cloning."));
            } else {
                const auto preview_start = std::min(current_remote_preview_scroll,
                    all_preview_lines.size() > visible_rows
                        ? all_preview_lines.size() - visible_rows
                        : std::size_t{0});
                const auto preview_end = std::min(all_preview_lines.size(),
                                                  preview_start + visible_rows);
                for (std::size_t index = preview_start; index < preview_end; ++index) {
                    preview_lines.push_back(render_syntax_line(all_preview_lines[index], palette));
                }
            }
            const auto preview_panel = ftxui::vbox({
                ftxui::text(current_remote_file_path.empty() ? "PREVIEW"
                                                             : current_remote_file_path) |
                    ftxui::bold,
                ftxui::separator(),
                ftxui::vbox(std::move(preview_lines)) | ftxui::yframe | ftxui::flex,
            }) | ftxui::border | ftxui::flex;

            const auto location = current_remote_directory_path.empty()
                                      ? current_remote_repository_name
                                      : current_remote_repository_name + "/" +
                                            current_remote_directory_path;
            return ftxui::vbox({
                ftxui::hbox({
                    ftxui::text("\uf09b  GitHub Desktop TUI") | ftxui::bold,
                    ftxui::filler(),
                    ftxui::text(current_gh_status),
                }),
                navigation_tabs,
                ftxui::text(location),
                ftxui::text(is_busy ? "Loading remote content..."
                                    : current_remote_browser_error.empty() ? current_notice
                                                                          : current_remote_browser_error),
                ftxui::hbox({remote_list, preview_panel}) | ftxui::flex,
                ftxui::text("TAB/Shift+TAB Screens  Click/Enter Open  Wheel/PageUp/Down Preview  Backspace Parent  t Theme  H Home  q Quit"),
            }) | ftxui::color(palette.foreground) | ftxui::bgcolor(palette.background) |
                ftxui::border;
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
                               ftxui::color(status_color(change, palette));
                    if (index == selection) {
                        row = row | ftxui::bgcolor(palette.selection_background);
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
                        row = row | ftxui::bgcolor(palette.selection_background);
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
                        row = row | ftxui::bgcolor(palette.selection_background);
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
                              ftxui::vbox(render_diff(right_text, palette)) |
                                  ftxui::yframe | ftxui::flex,
                          }) |
                          ftxui::border | ftxui::flex;

        const auto branch = current.branch.empty() ? "(detached HEAD)" : current.branch;
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
            footer = "TAB Screens  Mouse Select  Space/s Stage  i Commit  f Fetch  l Pull  u Push  r Refresh  t Theme  q Quit";
        } else if (view == View::History) {
            footer = "TAB Screens  Mouse Select  f Fetch  l Pull  u Push  r Refresh  t Theme  q Quit";
        } else if (view == View::Branches) {
            footer = "TAB Screens  Mouse Select  Enter Switch  c Create  d Delete  v Compare  f Fetch  l Pull  u Push  t Theme";
        } else {
            footer = "A: Authenticate with GitHub  t Theme  q: Quit";
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
            cli_output_lines.push_back(ftxui::text("Git and GitHub results will appear here."));
        }
        const auto cli_panel = ftxui::vbox({
            ftxui::hbox({
                ftxui::text("\uf120  GIT & GITHUB OUTPUT") | ftxui::bold,
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
                   navigation_tabs,
                   ftxui::hbox({
                       ftxui::text(current.path),
                       ftxui::filler(),
                       ftxui::text(current_gh_status) |
                           ftxui::color(is_gh_authenticated ? palette.success : palette.warning),
                   }),
                   ftxui::text(current.status),
                   current.has_conflicts
                       ? ftxui::text("Merge conflicts detected. Resolve them before committing.") |
                             ftxui::color(palette.error)
                       : ftxui::text(""),
                   ftxui::separator(),
                   content,
                   ftxui::separator(),
                   ftxui::text(commit_label + message_display),
                   cli_panel,
                   ftxui::text(footer),
               }) |
               ftxui::color(palette.foreground) | ftxui::bgcolor(palette.background) |
               ftxui::border;
    });

    app |= ftxui::CatchEvent([&](ftxui::Event event) {
        bool is_busy = false;
        bool is_editing = false;
        InputMode current_input_mode = InputMode::None;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            is_busy = busy;
            is_editing = input_mode != InputMode::None;
            current_input_mode = input_mode;
        }

        if (current_input_mode == InputMode::ThemePicker) {
            if (event == ftxui::Event::Escape) {
                std::lock_guard<std::mutex> lock(state_mutex);
                input_mode = InputMode::None;
                theme_selection = theme_index(active_theme);
                status_message = "Theme selection cancelled.";
                return true;
            }
            if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k') ||
                event == ftxui::Event::ArrowDown || event == ftxui::Event::Character('j')) {
                std::lock_guard<std::mutex> lock(state_mutex);
                const auto count = app::available_themes().size();
                if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k')) {
                    theme_selection = theme_selection == 0 ? count - 1 : theme_selection - 1;
                } else {
                    theme_selection = (theme_selection + 1) % count;
                }
                return true;
            }
            for (std::size_t index = 0; index < app::available_themes().size(); ++index) {
                if (event == ftxui::Event::Character(static_cast<char>('1' + index))) {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    theme_selection = index;
                    return true;
                }
            }
            if (event == ftxui::Event::Return) {
                app::ThemeName selected_theme;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    selected_theme = app::available_themes()[theme_selection];
                }
                const bool saved = !theme_path.empty() &&
                                   app::save_theme_preference(theme_path, selected_theme);
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    active_theme = selected_theme;
                    input_mode = InputMode::None;
                    status_message = "Theme changed to " +
                                     std::string(app::theme_name(selected_theme)) +
                                     (saved ? "." : "; preference could not be saved.");
                }
                return true;
            }
            return true;
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

        if (!is_editing && event == ftxui::Event::Character('t')) {
            std::lock_guard<std::mutex> lock(state_mutex);
            theme_selection = theme_index(active_theme);
            input_mode = InputMode::ThemePicker;
            status_message = "Choose a color theme.";
            return true;
        }

        if (!is_editing && event == ftxui::Event::Tab) {
            cycle_view(false);
            return true;
        }
        if (!is_editing && event == ftxui::Event::TabReverse) {
            cycle_view(true);
            return true;
        }

        if (!is_editing && event.is_mouse()) {
            const auto mouse = event.mouse();
            if (mouse.button == ftxui::Mouse::WheelUp ||
                mouse.button == ftxui::Mouse::WheelDown) {
                const bool move_up = mouse.button == ftxui::Mouse::WheelUp;
                if (current_view == View::Home) {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    if (move_up && remote_repository_index > 0) {
                        --remote_repository_index;
                    } else if (!move_up && remote_repository_index + 1 < remote_repositories.size()) {
                        ++remote_repository_index;
                    }
                    return true;
                }
                if (current_view == View::RemoteFiles) {
                    if (mouse.x >= ftxui::Terminal::Size().dimx / 2) {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        if (move_up) {
                            remote_preview_scroll = remote_preview_scroll > 3
                                                        ? remote_preview_scroll - 3
                                                        : 0;
                        } else {
                            remote_preview_scroll += 3;
                        }
                    } else {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        if (move_up && remote_entry_index > 0) {
                            --remote_entry_index;
                        } else if (!move_up && remote_entry_index + 1 < remote_entries.size()) {
                            ++remote_entry_index;
                        }
                    }
                    return true;
                }
                event = ftxui::Event::Character(move_up ? 'k' : 'j');
            } else if (mouse.button == ftxui::Mouse::Left &&
                       mouse.motion == ftxui::Mouse::Pressed) {
                if (mouse.y == 2) {
                    std::vector<std::pair<std::string, View>> tabs = {
                        {"\uf015 Home", View::Home},
                    };
                    {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        if (snapshot.is_repository) {
                            tabs.emplace_back("\uf044 Changes", View::Changes);
                            tabs.emplace_back("\uf1da History", View::History);
                            tabs.emplace_back("\ue0a0 Branches", View::Branches);
                        }
                        if (!remote_repository_name.empty()) {
                            tabs.emplace_back("\uf07c Remote Files", View::RemoteFiles);
                        }
                    }
                    int tab_x = 1;
                    for (const auto& [label, target] : tabs) {
                        const int tab_width = ftxui::string_width(label);
                        if (mouse.x >= tab_x && mouse.x < tab_x + tab_width) {
                            switch_view(target);
                            return true;
                        }
                        tab_x += tab_width + 2;
                    }
                    return true;
                }
                const int half_width = ftxui::Terminal::Size().dimx / 2;
                if (current_view == View::Home) {
                    if (mouse.x < half_width && mouse.y >= 6) {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        const auto visible_rows = static_cast<std::size_t>(
                            std::max(1, ftxui::Terminal::Size().dimy - 16));
                        const auto index = visible_window_start(remote_repository_index,
                                                                visible_rows) +
                                           static_cast<std::size_t>(mouse.y - 6);
                        if (index < remote_repositories.size()) {
                            remote_repository_index = index;
                        }
                    } else if (mouse.x >= half_width && mouse.y == 8) {
                        browse_selected_remote_repository();
                    } else if (mouse.x >= half_width && mouse.y == 9) {
                        clone_selected_remote_repository();
                    }
                    return true;
                }
                if (current_view == View::RemoteFiles) {
                    if (mouse.x < half_width && mouse.y >= 8) {
                        const auto visible_rows = static_cast<std::size_t>(
                            std::max(1, ftxui::Terminal::Size().dimy - 11));
                        const auto index = visible_window_start(remote_entry_index,
                                                                visible_rows) +
                                           static_cast<std::size_t>(mouse.y - 8);
                        open_remote_entry(index);
                    }
                    return true;
                }
                if (mouse.y >= 10 &&
                    (ftxui::Terminal::Size().dimx < 90 || mouse.x < 44)) {
                    const auto index = static_cast<std::size_t>(mouse.y - 10);
                    View selected_view;
                    std::string active_path;
                    FileChange selected_change;
                    CommitInfo selected_commit;
                    BranchInfo selected_branch;
                    bool select_change = false;
                    bool select_commit = false;
                    bool select_branch = false;
                    std::string current_branch;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex);
                        selected_view = current_view;
                        active_path = snapshot.path;
                        current_branch = snapshot.branch;
                        if (selected_view == View::Changes && index < snapshot.changes.size()) {
                            selected_change_index = index;
                            selected_change = snapshot.changes[index];
                            select_change = true;
                        } else if (selected_view == View::History && index < commit_list.size()) {
                            selected_commit_index = index;
                            selected_commit = commit_list[index];
                            select_commit = true;
                        } else if (selected_view == View::Branches && index < branch_list.size()) {
                            selected_branch_index = index;
                            selected_branch = branch_list[index];
                            select_branch = true;
                        }
                    }
                    if (select_change) {
                        run_background([&, active_path, selected_change] {
                            auto next_diff = Repository::diff(active_path, selected_change);
                            std::lock_guard<std::mutex> lock(state_mutex);
                            diff_text = std::move(next_diff);
                            status_message = "Selected " + selected_change.path;
                        });
                    } else if (select_commit) {
                        run_background([&, active_path, selected_commit] {
                            auto next_detail = Repository::commit_detail(active_path, selected_commit.id);
                            std::lock_guard<std::mutex> lock(state_mutex);
                            detail_title = selected_commit.short_id + "  " + selected_commit.subject;
                            detail_text = std::move(next_detail);
                            status_message = "Selected commit " + selected_commit.short_id;
                        });
                    } else if (select_branch) {
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
            }
        }

        if (current_view == View::RemoteFiles && input_mode == InputMode::None) {
            if (event == ftxui::Event::Backspace) {
                std::string repository_name;
                std::string directory_path;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    repository_name = remote_repository_name;
                    if (!remote_file_path.empty()) {
                        remote_file_path.clear();
                        remote_file_content.clear();
                        remote_preview_scroll = 0;
                        return true;
                    }
                    directory_path = remote_directory_path;
                }
                if (!directory_path.empty()) {
                    refresh_remote_directory(repository_name,
                                             github::parent_repository_path(directory_path));
                } else {
                    switch_view(View::Home);
                }
                return true;
            }
            if (event == ftxui::Event::Return) {
                std::size_t index;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    index = remote_entry_index;
                }
                open_remote_entry(index);
                return true;
            }
            if (event == ftxui::Event::PageUp || event == ftxui::Event::PageDown) {
                std::lock_guard<std::mutex> lock(state_mutex);
                const auto page = static_cast<std::size_t>(
                    std::max(1, ftxui::Terminal::Size().dimy - 11));
                if (event == ftxui::Event::PageUp) {
                    remote_preview_scroll = remote_preview_scroll > page
                                                ? remote_preview_scroll - page
                                                : 0;
                } else {
                    remote_preview_scroll += page;
                }
                return true;
            }
            if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k') ||
                event == ftxui::Event::ArrowDown || event == ftxui::Event::Character('j')) {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k')) {
                    if (remote_entry_index > 0) {
                        --remote_entry_index;
                    }
                } else if (remote_entry_index + 1 < remote_entries.size()) {
                    ++remote_entry_index;
                }
                return true;
            }
            return true;
        }

        if (current_view == View::Home && input_mode == InputMode::None) {
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
            if (event == ftxui::Event::Character('v')) {
                browse_selected_remote_repository();
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
                clone_selected_remote_repository();
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
        if (event == ftxui::Event::Character('f')) {
            run_git_remote_action('f');
            return true;
        }
        if (event == ftxui::Event::Character('l')) {
            run_git_remote_action('l');
            return true;
        }
        if (event == ftxui::Event::Character('u')) {
            run_git_remote_action('u');
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