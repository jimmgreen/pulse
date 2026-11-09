#include "setup_transaction.h"

#include "setup_common.h"
#include "setup_log.h"

#include <windows.h>


namespace pulse::setup {
namespace {

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

void EnsureParent(const std::wstring& path) { CreateDirs(ParentDir(path)); }

// Journal: UTF-8 lines "<0|1>\t<relative path>", written and flushed before
// each move. had_old=1 means the previous file is (or will be) in backup.
bool AppendJournal(HANDLE journal, bool had_old, const std::wstring& rel) {
    const std::wstring line = std::wstring(had_old ? L"1\t" : L"0\t") + rel + L"\n";
    const int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, utf8.data(), n, nullptr, nullptr);
    utf8.resize(static_cast<size_t>(n - 1));
    DWORD written = 0;
    return WriteFile(journal, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) &&
           written == utf8.size() && FlushFileBuffers(journal);
}

// Undo of one journaled step; safe whether or not the step completed.
bool UndoStep(const std::wstring& app, const std::wstring& backup, bool had_old, const std::wstring& rel) {
    const std::wstring dst = app + L"\\" + rel, old = backup + L"\\" + rel;
    if (had_old) {
        if (Exists(old) && !MoveFileExW(old.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING))
            return false;
    } else if (Exists(dst) && !DeleteFileW(dst.c_str())) {
        return false;
    }
    return true;
}

}  // namespace

FileTransaction::FileTransaction(std::wstring app_dir, Recovery recovery)
    : recovery_(std::move(recovery)), app_(NormalizeDir(std::move(app_dir))),
      work_(app_ + L"\\.pulse-setup"),
      staging_(work_ + L"\\staging"),
      backup_(work_ + L"\\backup"),
      journal_(work_ + L"\\journal.txt") {}

bool FileTransaction::RecoverInterrupted(const std::wstring& app_dir, std::wstring& error, Recovery recovery) {
    return RecoverJournal(app_dir, error, std::move(recovery), true);
}

bool FileTransaction::RecoverJournal(const std::wstring& app_dir, std::wstring& error, Recovery recovery, bool interrupted) {
    FileTransaction t(app_dir);
    std::string journal;
    const DWORD attributes = GetFileAttributesW(t.journal_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD missing = GetLastError();
        if (missing == ERROR_FILE_NOT_FOUND || missing == ERROR_PATH_NOT_FOUND) return true;
        error = L"Cannot inspect update journal: " + Win32Error(missing); return false;
    }
    if (!ReadWholeFile(t.journal_, journal)) {
        error = L"Cannot read update journal: " + Win32Error(GetLastError()); return false;
    }
    Log(interrupted ? L"Found an interrupted update; restoring the previous files"
                    : L"Aborting current update; restoring the previous files");
    std::vector<Step> steps;
    for (size_t pos = 0; pos < journal.size();) {
        size_t end = journal.find('\n', pos);
        if (end == std::string::npos) end = journal.size();  // torn last line: its move never started
        const std::string line = journal.substr(pos, end - pos);
        pos = end + 1;
        if (line.size() < 3 || line[1] != '\t' || end == journal.size()) continue;
        steps.push_back({Utf8ToWide(line.substr(2)), line[0] == '1'});
    }
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) if (!UndoStep(t.app_, t.backup_, it->had_old, it->rel)) {
        error = L"Cannot restore interrupted update: " + Win32Error(GetLastError()); return false;
    }
    if (recovery && !recovery(error)) return false;
    if (!DeleteFileW(t.journal_.c_str())) {
        error = L"Cannot clear interrupted update journal: " + Win32Error(GetLastError());
        return false;
    }
    RemoveTree(t.work_);
    return true;
}

