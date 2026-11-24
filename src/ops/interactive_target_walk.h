#pragma once
#include <windows.h>
#include "interactive_links.h"
#include <algorithm>
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

// `links`: null keeps any reparse point a failure. Otherwise a link met in the
// tree is resolved and its destination recorded (and walked: it can hold more
// links); cloud placeholders and other in-place reparse points walk as plain.
template<class ApiPath, class Join>
bool WalkPlainInteractiveTargets(std::vector<std::wstring>& pending, InteractiveTreeWalkBudget& budget,
                                 ApiPath api_path, Join join, std::vector<std::wstring>* links = nullptr) {
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
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (!links) return false;
            const auto api = api_path(directory);
            const auto kind = ClassifyInteractiveReparse(api);
            if (kind == InteractiveReparse::Unsafe) return false;
            if (kind == InteractiveReparse::Link) {
                auto target = ResolveInteractiveLink(api);
                if (target.empty() || links->size() >= 256) return false;
                // Seen already: a loop, or two links to one place.
                if (std::any_of(links->begin(), links->end(), [&](const std::wstring& known) {
                        return _wcsicmp(known.c_str(), target.c_str()) == 0; })) continue;
                links->push_back(target);
                pending.push_back(std::move(target));
                continue;
            }
        }
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
                if (!budget.Visit()) { known = false; break; }
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    // Looked at again on its own (above), files included: a
                    // file link leads somewhere too.
                    if (!links || ClassifyInteractiveReparseTag(data.dwReserved0) == InteractiveReparse::Unsafe) {
                        known = false; break;
                    }
                    if (ClassifyInteractiveReparseTag(data.dwReserved0) == InteractiveReparse::Link ||
                        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        pending.push_back(join(directory, data.cFileName));
                } else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    pending.push_back(join(directory, data.cFileName));
                }
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
