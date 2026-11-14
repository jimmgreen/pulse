#include "setup_common.h"
#include <sddl.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>

namespace test {
unsigned checks = 0, failures = 0;
std::function<void(const std::wstring&, const WIN32_FIND_DATAW&)> enumerated;
std::function<void(const std::wstring&)> before_enumeration;
std::vector<std::wstring> deferred;
bool native_api_unavailable = false;
bool identity_unavailable = false;
DWORD enumeration_failure = ERROR_SUCCESS;

void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
}

void Require(bool ok, const char* label) {
    if (!ok) throw std::runtime_error(std::string(label) + ": " + std::to_string(GetLastError()));
}

FARPROC WINAPI Lookup(HMODULE module, LPCSTR name) {
    if (native_api_unavailable && std::strcmp(name, "NtCreateFile") == 0) return nullptr;
    return GetProcAddress(module, name);
}

BOOL WINAPI Identity(HANDLE file, LPBY_HANDLE_FILE_INFORMATION info) {
    const BOOL result = GetFileInformationByHandle(file, info);
    if (result && identity_unavailable) info->nFileIndexHigh = info->nFileIndexLow = 0;
    return result;
}

BOOL WINAPI Information(HANDLE file, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID buffer, DWORD size) {
    if (kind != FileIdBothDirectoryInfo) return GetFileInformationByHandleEx(file, kind, buffer, size);
    if (enumeration_failure) { SetLastError(enumeration_failure); return FALSE; }
    wchar_t full[32768]{};
    const DWORD length = GetFinalPathNameByHandleW(file, full, static_cast<DWORD>(std::size(full)), FILE_NAME_OPENED);
    Require(length && length < std::size(full), "identify fixture enumeration handle");
    std::wstring directory(full, length);
    if (directory.starts_with(L"\\\\?\\")) directory.erase(0, 4);
    if (before_enumeration) before_enumeration(directory);
    const BOOL result = GetFileInformationByHandleEx(file, kind, buffer, size);
    const DWORD error = GetLastError();
    if (result && enumerated) {
        const auto* item = static_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer);
        for (;;) {
            WIN32_FIND_DATAW data{};
            data.dwFileAttributes = item->FileAttributes;
            const size_t count = item->FileNameLength / sizeof(wchar_t);
            Require(count < std::size(data.cFileName), "fixture filename fits enumeration event");
            std::copy_n(item->FileName, count, data.cFileName);
            enumerated(directory, data);
            if (!item->NextEntryOffset) break;
            item = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(reinterpret_cast<const BYTE*>(item) + item->NextEntryOffset);
        }
    }
    SetLastError(error);
    return result;
}

BOOL WINAPI Defer(LPCWSTR path, LPCWSTR target, DWORD flags) {
    Require(!target && flags == MOVEFILE_DELAY_UNTIL_REBOOT, "only intercept reboot deletion");
    deferred.emplace_back(path);
    return TRUE;
}
} // namespace test

// Run the real cleanup against the filesystem. Only the enumeration boundary
// is instrumented for deterministic races, and reboot registry writes are kept
// local to this test process.
#define GetProcAddress test::Lookup
#define GetFileInformationByHandle test::Identity
#define GetFileInformationByHandleEx test::Information
#define MoveFileExW test::Defer
#include "setup_common.cpp"
#undef GetProcAddress
#undef GetFileInformationByHandle
#undef GetFileInformationByHandleEx
#undef MoveFileExW

