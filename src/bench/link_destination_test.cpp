#include <windows.h>
#include <winioctl.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "../app/link_resolve.h"
#include "../app/snapshot_patch.h"

static int failures;
static void Check(bool ok, const char* label) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}

static bool MakeShortcut(const std::filesystem::path& path, const std::filesystem::path& target) {
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&link))) || FAILED(link->SetPath(target.c_str()))) return false;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    return SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(path.c_str(), TRUE));
}

static bool MakeJunction(const std::filesystem::path& path, const std::filesystem::path& target) {
    if (!CreateDirectoryW(path.c_str(), nullptr)) return false;
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    struct MountPoint {
        DWORD tag;
        WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
        wchar_t path[2048];
    } data{};
    const std::wstring target_name = L"\\??\\" + target.wstring();
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(target_name.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
    data.length = static_cast<WORD>(8 + data.print_offset + sizeof(wchar_t));
    memcpy(data.path, target_name.c_str(), data.substitute_length + sizeof(wchar_t));
    DWORD bytes = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(data.length) + 8, nullptr, 0, &bytes, nullptr) != FALSE;
    CloseHandle(handle);
    return ok;
}

static void TestNotifyRefresh(const std::filesystem::path& root,
    const std::vector<pulse::fs::DirEntry>& initial, bool junction, bool symlink) {
    using namespace pulse;
    const auto event = [](DWORD action, const wchar_t* name, const wchar_t* old_name = L"") {
        fs::DirNotifyEvent result;
        result.action = action;
        result.name = name;
        result.old_name = old_name;
        return result;
    };
    const auto intact = [&](const std::vector<fs::DirEntry>& entries) {
        if (entries.size() != initial.size()) return false;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].name != initial[i].name || entries[i].link_destination != initial[i].link_destination ||
                entries[i].link_target != initial[i].link_target || entries[i].size != initial[i].size)
                return false;
        }
        return true;
    };
    std::vector<const wchar_t*> links{L"site.URL", L"live.lnk"};
    if (junction) links.push_back(L"junction");
    if (symlink) links.push_back(L"relative-link");
    for (const auto* name : links) {
        for (const DWORD action : {FILE_ACTION_ADDED, FILE_ACTION_MODIFIED}) {
            auto entries = initial;
            Check(app::ApplyDirNotify(entries, root.wstring(), event(action, name),
                ui::SortColumn::Name, ui::SortDirection::Asc) == app::NotifyPatch::NeedFullEnum && intact(entries),
                "single link notification requests worker refresh without changing snapshot");
            for (const size_t count : {size_t{1}, size_t{2}, size_t{6}}) {
                entries = initial;
                std::vector<fs::DirNotifyEvent> events(count, event(FILE_ACTION_REMOVED, L"plain.txt"));
                events.back() = event(action, name);
                Check(app::ApplyDirNotifyBatch(entries, root.wstring(), events, ui::SortColumn::Name,
                    ui::SortDirection::Asc) == app::NotifyPatch::NeedFullEnum && intact(entries),
                    "small and large link batches preserve original entries before refresh");
            }
            entries = initial;
            entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) {
                return entry.name == name;
            }), entries.end());
            Check(app::ApplyDirNotifyBatch(entries, root.wstring(), {event(action, name)},
                ui::SortColumn::Name, ui::SortDirection::Asc) == app::NotifyPatch::NeedFullEnum,
                "new link is detected from fresh file metadata");
        }
    }
    Check(MoveFileW((root / L"site.URL").c_str(), (root / L"renamed.txt").c_str()) != FALSE,
        "rename real URL to ordinary extension");
    auto entries = initial;
    const auto renamed = event(FILE_ACTION_RENAMED_NEW_NAME, L"renamed.txt", L"site.URL");
    Check(app::ApplyDirNotify(entries, root.wstring(), renamed, ui::SortColumn::Name,
        ui::SortDirection::Asc) == app::NotifyPatch::NeedFullEnum && intact(entries),
        "single rename from link extension requests refresh");
    std::vector<fs::DirNotifyEvent> renames(6, event(FILE_ACTION_REMOVED, L"plain.txt"));
    renames.back() = renamed;
    Check(app::ApplyDirNotifyBatch(entries, root.wstring(), renames, ui::SortColumn::Name,
        ui::SortDirection::Asc) == app::NotifyPatch::NeedFullEnum && intact(entries),
        "batch rename from link extension preserves entire snapshot");
    Check(MoveFileW((root / L"renamed.txt").c_str(), (root / L"site.URL").c_str()) != FALSE,
        "restore URL fixture name");
    std::ofstream(root / L"new.txt") << "ordinary addition";
    for (bool batch : {false, true}) {
        entries = initial;
        for (const auto& ordinary : {event(FILE_ACTION_ADDED, L"new.txt"),
                event(FILE_ACTION_MODIFIED, L"plain.txt"), event(FILE_ACTION_REMOVED, L"live.lnk")}) {
            const auto result = batch ? app::ApplyDirNotifyBatch(entries, root.wstring(), {ordinary},
                ui::SortColumn::Name, ui::SortDirection::Asc) : app::ApplyDirNotify(entries, root.wstring(), ordinary,
                ui::SortColumn::Name, ui::SortDirection::Asc);
            Check(result == app::NotifyPatch::Applied, "ordinary changes and link removal stay incremental");
        }
    }
    Check(MoveFileW((root / L"new.txt").c_str(), (root / L"renamed.txt").c_str()) != FALSE,
        "rename ordinary fixture");
    for (bool batch : {false, true}) {
        entries = initial;
        const auto ordinary = event(FILE_ACTION_RENAMED_NEW_NAME, L"renamed.txt", L"new.txt");
        const auto result = batch ? app::ApplyDirNotifyBatch(entries, root.wstring(), {ordinary},
            ui::SortColumn::Name, ui::SortDirection::Asc) : app::ApplyDirNotify(entries, root.wstring(), ordinary,
            ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(result == app::NotifyPatch::Applied, "ordinary rename stays incremental");
    }
}

