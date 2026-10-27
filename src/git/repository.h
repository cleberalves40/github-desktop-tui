#pragma once

#include "process/process_runner.h"

#include <string>
#include <vector>

struct FileChange {
    std::string path;
    char index_status = ' ';
    char worktree_status = ' ';

    bool is_untracked() const;
    bool is_staged() const;
    bool is_unstaged() const;
    bool is_conflicted() const;
};

struct BranchInfo {
    std::string name;
    bool is_current = false;
};

struct CommitInfo {
    std::string id;
    std::string short_id;
    std::string author;
    std::string date;
    std::string subject;
};

struct RepositorySnapshot {
    bool is_repository = false;
    std::string path;
    std::string branch;
    std::string status;
    std::string error;
    std::vector<FileChange> changes;
    bool has_conflicts = false;
};

class Repository {
public:
    static RepositorySnapshot inspect(const std::string& path);
    static std::string diff(const std::string& repository_path,
                            const FileChange& change);
    static ProcessResult toggle_stage(const std::string& repository_path,
                                      const FileChange& change);
    static ProcessResult commit(const std::string& repository_path,
                                const std::string& message);
    static ProcessResult fetch(const std::string& repository_path);
    static ProcessResult pull(const std::string& repository_path);
    static ProcessResult push(const std::string& repository_path);
    static std::vector<BranchInfo> branches(const std::string& repository_path);
    static ProcessResult create_branch(const std::string& repository_path,
                                       const std::string& name);
    static ProcessResult switch_branch(const std::string& repository_path,
                                       const std::string& name);
    static ProcessResult delete_branch(const std::string& repository_path,
                                       const std::string& name);
    static std::string compare_branch(const std::string& repository_path,
                                      const std::string& current_branch,
                                      const std::string& other_branch);
    static std::vector<CommitInfo> history(const std::string& repository_path);
    static std::string commit_detail(const std::string& repository_path,
                                     const std::string& commit_id);
};