namespace test {
bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string Read(const std::wstring& path) {
    std::string text;
    pulse::setup::ReadWholeFile(path, text);
    return text;
}

bool SetJunction(const std::wstring& path, const std::wstring& target, DWORD access = GENERIC_WRITE) {
    HANDLE handle = CreateFileW(path.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    struct MountPoint {
        DWORD tag;
        WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
        wchar_t path[2048];
    } data{};
    const std::wstring name = L"\\??\\" + target;
    Require(name.size() + 2 < std::size(data.path), "junction target fits fixture buffer");
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(name.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
    data.length = static_cast<WORD>(8 + data.print_offset + sizeof(wchar_t));
    std::memcpy(data.path, name.c_str(), data.substitute_length + sizeof(wchar_t));
    DWORD bytes = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(data.length) + 8, nullptr, 0, &bytes, nullptr) != FALSE;
    CloseHandle(handle);
    return ok;
}

struct Fixture {
    std::wstring root;
    std::vector<std::wstring> files, directories;

    Fixture() {
        wchar_t temp[32768]{}, full[32768]{};
        const DWORD size = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
        Require(size && size < std::size(temp), "resolve temporary directory");
        const DWORD length = GetFullPathNameW(temp, static_cast<DWORD>(std::size(full)), full, nullptr);
        Require(length && length < std::size(full), "resolve absolute temporary directory");
        std::wstring parent(full, length);
        Require(parent.size() > 3 && parent[1] == L':' && parent[2] == L'\\', "absolute local fixture parent");
        if (parent.back() != L'\\') parent += L'\\';
        static unsigned serial = 0;
        const auto candidate = parent + L"pulse-setup-cleanup-test-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++serial);
        const DWORD resolved = GetFullPathNameW(candidate.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
        Require(resolved && resolved < std::size(full) && candidate == full && candidate.starts_with(parent),
            "fixture stays inside resolved absolute temporary directory");
        Require(CreateDirectoryW(candidate.c_str(), nullptr) != FALSE, "create exclusive fixture");
        root = candidate;
        directories.push_back(root);
        deferred.clear();
    }

    bool Owns(const std::wstring& path) const {
        return path == root || (path.starts_with(root + L"\\") && path.find(L"..") == std::wstring::npos);
    }

    std::wstring Path(const wchar_t* relative) const {
        const auto path = root + L"\\" + relative;
        Require(Owns(path) && std::wstring(relative).find(L':') == std::wstring::npos,
            "fixture operation stays within owned absolute root");
        return path;
    }

    std::wstring Directory(const wchar_t* relative) {
        auto path = Path(relative);
        Require(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "create fixture directory");
        directories.push_back(path);
        return path;
    }

    std::wstring File(const wchar_t* relative, const char* content = "outside sentinel") {
        auto path = Path(relative);
        Require(pulse::setup::WriteWholeFile(path, content, std::strlen(content)), "write fixture file");
        files.push_back(path);
        return path;
    }

    std::wstring Junction(const wchar_t* relative, const std::wstring& target) {
        Require(Owns(target), "junction target stays inside fixture");
        const auto link = Directory(relative);
        Require(SetJunction(link, target), "create fixture junction without elevation");
        return link;
    }

    ~Fixture() {
        enumerated = {};
        before_enumeration = {};
        native_api_unavailable = false;
        identity_unavailable = false;
        enumeration_failure = ERROR_SUCCESS;
        // No recursive cleanup: remove only explicit fixture entries. A failed
        // regression must never make the test's own cleanup follow a junction.
        for (const auto& path : files) if (Owns(path)) {
            HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                FILE_BASIC_INFO info{}; info.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                SetFileInformationByHandle(file, FileBasicInfo, &info, sizeof(info));
                CloseHandle(file);
            }
            DeleteFileW(path.c_str());
        }
        for (auto it = directories.rbegin(); it != directories.rend(); ++it)
            if (Owns(*it)) RemoveDirectoryW(it->c_str());
    }
};

void RootJunction() {
    Fixture f;
    const auto outside = f.Directory(L"outside");
    const auto sentinel = f.File(L"outside\\sentinel.txt");
    const auto link = f.Junction(L"cleanup", outside);
    pulse::setup::RemoveTree(link + L"\\");
    Check(Read(sentinel) == "outside sentinel", "root junction preserves external target contents");
    Check(Exists(outside) && !Exists(link), "root junction removes only the link");
    Check(deferred.empty(), "unlocked root junction needs no reboot cleanup");
}

void ChildJunction() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    f.Directory(L"cleanup\\ordinary");
    f.File(L"cleanup\\ordinary\\local.txt", "owned");
    const auto outside = f.Directory(L"outside");
    const auto sentinel = f.File(L"outside\\sentinel.txt");
    f.Junction(L"cleanup\\linked", outside);
    pulse::setup::RemoveTree(cleanup);
    Check(Read(sentinel) == "outside sentinel", "child junction preserves external target contents");
    Check(Exists(outside) && !Exists(cleanup), "child junction and ordinary owned tree are removed");
}

void BrokenJunction() {
    Fixture f;
    const auto link = f.Junction(L"cleanup", f.Path(L"missing-target"));
    pulse::setup::RemoveTree(link);
    Check(!Exists(link) && deferred.empty(), "broken root junction is removed without opening its target");
}

void IntermediateJunction() {
    Fixture f;
    const auto outside = f.Directory(L"outside");
    f.Directory(L"outside\\tagicons");
    const auto sentinel = f.File(L"outside\\tagicons\\sentinel.txt");
    const auto link = f.Junction(L"redirect", outside);
    pulse::setup::RemoveTree(link + L"\\tagicons");
    Check(Read(sentinel) == "outside sentinel" && Exists(link) && deferred.empty(),
        "reparse ancestor cannot redirect a nested cleanup root or reboot deletion");
}

void KeepPacks() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    f.Directory(L"cleanup\\Packs");
    const auto kept = f.File(L"cleanup\\Packs\\keep.dat", "retained pack");
    const auto ordinary = f.File(L"cleanup\\settings.json", "owned");
    const auto outside = f.Directory(L"outside");
    const auto sentinel = f.File(L"outside\\sentinel.txt");
    const auto link = f.Junction(L"cleanup\\linked-cache", outside);
    pulse::setup::RemoveTree(cleanup, L"packs");
    Check(Read(kept) == "retained pack" && Exists(cleanup), "keep-packs cleanup retains named child case-insensitively and its root");
    Check(!Exists(ordinary) && !Exists(link) && Read(sentinel) == "outside sentinel" && deferred.empty(),
        "keep-packs cleanup removes ordinary data and junction itself without following target");
}