bool FileTransaction::Begin(std::wstring& error) {
    if (!RecoverInterrupted(app_, error, recovery_)) return false;
    RemoveTree(work_);
    if (!CreateDirs(staging_)) {
        error = L"Cannot create " + staging_ + L": " + Win32Error(GetLastError());
        return false;
    }
    SetFileAttributesW(work_.c_str(), FILE_ATTRIBUTE_HIDDEN);
    // Preparation can stop services or capture registration state before any
    // file replacement. An empty durable journal distinguishes that state from
    // a committed transaction whose backups have not yet been cleaned up.
    HANDLE journal = CreateFileW(journal_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (journal == INVALID_HANDLE_VALUE) { error = L"Cannot begin update journal: " + Win32Error(GetLastError()); return false; }
    const bool flushed = FlushFileBuffers(journal) != FALSE;
    const DWORD failure = GetLastError(); CloseHandle(journal);
    if (!flushed) { error = L"Cannot flush initial update journal: " + Win32Error(failure); return false; }
    return true;
}

bool FileTransaction::Commit(const std::vector<std::wstring>& files, std::wstring& error, const std::vector<std::wstring>& remove, Recovery before_commit) {
    HANDLE journal = CreateFileW(journal_.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (journal == INVALID_HANDLE_VALUE) {
        error = L"Cannot create update journal: " + Win32Error(GetLastError());
        return false;
    }
    LARGE_INTEGER journal_size{};
    if (!GetFileSizeEx(journal, &journal_size) || journal_size.QuadPart != 0) {
        CloseHandle(journal); error = L"Update journal requires recovery before committing."; return false;
    }
    steps_.clear();
    bool ok = true;
    auto changes = files;
    changes.insert(changes.end(), remove.begin(), remove.end());
    size_t index = 0;
    for (const auto& rel : changes) {
        const bool removing = index++ >= files.size();
        const std::wstring src = staging_ + L"\\" + rel, dst = app_ + L"\\" + rel, old = backup_ + L"\\" + rel;
        const bool had_old = Exists(dst);
        if (!AppendJournal(journal, had_old, rel)) {
            error = L"Cannot write update journal: " + Win32Error(GetLastError());
            ok = false;
            break;
        }
        steps_.push_back({rel, had_old});
        if (had_old) {
            EnsureParent(old);
            SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
            // Renaming works even for a running exe; a file opened without
            // FILE_SHARE_DELETE fails here and the whole commit is undone.
            if (!MoveFileExW(dst.c_str(), old.c_str(), 0)) {
                error = L"Cannot replace " + dst + L": " + Win32Error(GetLastError());
                ok = false;
                break;
            }
        }
        if (removing) continue;
        EnsureParent(dst);
        if (!MoveFileExW(src.c_str(), dst.c_str(), 0)) {
            error = L"Cannot install " + dst + L": " + Win32Error(GetLastError());
            ok = false;
            break;
        }
    }
    CloseHandle(journal);
    if (ok && before_commit) ok = before_commit(error);
    if (!ok) {
        Log(L"Commit failed, rolling back: " + error);
        std::wstring rollback_error;
        if (!Rollback(steps_.size(), rollback_error)) error += L"\nRollback pending: " + rollback_error;
        return false;
    }
    // The journal's removal is the commit point.
    if (!DeleteFileW(journal_.c_str())) {
        error = L"Cannot finalize update journal: " + Win32Error(GetLastError());
        std::wstring rollback_error;
        if (!Rollback(steps_.size(), rollback_error)) error += L"\nRollback pending: " + rollback_error;
        return false;
    }
    return true;
}

bool FileTransaction::Rollback(size_t done, std::wstring& error) {
    for (size_t i = done; i-- > 0;) if (!UndoStep(app_, backup_, steps_[i].had_old, steps_[i].rel)) {
        error = L"Cannot restore " + steps_[i].rel + L": " + Win32Error(GetLastError()); return false;
    }
    if (recovery_ && !recovery_(error)) return false;
    if (!DeleteFileW(journal_.c_str())) { error = L"Cannot clear rollback journal: " + Win32Error(GetLastError()); return false; }
    RemoveTree(work_);
    return true;
}

void FileTransaction::Finish() { RemoveTree(work_); }
void FileTransaction::Abort() {
    std::wstring error;
    if (!RecoverJournal(app_, error, recovery_, false)) { Log(L"Preparation rollback remains pending: " + error); return; }
    RemoveTree(work_);
}

}  // namespace pulse::setup
