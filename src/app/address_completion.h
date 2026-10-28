#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../fs/fs_enum.h"

namespace pulse {
struct AddressCompletionQuery {
    std::wstring parent, prefix;
};
inline bool ParseAddressCompletion(std::wstring text, AddressCompletionQuery& query, const std::wstring& base = {}) {
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    text.erase(0, first);
    if (text.front() == L'"') {
        text.erase(0, 1);
        const auto quote = text.find(L'"');
        if (quote != std::wstring::npos) text.resize(quote);
    }
    for (auto& c : text) if (c == L'/') c = L'\\';
    if (!text.empty() && text.front() != L'\\' && text.find(L':') == std::wstring::npos &&
        text.front() != L'%' && !base.empty() && !fs::IsVirtualPath(base)) {
        text = base + (base.back() == L'\\' ? L"" : L"\\") + text;
    }
    const bool unc = text.compare(0, 2, L"\\\\") == 0;
    const bool drive = text.size() >= 3 && iswalpha(text[0]) && text[1] == L':' && text[2] == L'\\';
    const auto cut = text.find_last_of(L'\\');
    if ((!unc && !drive) || cut == std::wstring::npos || (unc && cut <= 2)) return false;
    query = {text.substr(0, cut + 1), text.substr(cut + 1)};
    return query.prefix.find_first_of(L"*?\"") == std::wstring::npos;
}
struct AddressCompletionEntry { std::wstring path; bool directory; };
struct AddressCompletionResult {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    bool ready = false;
    std::vector<AddressCompletionEntry> paths;
};
// One process-lifetime worker: a stalled network query cannot create more threads.
// The sole pending request is replaced by the newest input; workers own no windows.
class AddressCompletionWorker {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        AddressCompletionQuery query;
        std::shared_ptr<AddressCompletionResult> pending;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
public:
    AddressCompletionWorker() {
        std::thread([state = state_] {
            for (;;) {
                std::unique_lock guard(state->mutex);
                state->changed.wait(guard, [&] { return !!state->pending; });
                auto result = std::move(state->pending);
                auto query = std::move(state->query);
                guard.unlock();
                if (result->cancelled) continue;
                DWORD previous = 0;
                const bool changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous) != FALSE;
                WIN32_FIND_DATAW data{};
                HANDLE find = FindFirstFileW(fs::NormalizePath(query.parent + L"*").c_str(), &data);
                std::vector<AddressCompletionEntry> paths;
                if (find != INVALID_HANDLE_VALUE) {
                    do {
                        if (result->cancelled) break;
                        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
                        if (_wcsnicmp(data.cFileName, query.prefix.c_str(), query.prefix.size()) == 0)
                            paths.push_back({query.parent + data.cFileName, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
                        if (paths.size() >= 32) break;
                    } while (FindNextFileW(find, &data));
                    FindClose(find);
                }
                if (changed) SetThreadErrorMode(previous, nullptr);
                std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
                if (paths.size() > 6) paths.resize(6);
                std::lock_guard lock(result->mutex);
                if (!result->cancelled) {
                    result->paths = std::move(paths);
                    result->ready = true;
                }
            }
        }).detach();
    }
    void Submit(AddressCompletionQuery query, const std::shared_ptr<AddressCompletionResult>& result) {
        std::lock_guard lock(state_->mutex);
        if (state_->pending) state_->pending->cancelled = true;
        state_->query = std::move(query);
        state_->pending = result;
        state_->changed.notify_one();
    }
};
}
