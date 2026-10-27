#include "git/repository.h"

#include "process/process_runner.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string trim(std::string value) {
    const auto not_space = [](unsigned char character) {
        return std::isspace(character) == 0;
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(delimiter, start);
        fields.push_back(value.substr(start, end == std::string::npos
                                                ? std::string::npos
                                                : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return fields;
}

ProcessResult validate_branch(const std::string& repository_path,
                              const std::string& name) {
    if (trim(name).empty()) {
        return {1, "Branch name cannot be empty."};
    }
    return ProcessRunner::run(
        {"git", "-C", repository_path, "check-ref-format", "--branch", name});
}

}  // namespace

bool FileChange::is_untracked() const {
    return index_status == '?' && worktree_status == '?';
}

bool FileChange::is_staged() const {
    return index_status != ' ' && index_status != '?';
}

bool FileChange::is_unstaged() const {
    return worktree_status != ' ' && worktree_status != '?';
}

bool FileChange::is_conflicted() const {
    const std::string code{index_status, worktree_status};
    return code == "DD" || code == "AU" || code == "UD" || code == "UA" ||
           code == "DU" || code == "AA" || code == "UU";
}

RepositorySnapshot Repository::inspect(const std::string& path) {
    RepositorySnapshot snapshot;
    const auto root = ProcessRunner::run(
        {"git", "-C", path, "rev-parse", "--show-toplevel"});
    if (root.exit_code != 0) {
        snapshot.error = trim(root.output);
        if (snapshot.error.empty()) {
            snapshot.error = "The selected directory is not a Git repository.";
        }
        return snapshot;
    }

    snapshot.path = trim(root.output);
    const auto branch = ProcessRunner::run(
        {"git", "-C", snapshot.path, "branch", "--show-current"});
    if (branch.exit_code != 0) {
        snapshot.error = trim(branch.output);
        return snapshot;
    }

    snapshot.branch = trim(branch.output);
    const auto summary = ProcessRunner::run(
        {"git", "-C", snapshot.path, "status", "--short", "--branch"});
    if (summary.exit_code != 0) {
        snapshot.error = trim(summary.output);
        return snapshot;
    }

    snapshot.status = trim(summary.output);
    const auto status = ProcessRunner::run(
        {"git", "-C", snapshot.path, "status", "--porcelain=v1", "-z",
         "--untracked-files=all"});
    if (status.exit_code != 0) {
        snapshot.error = trim(status.output);
        return snapshot;
    }

    std::size_t offset = 0;
    while (offset < status.output.size()) {
        const auto end = status.output.find('\0', offset);
        const auto entry_end = end == std::string::npos ? status.output.size() : end;
        const std::string entry = status.output.substr(offset, entry_end - offset);
        offset = end == std::string::npos ? status.output.size() : end + 1;

        if (entry.size() < 4) {
            continue;
        }

        FileChange change;
        change.index_status = entry[0];
        change.worktree_status = entry[1];
        change.path = entry.substr(3);
        snapshot.has_conflicts = snapshot.has_conflicts || change.is_conflicted();
        snapshot.changes.push_back(std::move(change));

        if ((snapshot.changes.back().index_status == 'R' ||
             snapshot.changes.back().index_status == 'C' ||
             snapshot.changes.back().worktree_status == 'R' ||
             snapshot.changes.back().worktree_status == 'C') &&
            offset < status.output.size()) {
            const auto original_end = status.output.find('\0', offset);
            offset = original_end == std::string::npos
                         ? status.output.size()
                         : original_end + 1;
        }
    }

    snapshot.is_repository = true;
    return snapshot;
}

std::string Repository::diff(const std::string& repository_path,
                             const FileChange& change) {
    if (change.is_untracked()) {
        const auto file_path = (std::filesystem::path(repository_path) / change.path).string();
        const auto result = ProcessRunner::run(
            {"git", "-C", repository_path, "diff", "--no-index", "--no-ext-diff",
             "--no-color", "--", "/dev/null", file_path});
        if (result.exit_code <= 1) {
            return result.output;
        }
        return result.output.empty() ? "Unable to read diff for " + change.path
                                     : result.output;
    }

    std::string output;
    if (change.is_staged()) {
        const auto staged = ProcessRunner::run(
            {"git", "-C", repository_path, "diff", "--cached", "--no-ext-diff",
             "--no-color", "--", change.path});
        output += "Staged changes\n" + staged.output;
    }
    if (change.is_unstaged()) {
        if (!output.empty()) {
            output += "\n";
        }
        const auto unstaged = ProcessRunner::run(
            {"git", "-C", repository_path, "diff", "--no-ext-diff", "--no-color",
             "--", change.path});
        output += "Unstaged changes\n" + unstaged.output;
    }
    return output.empty() ? "No diff available for " + change.path : output;
}

ProcessResult Repository::toggle_stage(const std::string& repository_path,
                                       const FileChange& change) {
    if (!change.is_staged() || change.is_unstaged() || change.is_untracked()) {
        return ProcessRunner::run(
            {"git", "-C", repository_path, "add", "-A", "--", change.path});
    }

    const auto head = ProcessRunner::run(
        {"git", "-C", repository_path, "rev-parse", "--verify", "HEAD"});
    if (head.exit_code == 0) {
        return ProcessRunner::run(
            {"git", "-C", repository_path, "restore", "--staged", "--", change.path});
    }
    return ProcessRunner::run(
        {"git", "-C", repository_path, "rm", "--cached", "--", change.path});
}

ProcessResult Repository::commit(const std::string& repository_path,
                                 const std::string& message) {
    if (trim(message).empty()) {
        return {1, "Commit message cannot be empty."};
    }
    return ProcessRunner::run(
        {"git", "-C", repository_path, "commit", "-m", message});
}

std::vector<BranchInfo> Repository::branches(const std::string& repository_path) {
    const auto result = ProcessRunner::run(
        {"git", "-C", repository_path, "branch", "--format=%(HEAD)%09%(refname:short)"});
    if (result.exit_code != 0) {
        throw std::runtime_error(trim(result.output));
    }

    std::vector<BranchInfo> branches;
    std::istringstream input(result.output);
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('\t');
        if (separator == std::string::npos || separator + 1 >= line.size()) {
            continue;
        }
        branches.push_back({line.substr(separator + 1), line[0] == '*'});
    }
    return branches;
}