static void TestEnumerationProgress(const std::filesystem::path& root) {
    using namespace pulse;
    const auto folder = root / L"progress";
    std::filesystem::create_directories(folder);
    constexpr int kFiles = 5000;
    for (int i = 0; i < kFiles; ++i)
        std::ofstream(folder / (L"p" + std::to_wstring(i) + L".tmp")) << "x";

    size_t calls = 0;
    size_t last_count = 0;
    bool increasing = true;
    std::vector<fs::DirEntry> entries;
    const bool completed = fs::EnumerateDirectory(folder.wstring(), entries,
        [&](const std::vector<fs::DirEntry>& gathered) {
            ++calls;
            increasing = increasing && gathered.size() >= last_count;
            last_count = gathered.size();
            return true;
        });
    Check(completed && entries.size() == kFiles,
        "enumeration progress: every file enumerated");
    Check(calls >= 2 && increasing && last_count == entries.size(),
        "enumeration progress: batches grow with the listing");

    // Returning false stops the scan; the call reports it instead of failing.
    size_t stop_calls = 0;
    std::vector<fs::DirEntry> stopped;
    const bool stopped_ok = fs::EnumerateDirectory(folder.wstring(), stopped,
        [&](const std::vector<fs::DirEntry>&) {
            ++stop_calls;
            return false;
        });
    Check(!stopped_ok && stop_calls == 1 && !stopped.empty() && stopped.size() < entries.size(),
        "enumeration progress: a stop request ends the scan and reports it");
}

