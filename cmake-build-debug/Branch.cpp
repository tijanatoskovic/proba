#include "Branch.hpp"

#include <iostream>
#include <stdexcept>

git::Branch::Branch(git_reference* branch, Repository* repo) : _branch(branch), _repo(repo) {
    git_object* commit = nullptr;
    if (git_reference_peel(&commit, _branch, GIT_OBJECT_COMMIT) != 0) {
        throw std::runtime_error("Failed to peel the reference to commit object.");
    }

    _lastCommit = Commit::create(reinterpret_cast<git_commit*>(commit), repo);
}

git::Branch::Branch(const Branch& other)
    : _branch(nullptr), _repo(other._repo), _lastCommit(other._lastCommit) {
    if (git_reference_dup(&_branch, other._branch) != 0) {
        throw std::runtime_error("Failed to duplicate git_reference");
    }
}

git::Branch::Branch(Branch&& other) noexcept
    : _branch(other._branch), _repo(other._repo), _lastCommit(other._lastCommit) {}

git::Branch::Branch(const Branch& other, Repository* repo)
    : _branch(other._branch), _repo(repo), _lastCommit(other._lastCommit) {}

git::Branch::Branch(Branch&& other, Repository* repo)
    : _branch(other._branch), _repo(repo), _lastCommit(other._lastCommit) {}

git::Branch::~Branch() {
    git_reference_free(_branch);
}

git::Branch& git::Branch::operator=(const Branch& other) {
    if (this == &other) {
        return *this;
    }

    Branch temp(other);
    std::swap(_branch, temp._branch);
    std::swap(_repo, temp._repo);
    std::swap(_lastCommit, temp._lastCommit);

    return *this;
}

git::Branch& git::Branch::operator=(Branch&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    Branch temp(std::move(other));
    std::swap(_branch, temp._branch);
    std::swap(_repo, temp._repo);
    std::swap(_lastCommit, temp._lastCommit);

    return *this;
}

std::vector<std::unique_ptr<git::Branch>> git::Branch::getAllBranches(Repository* repo) {
    std::vector<std::unique_ptr<Branch>> branches;

    git_branch_iterator* iterator = nullptr;
    if (git_branch_iterator_new(&iterator, repo->_repo, GIT_BRANCH_ALL) != 0) {
        throw std::runtime_error("Failed to create branch iterator.");
    }

    git_reference* branchRef = nullptr;
    git_branch_t branchType  = GIT_BRANCH_LOCAL;

    while (git_branch_next(&branchRef, &branchType, iterator) == 0) {
        branches.push_back(create(branchRef, repo));
    }

    git_branch_iterator_free(iterator);

    return branches;
}

git::Commit* git::Branch::getLastCommit() const {
    return _lastCommit;
}

std::unique_ptr<git::Branch> git::Branch::create(git_reference* branch, Repository* repo) {
    return std::unique_ptr<Branch>(new Branch(branch, repo));
}

git::Repository* git::Branch::getRepository() const {
    return _repo;
}

void git::Branch::checkout(const git::Branch* targetBranch) {
    if (!targetBranch) {
        throw std::invalid_argument("Target branch is null.");
    }

    git_reference* branchRef = targetBranch->_branch;
    if (!branchRef) {
        throw std::invalid_argument("Target branch reference is null.");
    }

    // azuriranje HEAD na novu granu
    int error =
        git_repository_set_head(this->getRepository()->_repo, git_reference_name(branchRef));
    if (error != 0) {
        throw std::runtime_error("Could not update HEAD to target branch: " +
                                 std::string(git_error_last()->message));
    }

    git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
    opts.checkout_strategy    = GIT_CHECKOUT_SAFE;

    // prebacivanje radnog direktorijuma
    error = git_checkout_head(this->getRepository()->_repo, &opts);
    if (error != 0) {
        throw std::runtime_error("Checkout failed: " + std::string(git_error_last()->message));
    }
}
std::string git::Branch::getBranchName() const {
    const char* branch_name = nullptr;

    if (git_branch_name(&branch_name, _branch) != 0) {
        throw std::runtime_error("Failed to get branch name: " +
                                 std::string(git_error_last()->message));
    }

    return std::string(branch_name);
}