ProcessResult Repository::create_branch(const std::string& repository_path,
                                        const std::string& name) {
    const auto validation = validate_branch(repository_path, name);
    if (validation.exit_code != 0) {
        return validation;
    }
    return ProcessRunner::run(
        {"git", "-C", repository_path, "switch", "-c", name});
}

ProcessResult Repository::switch_branch(const std::string& repository_path,
                                        const std::string& name) {
    const auto validation = validate_branch(repository_path, name);
    if (validation.exit_code != 0) {
        return validation;
    }
    return ProcessRunner::run(
        {"git", "-C", repository_path, "switch", "--", name});
}

ProcessResult Repository::delete_branch(const std::string& repository_path,
                                        const std::string& name) {
    const auto validation = validate_branch(repository_path, name);
    if (validation.exit_code != 0) {
        return validation;
    }
    return ProcessRunner::run(
        {"git", "-C", repository_path, "branch", "-d", "--", name});
}

std::string Repository::compare_branch(const std::string& repository_path,
                                       const std::string& current_branch,
                                       const std::string& other_branch) {
    const auto validation = validate_branch(repository_path, other_branch);
    if (validation.exit_code != 0) {
        return trim(validation.output);
    }
    const auto current_ref = current_branch.empty() ? "HEAD" : "refs/heads/" + current_branch;
    const auto other_ref = "refs/heads/" + other_branch;
    const auto result = ProcessRunner::run(
        {"git", "-C", repository_path, "diff", "--no-ext-diff", "--no-color",
         current_ref + "..." + other_ref});
    return result.output.empty() ? "No differences between the selected branches."
                                 : result.output;
}

std::vector<CommitInfo> Repository::history(const std::string& repository_path) {
    const auto result = ProcessRunner::run(
        {"git", "-C", repository_path, "log",
         "--format=%H%x1f%h%x1f%an%x1f%ad%x1f%s%x00", "--date=short", "-n", "100"});
    if (result.exit_code != 0) {
        throw std::runtime_error(trim(result.output));
    }

    std::vector<CommitInfo> commits;
    std::size_t offset = 0;
    while (offset < result.output.size()) {
        const auto end = result.output.find('\0', offset);
        const auto record_end = end == std::string::npos ? result.output.size() : end;
        const auto record = trim(result.output.substr(offset, record_end - offset));
        offset = end == std::string::npos ? result.output.size() : end + 1;
        if (record.empty()) {
            continue;
        }
        const auto fields = split(record, '\x1f');
        if (fields.size() == 5) {
            commits.push_back({fields[0], fields[1], fields[2], fields[3], fields[4]});
        }
    }
    return commits;
}

std::string Repository::commit_detail(const std::string& repository_path,
                                      const std::string& commit_id) {
    const auto result = ProcessRunner::run(
        {"git", "-C", repository_path, "show", "--no-ext-diff", "--no-color",
         "--format=fuller", "--stat", "--patch", commit_id});
    if (result.exit_code != 0) {
        return result.output.empty() ? "Unable to read commit details." : result.output;
    }
    return result.output.empty() ? "No commit details available." : result.output;
}