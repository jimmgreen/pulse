#include "dialog_restart_inventory.h"
#include <tlhelp32.h>
#include <algorithm>
#include <filesystem>
#include <set>
namespace pulse::dialog_restart {
namespace {
namespace fs = std::filesystem;
std::wstring Normal(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    path = fs::path(path).lexically_normal().wstring();
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    for (auto& c : path) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return path;
}
bool Digest(const std::wstring& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    });
}
bool Directory(const std::wstring& root, const std::wstring& directory) {
    const fs::path path(directory);
    return path.is_absolute() && Digest(path.filename().wstring()) &&
        path.parent_path().wstring() == root + L"\\dialogbridge";
}
bool Known(const std::wstring& name, unsigned arch) {
    if (name == L"pulsepickeruiproof.dll") return true;
    if (arch == 64) return name == L"pulsedialogauto64.dll" || name == L"pulsefiledialogruntime64.dll" || name == L"pulsefiledialogruntimeuiproof64r8.dll";
    if (arch == 32) return name == L"pulsedialogauto32.dll" || name == L"pulsefiledialogruntime32.dll" || name == L"pulsefiledialogruntimeuiproof32r8.dll";
    return false;
}
bool DisplayName(const std::wstring& value) {
    return !value.empty() && value.size() <= 255 && std::none_of(value.begin(), value.end(), [](wchar_t c) {
        return c < 32 || (c >= 0x7f && c <= 0x9f) || c == 0x061c || c == 0x200e || c == 0x200f ||
            (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069);
    });
}
uint64_t Fingerprint(const std::wstring& path) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto c : path) { hash ^= static_cast<uint64_t>(c); hash *= 1099511628211ull; }
    return hash;
}
}
CurrentComponents CaptureCurrent(const std::wstring& root,
    const dialog_bundle::BundleLease* x64, const dialog_bundle::BundleLease* x86) {
    CurrentComponents result; result.install_root = Normal(root);
    auto capture = [&](const dialog_bundle::BundleLease* lease, unsigned machine, std::wstring& directory, DWORD& error) {
        if (!lease || !*lease) return;
        const auto path = Normal(lease->Directory());
        if (lease->Machine() != machine || !Directory(result.install_root, path)) { error = ERROR_INVALID_DATA; return; }
        directory = path; error = ERROR_SUCCESS;
    };
    capture(x64, IMAGE_FILE_MACHINE_AMD64, result.x64_directory, result.x64_error);
    capture(x86, IMAGE_FILE_MACHINE_I386, result.x86_directory, result.x86_error);
    return result;
}
Inventory Classify(const CurrentComponents& current,
    const std::vector<dialog_diagnostics::RestartProcessSnapshot>& processes) {
    Inventory out; const auto root = Normal(current.install_root);
    if (!fs::path(root).is_absolute()) { out.scan_complete = false; ++out.query_errors; return out; }
    std::vector<std::wstring> images;
    std::set<std::pair<DWORD, uint64_t>> seen;
    for (const auto& item : processes) {
        ++out.scanned_processes; const auto& p = item.process;
        if (item.scope_error || p.process_query_error) { out.scan_complete = false; ++out.query_errors; continue; }
        if (!item.same_user_session) continue;
        if (!p.identity_verified || !p.creation_time || !*p.creation_time || !p.pid ||
            p.module_status != dialog_diagnostics::ModuleStatus::Complete || p.module_error ||
            (p.process_arch != 32 && p.process_arch != 64) || item.image_path.empty()) {
            out.scan_complete = false; ++out.query_errors; continue;
        }
        if (!seen.emplace(p.pid, *p.creation_time).second) continue;
        const auto target = Normal(p.process_arch == 64 ? current.x64_directory : current.x86_directory);
        const auto error = p.process_arch == 64 ? current.x64_error : current.x86_error;
        if (error || !Directory(root, target)) { out.scan_complete = false; ++out.query_errors; continue; }
        std::set<std::wstring> old;
        for (const auto& module : item.module_paths) {
            const fs::path path(Normal(module));
            const auto parent = path.parent_path().wstring();
            if (Known(path.filename().wstring(), p.process_arch) && Directory(root, parent) && parent != target)
                old.insert(path.wstring());
        }
        if (old.empty()) continue;
        const auto image = Normal(item.image_path); const auto basename = fs::path(item.image_path).filename().wstring();
        if (!fs::path(image).is_absolute() || !DisplayName(basename)) { out.scan_complete = false; ++out.query_errors; continue; }
        auto it = std::find(images.begin(), images.end(), image);
        size_t index = static_cast<size_t>(it - images.begin());
        if (it == images.end()) { images.push_back(image); Record r; r.app_basename = basename; r.app_image_id = Fingerprint(image); out.records.push_back(std::move(r)); }
        auto& record = out.records[index]; record.processes.push_back({p.pid, *p.creation_time});
        record.old_component_count += static_cast<uint32_t>(old.size());
    }
    return out;
}
std::vector<DWORD> ProcessOrder(std::vector<DWORD> pids, DWORD start_after_pid) {
    std::sort(pids.begin(), pids.end());
    pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    const auto begin = std::upper_bound(pids.begin(), pids.end(), start_after_pid);
    std::rotate(pids.begin(), begin, pids.end());
    return pids;
}
Inventory Scan(const CurrentComponents& current, const std::function<bool()>& cancelled, DWORD start_after_pid) {
    Inventory result;
    try {
        if (cancelled && cancelled()) { result.cancelled = true; result.scan_complete = false; return result; }
        const auto root = Normal(current.install_root);
        if ((current.x64_error || !Directory(root, Normal(current.x64_directory))) &&
            (current.x86_error || !Directory(root, Normal(current.x86_directory)))) {
            result.scan_complete = false; result.query_errors = 1; return result;
        }
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) { result.scan_complete = false; result.query_errors = 1; return result; }
        struct Close { HANDLE h; ~Close() { CloseHandle(h); } } close{snapshot};
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        BOOL more = Process32FirstW(snapshot, &entry); bool limited = false;
        DWORD enumeration_error = more ? ERROR_SUCCESS : GetLastError();
        std::vector<DWORD> pids;
        while (more) {
            if (cancelled && cancelled()) { result.cancelled = true; result.scan_complete = false; return result; }
            if (pids.size() >= 65536) { limited = true; break; }
            if (entry.th32ProcessID && entry.th32ProcessID != 4) pids.push_back(entry.th32ProcessID);
            entry.dwSize = sizeof(entry); more = Process32NextW(snapshot, &entry);
            if (!more) enumeration_error = GetLastError();
        }
        pids = ProcessOrder(std::move(pids), start_after_pid);
        const auto start = GetTickCount64(); DWORD last_pid = start_after_pid;
        std::vector<dialog_diagnostics::RestartProcessSnapshot> processes;
        for (const auto pid : pids) {
            if (cancelled && cancelled()) { result.cancelled = true; result.scan_complete = false; return result; }
            if (processes.size() >= 512 || GetTickCount64() - start >= 1500) { limited = true; break; }
            processes.push_back(dialog_diagnostics::CaptureRestartProcess(pid)); last_pid = pid;
        }
        if (cancelled && cancelled()) { result.cancelled = true; result.scan_complete = false; return result; }
        result = Classify(current, processes);
        if (limited || (enumeration_error && enumeration_error != ERROR_NO_MORE_FILES)) result.scan_complete = false;
        if (limited) result.next_pid = last_pid;
        if (enumeration_error && enumeration_error != ERROR_NO_MORE_FILES) ++result.query_errors;
    } catch (...) { result = {}; result.scan_complete = false; result.query_errors = 1; }
    return result;
}
}
