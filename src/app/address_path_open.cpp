#include "address_path_open.h"
#include "app_internal.h"

namespace pulse {
void OpenAddressPath(AppState& s, const std::wstring& path) {
    if (path.empty() || fs::IsVirtualPath(path)) { NavigateTo(s, path); return; }
    auto* tab = ActiveTab(s);
    if (!tab) return;
    std::wstring target = path;
    if (target.find(L':') == std::wstring::npos && !target.starts_with(L"\\\\")) {
        if (!tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path)) {
            if (target.front() == L'\\' || target.front() == L'/') {
                const auto plain = ClipboardPath(tab->current_path);
                if (plain.size() >= 2 && plain[1] == L':') target = plain.substr(0, 2) + target;
            } else target = tab->current_path + L"\\" + target;
        }
    }
    target = fs::NormalizePath(target);
    if (target.empty()) { NavigateTo(s, path); return; }
    if (!s.addressPathOpen) s.addressPathOpen = std::make_shared<AddressPathOpen>();
    const auto job = s.addressPathOpen;
    job->tab = tab;
    job->view_generation = tab->view_generation;
    job->origin = tab->current_path;
    bool launch = false;
    {
        std::lock_guard lock(job->mutex);
        job->pending = AddressPathOpen::Result{target, INVALID_FILE_ATTRIBUTES, ++job->generation};
        job->result.reset();
        launch = !job->running;
        job->running = true;
    }
    if (launch) {
        try {
            std::thread([job] {
                DWORD old_mode = 0;
                const BOOL changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode);
                for (;;) {
                    AddressPathOpen::Result request;
                    {
                        std::lock_guard lock(job->mutex);
                        if (!job->pending) { job->running = false; break; }
                        request = std::move(*job->pending);
                        job->pending.reset();
                    }
                    request.attrs = GetFileAttributesW(request.path.c_str());
                    std::lock_guard lock(job->mutex);
                    if (request.generation == job->generation) job->result = std::move(request);
                }
                if (changed) SetThreadErrorMode(old_mode, nullptr);
            }).detach();
        } catch (...) {
            std::lock_guard lock(job->mutex);
            job->running = false;
            job->result = std::move(job->pending);
        }
    }
    SetTimer(s.hwnd, kAddressPathOpenTimer, 30, nullptr);
}

void PollAddressPathOpen(AppState& s) {
    const auto job = s.addressPathOpen;
    if (!job) { KillTimer(s.hwnd, kAddressPathOpenTimer); return; }
    const auto* tab = ActiveTab(s);
    if (tab != job->tab || !tab || tab->view_generation != job->view_generation ||
        tab->current_path != job->origin || s.addressEditing) {
        KillTimer(s.hwnd, kAddressPathOpenTimer);
        std::lock_guard lock(job->mutex);
        ++job->generation;
        job->pending.reset();
        job->result.reset();
        return;
    }
    std::optional<AddressPathOpen::Result> result;
    {
        std::lock_guard lock(job->mutex);
        result = std::move(job->result);
        job->result.reset();
    }
    if (!result) return;
    KillTimer(s.hwnd, kAddressPathOpenTimer);
    if (result->attrs != INVALID_FILE_ATTRIBUTES && !(result->attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        s.ops.OpenWith(result->path);
        RecordRecentOpen(s, result->path, app::PlaceItemKind::File);
    } else {
        // The normal navigation error UI handles nonexistent/inaccessible paths.
        NavigateTo(s, result->path);
    }
}
}
