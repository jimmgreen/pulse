#include "delete_preflight.h"
#include "../ops/elevated_delete.h"
#include "../common/path_utils.h"
#include <thread>

namespace pulse {
namespace {
std::wstring LongPath(const std::wstring& path) {
    if (path.size() < 240 || path.starts_with(L"\\\\?\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}
}

void StartDeletePreflight(const std::shared_ptr<DeletePreflight>& request) {
    request->input_count = request->paths.size();
    request->first_path = request->paths.empty() ? std::wstring{} : request->paths.front();
    request->started_at = GetTickCount64();
    try {
        std::thread([request] {
            try {
                if (request->before_probe) request->before_probe();
                if (request->cancel.load()) { request->done.store(true); return; }
                auto& paths = request->paths;
                if (request->permanent) {
                    request->offer_recycle = paths.size() <= 256;
                    for (const auto& path : paths) {
                        if (request->cancel.load() || !request->offer_recycle) break;
                        const auto policy = ops::QueryRecyclePolicy(path);
                        request->offer_recycle = policy.available && !policy.volume_error;
                    }
                } else {
                    using Reason = ui::PermanentDeleteInfo::NoBinReason;
                    std::vector<std::wstring> direct, recyclable;
                    Reason reason = Reason::TooLarge;
                    auto& no_cancel = request->cancel;
                    std::wstring cached_parent;
                    bool cached_available = true;
                    ops::RecycleBinLimits cached_limits;
                    for (const auto& path : paths) {
                        if (no_cancel.load()) break;
                        // Items of one folder share a volume, except a mount point.
                        const DWORD attributes = GetFileAttributesW(LongPath(path).c_str());
                        const bool mount = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
                        const std::wstring parent = mount ? std::wstring{} : path.substr(0, path.find_last_of(L'\\') + 1);
                        if (parent.empty() || !pulse::path::EqualInsensitive(parent, cached_parent)) {
                            const auto policy = ops::QueryRecyclePolicy(path);
                            if (policy.volume_error) { recyclable.push_back(path); continue; }
                            cached_available = policy.available;
                            cached_limits = policy.limits;
                            cached_parent = parent;
                        }
                        Reason why;
                        if (!cached_available) why = cached_limits.disabled ? Reason::Disabled : Reason::Location;
                        else if (ops::CheckRecycleFits(path, cached_limits, no_cancel, false) == ops::kRecycleBinTooSmall)
                            why = Reason::TooLarge;
                        else { recyclable.push_back(path); continue; }
                        if (why == Reason::Location || (why == Reason::Disabled && reason == Reason::TooLarge)) reason = why;
                        direct.push_back(path);
                    }
                    paths = std::move(recyclable);
                    request->direct = std::move(direct);
                    request->reason = reason;
                }
            } catch (...) { request->failed = true; }
            request->done.store(true);
            // Posted completion is not starved by continuous paint traffic like
            // a synthesized WM_TIMER; the UI still validates the current request.
            if (request->notify_window) PostMessageW(request->notify_window, WM_TIMER, 0xD311, 0);
        }).detach();
    } catch (...) { request->failed = true; request->done.store(true); }
}
}
