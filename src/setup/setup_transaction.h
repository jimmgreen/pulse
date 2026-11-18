#pragma once
// Replaces installed files as one unit. New files are extracted to
// <app>\.pulse-setup\staging first; Commit() then moves each old file to
// <app>\.pulse-setup\backup and the new one into place, journaling every step
// so a failure — or a crash/power loss on the next run — restores the
// previous installation.

#include <string>
#include <functional>
#include <vector>

namespace pulse::setup {

class FileTransaction {
public:
    using Recovery = std::function<bool(std::wstring&)>;
    explicit FileTransaction(std::wstring app_dir, Recovery recovery = {});

    const std::wstring& StagingDir() const { return staging_; }
    // Prepares an empty staging directory; rolls back an interrupted commit first.
    bool Begin(std::wstring& error);
    // `files` are paths relative to the staging/app directory.
    bool Commit(const std::vector<std::wstring>& files, std::wstring& error,
                const std::vector<std::wstring>& remove = {}, Recovery before_commit = {});
    // Removes backups and the work directory after a successful commit.
    void Finish();
    // Drops staged files without touching the installation.
    void Abort();

    // Restores the installation from an interrupted journal, if any.
    static bool RecoverInterrupted(const std::wstring& app_dir, std::wstring& error, Recovery recovery = {});

private:
    static bool RecoverJournal(const std::wstring& app_dir, std::wstring& error, Recovery recovery, bool interrupted);
    bool Rollback(size_t done, std::wstring& error);
    Recovery recovery_;
    std::wstring app_, work_, staging_, backup_, journal_;
    struct Step { std::wstring rel; bool had_old; };
    std::vector<Step> steps_;
};

}  // namespace pulse::setup
