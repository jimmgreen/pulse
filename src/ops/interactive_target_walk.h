#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::ops {

// Time is a validity/cancellation checkpoint, not evidence of a path alias.
// One scheduler admission owns the DFS stack and active FindNext handle for
// its entire walk. No filesystem call is made under the manager mutex.
struct InteractiveTreeWalkBudget {
    const std::atomic<bool>& stopping;
    std::function<bool()> still_current;
    ULONGLONG deadline;
    ULONGLONG slice_ms = 250;
    size_t visited = 0;
    size_t continuations = 0;
    bool interrupted = false;

    bool Visit() {
        if (stopping.load(std::memory_order_relaxed)) { interrupted = true; return false; }
        if (GetTickCount64() >= deadline) {
            if (!still_current()) { interrupted = true; return false; }
            ++continuations;
            deadline = GetTickCount64() + slice_ms;
        }
        return ++visited <= 65536; // unchanged bound for unknown/oversized trees
    }
};

template<class ApiPath, class Join>
bool WalkPlainInteractiveTargets(std::vector<std::wstring>& pending, InteractiveTreeWalkBudget& budget,
                                 ApiPath api_path, Join join) {
    while (!pending.empty()) {
        if (!budget.Visit()) return false;
        auto directory = std::move(pending.back());
        pending.pop_back();
        const auto attrs = GetFileAttributesW(api_path(directory).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
            return false;
        }
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return false;
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW(join(api_path(directory), L"*").c_str(), FindExInfoBasic,
            &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) continue;
            return false;
        }
        bool known = true;
        for (;;) {
            const std::wstring_view name(data.cFileName);
            if (name != L"." && name != L"..") {
                if (!budget.Visit() ||
                    (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { known = false; break; }
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    pending.push_back(join(directory, data.cFileName));
            }
            if (!FindNextFileW(find, &data)) {
                known = GetLastError() == ERROR_NO_MORE_FILES;
                break;
            }
        }
        FindClose(find);
        if (!known) return false;
    }
    return true;
}
} // namespace pulse::ops
