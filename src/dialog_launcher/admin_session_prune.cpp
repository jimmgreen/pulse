#include "admin_session_prune.h"
#include "../shell_bridge/runtime_config.h"
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <vector>

namespace pulse::dialog_launcher::admin_auto {
namespace {
namespace fs = std::filesystem;
namespace cfg = pulse::shell_bridge::runtime;
constexpr size_t kMaxSessions = 8192;
constexpr size_t kMaxMembers = 32;

uint64_t Ticks(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
bool GuidName(const std::wstring& name) {
    if (name.size() != 38 || name.front() != L'{' || name.back() != L'}') return false;
    for (size_t i = 1; i < 37; ++i) {
        const bool dash = i == 9 || i == 14 || i == 19 || i == 24;
        if (dash ? name[i] != L'-' : !std::iswxdigit(name[i])) return false;
    }
    return true;
}
struct Members {
    std::vector<std::wstring> files;
    uint64_t newest = 0;
};
// Only a flat directory of plain files is eligible; anything else is retained.
bool ListMembers(const fs::path& directory, Members& members) {
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileExW((directory / L"*").c_str(), FindExInfoBasic, &found,
                                     FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        const std::wstring_view name = found.cFileName;
        if (name == L"." || name == L"..") continue;
        if ((found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            members.files.size() >= kMaxMembers) { ok = false; break; }
        members.files.emplace_back(name);
        members.newest = (std::max)(members.newest, Ticks(found.ftLastWriteTime));
    } while (FindNextFileW(search, &found));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES) ok = false;
    FindClose(search);
    return ok;
}
bool ReadSession(const fs::path& path, cfg::Session& session) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    DWORD read = 0;
    const bool ok = GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && GetFileSizeEx(file, &size) &&
        size.QuadPart == sizeof(session) &&
        ReadFile(file, &session, sizeof(session), &read, nullptr) && read == sizeof(session);
    CloseHandle(file);
    return ok;
}
enum class Target { Running, Exited, Unknown };
// A process created before this boot cannot still run. Used only when its PID
// cannot be opened (reused by an inaccessible process), never to override a match.
bool CreatedBeforeBoot(const FILETIME& created, uint64_t now) {
    constexpr uint64_t slack = 60ull * 10000000;
    const uint64_t uptime = GetTickCount64() * 10000;
    return now > uptime + slack && Ticks(created) < now - uptime - slack;
}
Target TargetState(const cfg::Session& session, uint64_t now) {
    // Limited query only: it is granted across integrity levels, unlike SYNCHRONIZE.
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, session.target_pid);
    if (!process) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) return Target::Exited;
        return CreatedBeforeBoot(session.target_created, now) ? Target::Exited : Target::Unknown;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    DWORD code = 0;
    Target state = Target::Unknown;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        // A reused PID has a different creation time: the bound target is gone.
        // An exited target still referenced by a handle keeps its PID and times.
        if (CompareFileTime(&created, &session.target_created) != 0) state = Target::Exited;
        else if (GetExitCodeProcess(process, &code)) state = code == STILL_ACTIVE ? Target::Running : Target::Exited;
    }
    CloseHandle(process);
    return state;
}
}

SessionPruneReport PruneExitedSessions(const std::wstring& control_root, ULONGLONG min_idle_ms) noexcept {
    SessionPruneReport report;
    try {
        const fs::path root(control_root);
        std::vector<std::wstring> candidates;
        WIN32_FIND_DATAW found{};
        HANDLE search = FindFirstFileExW((root / L"*").c_str(), FindExInfoBasic, &found,
                                         FindExSearchNameMatch, nullptr, 0);
        if (search == INVALID_HANDLE_VALUE) return report;
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !GuidName(found.cFileName)) continue;
            if (candidates.size() >= kMaxSessions) { report.limited = true; break; }
            candidates.emplace_back(found.cFileName);
        } while (FindNextFileW(search, &found));
        FindClose(search);

        FILETIME now_time{};
        GetSystemTimeAsFileTime(&now_time);
        const uint64_t now = Ticks(now_time), idle = static_cast<uint64_t>(min_idle_ms) * 10000;
        auto session = std::make_unique<cfg::Session>();
        for (const auto& name : candidates) {
            const fs::path directory = root / name;
            Members members;
            if (!ListMembers(directory, members) || !ReadSession(directory / cfg::kSessionFile, *session) ||
                session->magic != cfg::kConfigMagic || session->version != cfg::kVersion ||
                session->size != sizeof(cfg::Session) ||
                _wcsnicmp(session->nonce, name.c_str(), std::size(session->nonce)) != 0) {
                ++report.retained;
                continue;
            }
            const Target state = TargetState(*session, now);
            if (state != Target::Exited) { ++report.live; continue; }
            if (members.newest + idle > now) { ++report.recent; continue; }
            // session.bin goes last so a partly removed directory is still
            // recognised, and finished, on a later pass.
            bool removed = true;
            for (const auto& file : members.files) {
                if (_wcsicmp(file.c_str(), cfg::kSessionFile) == 0) continue;
                if (!DeleteFileW((directory / file).c_str())) { removed = false; break; }
            }
            if (removed) removed = DeleteFileW((directory / cfg::kSessionFile).c_str()) &&
                                   RemoveDirectoryW(directory.c_str());
            ++(removed ? report.removed : report.retained);
        }
    } catch (...) {
        report.limited = true;
    }
    return report;
}
}