void git::Branch::executeMerge(Branch* targetBranch) {
    if (!targetBranch) {
        throw std::invalid_argument("Target branch is null.");
    }
    if (performFastforward(targetBranch)) {
        return;
    }

    executeMergeCommit();

    auto conflicts = getConflictingFiles();
    if (!conflicts.empty()) {
        throw std::runtime_error("Merge completed with conflicts. Files in conflict: " +
                                 std::to_string(conflicts.size()));
    }
}
bool git::Branch::performFastforward(Branch* targetBranch) {
    if (!targetBranch) {
        throw std::invalid_argument("Target branch is null.");
    }

    git_commit* targetCommit = targetBranch->getLastCommit()->_commit;
    git_repository* repo = this->getRepository()->_repo;

    if (!targetCommit) {
        throw std::runtime_error("Target branch last commit is null.");
    }

    git_annotated_commit* annotatedTarget = nullptr;
    if (git_annotated_commit_lookup(&annotatedTarget, repo, git_commit_id(targetCommit)) != 0) {
        throw std::runtime_error("Failed to lookup annotated commit for fast-forward.");
    }

    git_merge_analysis_t analysis = GIT_MERGE_ANALYSIS_NONE;
    git_merge_preference_t preference = GIT_MERGE_PREFERENCE_NONE;

    if (git_merge_analysis(&analysis, &preference, repo,
                           const_cast<const git_annotated_commit**>(&annotatedTarget), 1) != 0) {
        git_annotated_commit_free(annotatedTarget);
        throw std::runtime_error("Merge analysis failed: " +
                                 std::string(git_error_last()->message));
    }

    if (analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) {
        if (git_repository_set_head(repo, git_reference_name(_branch)) != 0) {
            git_annotated_commit_free(annotatedTarget);
            throw std::runtime_error("Fast-forward failed: " +
                                     std::string(git_error_last()->message));
        }

        git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
        opts.checkout_strategy = GIT_CHECKOUT_SAFE;
        if (git_checkout_head(repo, &opts) != 0) {
            git_annotated_commit_free(annotatedTarget);
            throw std::runtime_error("Fast-forward checkout failed: " +
                                     std::string(git_error_last()->message));
        }

        git_annotated_commit_free(annotatedTarget);
        return true;
    }

    git_annotated_commit_free(annotatedTarget);
    return false;
}


void git::Branch::executeMergeCommit() {
    git_repository* repo = this->getRepository()->_repo;

    git_index* index = nullptr;
    if (git_merge(repo, nullptr, 0, nullptr, nullptr) != 0) {
        throw std::runtime_error("Failed to prepare merge: " +
                                 std::string(git_error_last()->message));
    }

    if (git_repository_index(&index, repo) != 0) {
        throw std::runtime_error("Failed to get repository index: " +
                                 std::string(git_error_last()->message));
    }

    if (git_index_has_conflicts(index)) {
        git_index_free(index);
        throw std::runtime_error("Merge conflicts detected.");
    }

    const char* message = "Merged branch via libgit2";
    git_oid tree_oid, commit_oid;
    git_tree* tree = nullptr;

    if (git_index_write_tree(&tree_oid, index) != 0) {
        git_index_free(index);
        throw std::runtime_error("Failed to write tree: " +
                                 std::string(git_error_last()->message));
    }

    if (git_tree_lookup(&tree, repo, &tree_oid) != 0) {
        git_index_free(index);
        throw std::runtime_error("Failed to lookup tree: " +
                                 std::string(git_error_last()->message));
    }

    git_reference* headRef = nullptr;
    git_commit* parent = nullptr;

    if (git_repository_head(&headRef, repo) != 0 ||
        git_commit_lookup(&parent, repo, git_reference_target(headRef)) != 0) {
        git_tree_free(tree);
        git_index_free(index);
        throw std::runtime_error("Failed to get HEAD commit.");
    }

    if (git_commit_create_v(
            &commit_oid, repo, "HEAD", nullptr, nullptr, nullptr, message, tree, 1, parent) != 0) {
        git_tree_free(tree);
        git_index_free(index);
        git_reference_free(headRef);
        git_commit_free(parent);
        throw std::runtime_error("Failed to create merge commit: " +
                                 std::string(git_error_last()->message));
    }

    git_tree_free(tree);
    git_index_free(index);
    git_reference_free(headRef);
    git_commit_free(parent);
}

std::vector<std::string> git::Branch::getConflictingFiles() const {
    std::vector<std::string> conflictingFiles;
    git_index* index = nullptr;

    if (git_repository_index(&index, this->_repo->_repo) != 0) {
        throw std::runtime_error("Failed to get repository index: " +
                                 std::string(git_error_last()->message));
    }

    git_index_conflict_iterator* conflictIter = nullptr;
    if (git_index_conflict_iterator_new(&conflictIter, index) != 0) {
        git_index_free(index);
        throw std::runtime_error("Failed to create conflict iterator: " +
                                 std::string(git_error_last()->message));
    }

    const git_index_entry* ancestor = nullptr;
    const git_index_entry* ours = nullptr;
    const git_index_entry* theirs = nullptr;

    while (git_index_conflict_next(&ancestor, &ours, &theirs, conflictIter) == 0) {
        if (ours) {
            conflictingFiles.push_back(ours->path);
        }
    }

    git_index_conflict_iterator_free(conflictIter);
    git_index_free(index);

    return conflictingFiles;
}