void KeepPacksRootJunction() {
    Fixture f;
    const auto outside = f.Directory(L"outside");
    const auto sentinel = f.File(L"outside\\settings.json");
    const auto link = f.Junction(L"cleanup", outside);
    pulse::setup::RemoveTree(link, L"packs");
    Check(Read(sentinel) == "outside sentinel" && !Exists(link) && deferred.empty(),
        "keep-packs cleanup of a root junction removes only the link");
}

void KeepPacksBlockedRoot() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    f.Directory(L"cleanup\\packs");
    const auto kept = f.File(L"cleanup\\packs\\keep.dat", "retained pack");
    HANDLE writer = CreateFileW(cleanup.c_str(), FILE_ADD_FILE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Require(writer != INVALID_HANDLE_VALUE, "hold fixture directory writer");
    pulse::setup::RemoveTree(cleanup, L"packs");
    CloseHandle(writer);
    Check(Read(kept) == "retained pack" && deferred.empty(),
        "blocked keep-packs root is retained without scheduling its deletion");
}

void OrdinaryTree() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    f.Directory(L"cleanup\\nested");
    const auto readonly_file = f.File(L"cleanup\\nested\\readonly.txt", "owned");
    Require(SetFileAttributesW(readonly_file.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "set fixture readonly bit");
    f.Directory(L"cleanup\\empty");
    pulse::setup::RemoveTree(cleanup);
    Check(!Exists(cleanup) && deferred.empty(), "ordinary nested tree including readonly file is removed immediately");
    pulse::setup::RemoveTree(cleanup);
    Check(deferred.empty(), "missing root is an idempotent no-op");
}

void PaginatedDirectory() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    for (unsigned i = 0; i != 400; ++i) {
        const auto relative = L"cleanup\\owned-" + std::to_wstring(i) + L".txt";
        f.File(relative.c_str(), "owned");
    }
    pulse::setup::RemoveTree(cleanup);
    Check(!Exists(cleanup) && deferred.empty(), "handle enumeration cleans multiple result buffers completely");
}

void NativeApiUnavailable() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    const auto file = f.File(L"cleanup\\owned.txt", "owned");
    native_api_unavailable = true;
    pulse::setup::RemoveTree(cleanup);
    native_api_unavailable = false;
    Check(Read(file) == "owned" && deferred.empty(), "missing relative-open API retains tree without path deletion fallback");
}

void EnumerationUnavailable() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    const auto file = f.File(L"cleanup\\owned.txt", "owned");
    enumeration_failure = ERROR_NOT_SUPPORTED;
    pulse::setup::RemoveTree(cleanup);
    enumeration_failure = ERROR_SUCCESS;
    Check(Read(file) == "owned" && deferred.empty(), "unsupported handle enumeration retains tree without reboot fallback");
}

void IdentityUnavailable() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    const auto file = f.File(L"cleanup\\owned.txt", "owned");
    identity_unavailable = true;
    pulse::setup::RemoveTree(cleanup);
    identity_unavailable = false;
    Check(Read(file) == "owned" && deferred.empty(), "unverifiable object identity retains tree without deletion fallback");
}

void ReplacedChild() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    const auto child = f.Directory(L"cleanup\\child");
    const auto outside = f.Directory(L"outside");
    const auto sentinel = f.File(L"outside\\sentinel.txt");
    bool replaced = false;
    enumerated = [&](const std::wstring& parent, const WIN32_FIND_DATAW& data) {
        if (parent != cleanup || std::wstring(data.cFileName) != L"child" || replaced) return;
        Require((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0, "enumeration observed ordinary child");
        replaced = SetJunction(child, outside);
        Require(replaced, "replace enumerated child with a junction");
    };
    pulse::setup::RemoveTree(cleanup);
    enumerated = {};
    Check(replaced && Read(sentinel) == "outside sentinel", "child changed after enumeration cannot redirect deletion");
    Check(!Exists(cleanup), "replacement junction itself is still cleaned");
}

