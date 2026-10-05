#include <windows.h>
#include <winioctl.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <iterator>
#include "../fs/fs_enum.cpp"
#include "../ui/link_type_text.h"

static std::wstring fixture;
static HRESULT WINAPI FixtureAppData(HWND, int, HANDLE, DWORD, LPWSTR path) {
    return wcscpy_s(path, MAX_PATH, fixture.c_str()) == 0 ? S_OK : E_FAIL;
}
#define SHGetFolderPathW FixtureAppData
#include "../fs/fs_net_cache.cpp"
#undef SHGetFolderPathW

static int failures;
static void Check(bool ok, const char* label) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}

static bool MakeJunction(const std::filesystem::path& link, const std::filesystem::path& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    struct MountPoint {
        DWORD tag;
        WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
        wchar_t path[2048];
    } data{};
    const std::wstring substitute = L"\\??\\" + target.wstring();
    const std::wstring display = target.wstring();
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
    data.print_length = static_cast<WORD>(display.size() * sizeof(wchar_t));
    data.length = static_cast<WORD>(8 + data.print_offset + data.print_length + sizeof(wchar_t));
    memcpy(data.path, substitute.c_str(), data.substitute_length + sizeof(wchar_t));
    memcpy(reinterpret_cast<BYTE*>(data.path) + data.print_offset, display.c_str(),
        data.print_length + sizeof(wchar_t));
    DWORD bytes = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(data.length) + 8, nullptr, 0, &bytes, nullptr) != FALSE;
    CloseHandle(handle);
    return ok;
}

int main() {
    using namespace pulse::fs;
    const auto root = std::filesystem::absolute(L"bench_data/reparse-entry-" +
        std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root / L"target");
    fixture = root.wstring();
    std::ofstream(root / L"plain.txt") << "fixture";
    Check(ClassifyLink(FILE_ATTRIBUTE_DIRECTORY, IO_REPARSE_TAG_SYMLINK) == LinkKind::None,
        "ordinary directory ignores stale tag");
    Check(ClassifyLink(FILE_ATTRIBUTE_NORMAL, 0) == LinkKind::None, "ordinary file is not link");
    Check(ClassifyLink(FILE_ATTRIBUTE_REPARSE_POINT, IO_REPARSE_TAG_SYMLINK) == LinkKind::SymbolicLink,
        "file symlink classification");
    Check(ClassifyLink(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT,
        IO_REPARSE_TAG_SYMLINK) == LinkKind::SymbolicLink, "directory symlink classification");
    Check(ClassifyLink(FILE_ATTRIBUTE_REPARSE_POINT, IO_REPARSE_TAG_CLOUD) == LinkKind::None,
        "cloud placeholder is not link");
    Check(ClassifyLink(FILE_ATTRIBUTE_REPARSE_POINT, 0) == LinkKind::None,
        "unknown reparse tag is not link");
    Check(pulse::ui::LinkTypeText(LinkKind::None).empty() &&
        !pulse::ui::LinkTypeText(LinkKind::SymbolicLink).empty() &&
        pulse::ui::LinkTypeText(LinkKind::SymbolicLink) != pulse::ui::LinkTypeText(LinkKind::Junction),
        "link presentation uses distinct type labels");
    const auto junction = root / L"junction";
    Check(MakeJunction(junction, root / L"target"), "create isolated real junction");
    Check(ReadReparseTag(junction.wstring()) == IO_REPARSE_TAG_MOUNT_POINT,
        "worker metadata lookup preserves junction tag");
    const bool directory_symlink = CreateSymbolicLinkW((root / L"dir-link").c_str(),
        (root / L"target").c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
    if (!directory_symlink) printf("[SKIP] real directory symlink creation unavailable: %lu\n", GetLastError());
    const bool file_symlink = CreateSymbolicLinkW((root / L"file-link").c_str(),
        (root / L"plain.txt").c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
    if (!file_symlink) printf("[SKIP] real file symlink creation unavailable: %lu\n", GetLastError());
    for (bool native : {true, false}) {
        std::vector<DirEntry> entries;
        try {
            if (native) EnumerateNtQuery(NormalizePath(root.wstring()), entries, {});
            else EnumerateFindFirstFileEx(NormalizePath(root.wstring()), entries, {});
            bool found = false, ordinary = false, found_dir_link = false, found_file_link = false;
            for (const auto& e : entries) {
                if (e.name == L"junction") found = e.is_dir &&
                    ClassifyLink(e.attrs, e.reparse_tag) == LinkKind::Junction;
                if (e.name == L"target") ordinary = e.is_dir && !e.is_reparse && e.reparse_tag == 0;
                if (e.name == L"dir-link") found_dir_link = e.is_dir &&
                    ClassifyLink(e.attrs, e.reparse_tag) == LinkKind::SymbolicLink;
                if (e.name == L"file-link") found_file_link = !e.is_dir &&
                    ClassifyLink(e.attrs, e.reparse_tag) == LinkKind::SymbolicLink;
            }
            Check(found && ordinary, native ? "NT enumeration distinguishes real junction" :
                "Win32 fallback distinguishes real junction");
            if (directory_symlink) Check(found_dir_link, native ? "NT directory symlink" : "Win32 directory symlink");
            if (file_symlink) Check(found_file_link, native ? "NT file symlink" : "Win32 file symlink");
        } catch (const std::exception& error) {
            printf("Enumeration error: %s\n", error.what());
            Check(false, native ? "NT enumeration" : "fallback enumeration");
        }
    }
    auto entries = std::make_shared<std::vector<DirEntry>>();
    DirEntry entry;
    entry.name = L"junction";
    entry.attrs = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    entry.is_dir = entry.is_reparse = true;
    entry.reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
    entries->push_back(entry);
    const std::wstring unc = L"\\\\pulse-reparse-fixture\\share";
    Check(SaveNetSnapshot(unc, entries), "save isolated v2 cache");
    auto loaded = LoadNetSnapshot(unc);
    Check(loaded && loaded->size() == 1 && (*loaded)[0].reparse_tag == IO_REPARSE_TAG_MOUNT_POINT,
        "v2 cache retains link kind");
    const std::wstring file = CacheFile(unc);
    std::ifstream input(file, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    input.close();
    if (bytes.size() >= 29) {
        bytes[4] = 1;
        bytes.erase(25, 4); // Version 1 has no tag between attributes and size.
        std::ofstream legacy(file, std::ios::binary | std::ios::trunc);
        legacy.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        legacy.close();
        loaded = LoadNetSnapshot(unc);
        Check(loaded && loaded->size() == 1 && (*loaded)[0].name == L"junction" &&
            (*loaded)[0].is_reparse && (*loaded)[0].reparse_tag == 0,
            "v1 cache reads without inventing link classification");
        bytes.pop_back();
        std::ofstream truncated(file, std::ios::binary | std::ios::trunc);
        truncated.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        truncated.close();
        Check(!LoadNetSnapshot(unc), "truncated cache rejected");
    } else Check(false, "cache bytes available");
    RemoveDirectoryW(junction.c_str()); // Remove the link before recursive fixture cleanup.
    if (directory_symlink) RemoveDirectoryW((root / L"dir-link").c_str());
    if (file_symlink) DeleteFileW((root / L"file-link").c_str());
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