int main() {
    using namespace pulse;
    const auto root = std::filesystem::absolute(L"bench_data/link-destination-" +
        std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root / L"target");
    std::ofstream(root / L"plain.txt") << "fixture";
    std::ofstream(root / L"site.URL") << "[InternetShortcut]\r\nURL=https://example.invalid/a?q=1#part\r\n";
    std::ofstream(root / L"bad.url") << "[InternetShortcut]\r\nURL=not a destination\r\n";
    std::ofstream(root / L"bad.lnk") << "not a shortcut";
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(com), "initialize fixture COM");
    Check(MakeShortcut(root / L"live.lnk", root / L"plain.txt"), "create actual shortcut");
    Check(MakeShortcut(root / L"dead.lnk", root / L"missing.txt"), "create broken shortcut");
    const bool junction = MakeJunction(root / L"junction", root / L"target");
    Check(junction, "create real junction without print label");
    const bool symlink = CreateSymbolicLinkW((root / L"relative-link").c_str(), L"missing.txt",
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
    if (!symlink) printf("[SKIP] symlink creation unavailable: %lu\n", GetLastError());
    const bool cycle = symlink && CreateSymbolicLinkW((root / L"cycle").c_str(), L"cycle",
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
    if (symlink) Check(cycle, "create self-referencing symbolic link");
    std::vector<fs::DirEntry> entries;
    fs::EnumerateDirectory(root.wstring(), entries);
    app::ResolveLinksInPlace(root.wstring(), entries, {});
    auto find = [&](const wchar_t* name) -> const fs::DirEntry& {
        for (const auto& entry : entries) if (entry.name == name) return entry;
        static const fs::DirEntry missing;
        Check(false, "expected fixture enumerated");
        return missing;
    };
    Check(!find(L"live.lnk").link_destination.empty() && !find(L"live.lnk").link_target.empty(),
        "live shortcut has display destination and penetration metadata");
    Check(find(L"dead.lnk").link_destination.ends_with(L"missing.txt") && find(L"dead.lnk").link_target.empty(),
        "broken shortcut retains saved destination without penetration");
    Check(find(L"site.URL").link_destination == L"https://example.invalid/a?q=1#part" &&
        find(L"site.URL").link_target.empty(), "URL read locally without penetration");
    Check(find(L"bad.url").link_destination.empty() && find(L"bad.lnk").link_destination.empty(),
        "invalid shortcut data produces no destination");
    Check(find(L"plain.txt").link_destination.empty(), "ordinary file has no destination");
    if (junction) Check(find(L"junction").link_destination == (root / L"target").wstring() &&
        find(L"junction").link_target.empty(), "junction reads substitute target without changing identity");
    if (symlink) Check(find(L"relative-link").link_destination == L"missing.txt" &&
        find(L"relative-link").link_target.empty(), "broken relative symlink reads one immediate hop");
    if (cycle) Check(find(L"cycle").link_destination == L"cycle", "cyclic symlink is not followed");
    TestNotifyRefresh(root, entries, junction, symlink);
    if (junction) {
        RemoveDirectoryW((root / L"target").c_str());
        app::ResolveLinksInPlace(root.wstring(), entries, {});
        Check(find(L"junction").link_destination == (root / L"target").wstring(),
            "broken junction retains its immediate destination");
    }
    auto cancelled = entries;
    for (auto& entry : cancelled) entry.link_destination.clear();
    app::ResolveLinksInPlace(root.wstring(), cancelled, [] { return true; });
    bool untouched = true;
    for (const auto& entry : cancelled) untouched = untouched && entry.link_destination.empty();
    Check(untouched, "cancelled generation does not resolve entries");
    fs::DirEntry virtual_url;
    virtual_url.name = L"site.URL";
    virtual_url.full_path = (root / L"site.URL").wstring();
    std::vector<fs::DirEntry> virtual_entries{virtual_url};
    app::ResolveLinksInPlace(L"pulse:search:", virtual_entries, {});
    Check(virtual_entries[0].link_destination == find(L"site.URL").link_destination,
        "virtual listing uses its explicit full path");
    DeleteFileW((root / L"site.URL").c_str());
    app::ResolveLinksInPlace(L"pulse:search:", virtual_entries, {});
    Check(virtual_entries[0].link_destination.empty(), "deleted URL clears old destination");
    fs::DirEntry reused;
    Check(app::ResolveLink((root / L"live.lnk").wstring(), reused) && reused.link_target_size != 0,
        "existing shortcut populates reusable entry");
    DeleteFileW((root / L"plain.txt").c_str());
    reused.link_target_is_dir = true; // Verify every penetration field resets on failure.
    Check(!app::ResolveLink((root / L"live.lnk").wstring(), reused) && reused.link_target.empty() &&
        reused.link_target_size == 0 && !reused.link_target_is_dir &&
        reused.link_target_mtime.dwLowDateTime == 0 && reused.link_target_mtime.dwHighDateTime == 0 &&
        reused.link_destination.ends_with(L"plain.txt"),
        "deleted target clears prior penetration metadata while retaining saved destination");
    Check(!app::ResolveLink((root / L"bad.lnk").wstring(), reused) && reused.link_destination.empty(),
        "unreadable shortcut clears prior display destination");
    TestEnumerationProgress(root);
    if (junction) RemoveDirectoryW((root / L"junction").c_str());
    if (symlink) DeleteFileW((root / L"relative-link").c_str());
    if (cycle) DeleteFileW((root / L"cycle").c_str());
    std::filesystem::remove_all(root);
    if (SUCCEEDED(com)) CoUninitialize();
    return failures ? 1 : 0;
}