void ReplacedRoot() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    f.File(L"cleanup\\sentinel.txt", "owned");
    const auto moved = f.Path(L"moved");
    f.directories.push_back(moved);
    f.files.push_back(f.Path(L"moved\\sentinel.txt"));
    const auto outside = f.Directory(L"outside");
    const auto sentinel = f.File(L"outside\\sentinel.txt");
    bool attempted = false, renamed = false;
    before_enumeration = [&](const std::wstring& parent) {
        if (parent != cleanup || attempted) return;
        attempted = true;
        renamed = MoveFileExW(cleanup.c_str(), moved.c_str(), 0) != FALSE;
        if (renamed) {
            Require(CreateDirectoryW(cleanup.c_str(), nullptr) != FALSE && SetJunction(cleanup, outside),
                "replace root after enumeration begins");
        }
    };
    pulse::setup::RemoveTree(cleanup);
    before_enumeration = {};
    Check(attempted && !renamed, "ordinary root is pinned against rename during traversal");
    Check(Read(sentinel) == "outside sentinel", "root replacement attempt preserves external target");
}

void ReparsedRoot() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    const auto outside = f.Directory(L"outside");
    const auto sentinel = f.File(L"outside\\sentinel.txt");
    bool attempted = false, changed = false;
    before_enumeration = [&](const std::wstring& parent) {
        if (parent != cleanup || attempted) return;
        attempted = true;
        const DWORD accesses[] = {GENERIC_WRITE, FILE_WRITE_ATTRIBUTES, 0};
        for (DWORD access : accesses)
            changed = SetJunction(cleanup, outside, access) || changed;
    };
    pulse::setup::RemoveTree(cleanup);
    before_enumeration = {};
    Check(attempted && changed && Read(sentinel) == "outside sentinel",
        "root converted to a junction before enumeration cannot redirect deletion");
}

void LockedFile() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    const auto file = f.File(L"cleanup\\locked.txt", "locked owned file");
    Require(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "set locked fixture readonly bit");
    HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "hold fixture without delete sharing");
    pulse::setup::RemoveTree(cleanup);
    Check(Exists(file) && deferred == std::vector<std::wstring>{file, cleanup},
        "locked file and remaining directory retain ordered reboot cleanup");
    Check((GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_READONLY) == 0,
        "locked readonly file is made deletable before reboot cleanup");
    CloseHandle(lock);
    deferred.clear();
    pulse::setup::RemoveTree(cleanup);
    Check(!Exists(cleanup) && deferred.empty(), "released lock allows immediate cleanup");
}

void LockedWritingFile() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    const auto file = f.File(L"cleanup\\active.log", "active owned log");
    HANDLE writer = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Require(writer != INVALID_HANDLE_VALUE, "hold fixture log writer without delete sharing");
    pulse::setup::RemoveTree(cleanup);
    CloseHandle(writer);
    Check(Exists(file) && deferred == std::vector<std::wstring>{file, cleanup},
        "active writable file retains ordered reboot cleanup without requiring delete sharing");
}

void NoAttributeWritePermission() {
    Fixture f;
    const auto cleanup = f.Directory(L"cleanup");
    const auto file = f.File(L"cleanup\\deletable.txt", "owned");
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x101;;;WD)(A;;FA;;;WD)",
        SDDL_REVISION_1, &descriptor, nullptr) != FALSE, "create fixture DACL");
    const bool changed = SetFileSecurityW(file.c_str(), DACL_SECURITY_INFORMATION, descriptor) != FALSE;
    LocalFree(descriptor);
    Require(changed, "deny fixture file data reads and attribute writes");
    pulse::setup::RemoveTree(cleanup);
    Check(!Exists(cleanup) && deferred.empty(), "deletion does not require file data read or attribute write permission");
}
} // namespace test

int main() {
    try {
        test::RootJunction();
        test::ChildJunction();
        test::BrokenJunction();
        test::IntermediateJunction();
        test::KeepPacks();
        test::KeepPacksRootJunction();
        test::KeepPacksBlockedRoot();
        test::OrdinaryTree();
        test::PaginatedDirectory();
        test::NativeApiUnavailable();
        test::EnumerationUnavailable();
        test::IdentityUnavailable();
        test::ReplacedChild();
        test::ReplacedRoot();
        test::ReparsedRoot();
        test::LockedFile();
        test::LockedWritingFile();
        test::NoAttributeWritePermission();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Fixture failed: %s\n", error.what());
        return 2;
    }
    std::printf("%u checks, %u failures\n", test::checks, test::failures);
    return test::failures ? 1 : 0;
}
