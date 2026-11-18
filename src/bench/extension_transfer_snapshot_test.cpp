#include "../app/extensions/transfer_snapshot.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <winioctl.h>
#include <cstring>
using namespace pulse::extensions;
namespace {
int failures = 0;
void Check(bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; }
bool Junction(const std::filesystem::path& path, const std::filesystem::path& target) {
    if (!CreateDirectoryW(path.c_str(), nullptr)) return false;
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    struct Data { DWORD tag; WORD length, reserved, offset, size, print_offset, print_size; wchar_t path[2048]; } data{};
    const auto name = L"\\??\\" + target.wstring();
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.size = static_cast<WORD>(name.size() * sizeof(wchar_t));
    data.print_offset = data.size + sizeof(wchar_t);
    data.length = static_cast<WORD>(8 + data.print_offset + sizeof(wchar_t));
    std::memcpy(data.path, name.c_str(), data.size + sizeof(wchar_t));
    DWORD bytes = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, data.length + 8, nullptr, 0, &bytes, nullptr) != FALSE;
    CloseHandle(handle);
    return ok;
}
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
    namespace fs = std::filesystem;
    using namespace transfer_snapshot;
    const auto root = fs::absolute(fs::path("bench_data") / ("transfer-snapshot-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    std::atomic<bool> cancelled{false};
    fs::create_directories(root / "source"); fs::create_directory(root / "target");
    const auto file = root / "source" / "example.txt";
    std::ofstream(file) << "first";
    auto source = CaptureSource(file.wstring(), cancelled);
    auto target = CaptureTarget((root / "target").wstring(), cancelled);
    Check(source && target, "local file and existing directory capture real held handles");
    if (!source || !target) { std::printf("source error=%lu target error=%lu\n", source.error, target.error); return 1; }
    const auto original = source.value->snapshot;
    const auto destination = target.value->snapshot;
    Check(original.identity.size == 5 && original.parent.volume == original.identity.volume, "source captures size and real parent identity");
    Check(static_cast<bool>(ReopenSource(original, cancelled)), "same source identity verifies through reopened handle");
    Check(static_cast<bool>(ReopenTarget(destination, cancelled)), "same target identity verifies through reopened handle");
    HANDLE write = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(write == INVALID_HANDLE_VALUE, "source lease excludes new write opens"); if (write != INVALID_HANDLE_VALUE) CloseHandle(write);
    Check(!MoveFileExW(file.c_str(), (root / "moved.txt").c_str(), 0), "held source cannot be renamed");
    Check(!MoveFileExW((root / "source").c_str(), (root / "moved-parent").c_str(), 0), "held source parent cannot be replaced");
    { std::ofstream sibling(root / "source" / "sibling.txt"); sibling << "unrelated"; Check(static_cast<bool>(sibling), "directory read lease permits ordinary sibling file creation"); }
    Check(!MoveFileExW((root / "target").c_str(), (root / "moved-target").c_str(), 0), "held target directory cannot be replaced");
    source.value.reset(); target.value.reset();
    HANDLE metadata = CreateFileW(file.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    const uint64_t later = original.identity.last_write + 10000000;
    FILETIME time{static_cast<DWORD>(later), static_cast<DWORD>(later >> 32)};
    Check(metadata != INVALID_HANDLE_VALUE && SetFileTime(metadata, nullptr, nullptr, &time), "fixture changes mtime without changing length");
    if (metadata != INVALID_HANDLE_VALUE) CloseHandle(metadata);
    Check(!ReopenSource(original, cancelled), "same identity and size with changed mtime is rejected");
    std::ofstream(file, std::ios::trunc) << "changed length";
    Check(!ReopenSource(original, cancelled), "source content metadata change invalidates snapshot");
    auto changed = CaptureSource(file.wstring(), cancelled);
    const auto before_replace = changed.value->snapshot; changed.value.reset();
    fs::rename(file, root / "old-file.txt"); std::ofstream(file) << "changed length";
    Check(!ReopenSource(before_replace, cancelled), "same name replacement file identity is rejected");
    auto parent_source = CaptureSource(file.wstring(), cancelled);
    const auto before_parent = parent_source.value->snapshot; parent_source.value.reset();
    fs::rename(root / "source", root / "old-parent"); fs::create_directory(root / "source");
    Check(CreateHardLinkW(file.c_str(), (root / "old-parent" / "example.txt").c_str(), nullptr) != FALSE,
        "fixture reuses same file identity under a replacement parent");
    Check(!ReopenSource(before_parent, cancelled), "parent replacement rejected even when leaf file identity is unchanged");
    fs::rename(root / "target", root / "old-target"); fs::create_directory(root / "target");
    Check(!ReopenTarget(destination, cancelled), "target directory replacement identity is rejected");
    Check(!CaptureSource((root / "source").wstring(), cancelled), "directory cannot be a source file");
    Check(!CaptureTarget(file.wstring(), cancelled), "file cannot be target directory");
    Check(!CaptureSource((root / "missing").wstring(), cancelled), "missing source returns error");
    for (const auto* invalid : {L"relative.txt", L"\\\\server\\share\\file", L"\\\\.\\PhysicalDrive0", L"C:\\x:stream", L"C:\\a\\..\\b", L"C:\\NUL", L"C:\\bad.\\x"})
        Check(!CaptureSource(invalid, cancelled), "unsafe or ambiguous path form rejected");
    const bool junction = Junction(root / "link", root / "source");
    Check(junction, "isolated junction fixture created");
    if (junction) {
        Check(!CaptureTarget((root / "link").wstring(), cancelled), "leaf reparse directory rejected");
        Check(!CaptureSource((root / "link" / "example.txt").wstring(), cancelled), "ancestor reparse rejected before traversal");
        RemoveDirectoryW((root / "link").c_str());
    }
    auto long_dir = root;
    for (int i = 0; i < 5; ++i) long_dir /= std::wstring(55, static_cast<wchar_t>(L'a' + i));
    const fs::path extended_dir(L"\\\\?\\" + long_dir.wstring());
    fs::create_directories(extended_dir);
    const auto long_file = extended_dir / "long.txt"; std::ofstream(long_file) << "long";
    Check(static_cast<bool>(CaptureSource(long_file.wstring(), cancelled)), "extended local path beyond MAX_PATH accepted component by component");
    cancelled = true;
    Check(CaptureSource(file.wstring(), cancelled).error == ERROR_CANCELLED &&
        CaptureTarget((root / "target").wstring(), cancelled).error == ERROR_CANCELLED, "cancelled requests return no held lease");
    // Only this uniquely created fixture tree is removed; no external targets exist.
    if (root.parent_path() != fs::absolute("bench_data") || !root.filename().wstring().starts_with(L"transfer-snapshot-"))
        throw std::runtime_error("Fixture cleanup path escaped workspace");
    fs::remove_all(fs::path(L"\\\\?\\" + root.wstring()));
    return failures ? 1 : 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "[FAIL] fixture: %s\n", error.what()); return 1; }
}
