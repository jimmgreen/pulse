#include "../app/extensions/resource_settings.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <windows.h>
#include <winioctl.h>
using namespace pulse;
namespace {
int failures = 0;
void Check(bool ok, const char *text) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) {
        ++failures;
    }
}
bool Ok(const json::Value &result) { return result.get_bool("ok"); }
std::string Code(const json::Value &result) {
    const auto *error = result.find("error");
    return error ? error->get_string("code") : std::string{};
}
uint64_t Version(const json::Value &result) {
    const auto *data = result.find("data");
    int64_t version = -1;
    if (!data || !data->find("version") || !data->find("version")->get_int(version)) {
        return UINT64_MAX;
    }
    return static_cast<uint64_t>(version);
}
std::string Value(const json::Value &result, const char *key) {
    const auto *data = result.find("data");
    const auto *values = data ? data->find("values") : nullptr;
    return values ? values->get_string(key) : std::string{};
}
std::filesystem::path File(const std::filesystem::path &root, std::string_view package,
                           const wchar_t *suffix = L".json") {
    std::wstring name = L"pkg-";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (unsigned char c : package) {
        name += hex[c >> 4];
        name += hex[c & 15];
    }
    return root / L"extension-settings" / (name + suffix);
}
void Put(const std::filesystem::path &path, const std::string &bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
std::string Bytes(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}
bool Junction(const std::filesystem::path &link, const std::filesystem::path &target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) {
        return false;
    }
    HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(link.c_str());
        return false;
    }
    const std::wstring substitute = L"\\??\\" + target.wstring(), print = target.wstring();
    const size_t path_bytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
    struct MountPoint {
        DWORD tag;
        WORD size, reserved;
        WORD substitute_offset, substitute_size, print_offset, print_size;
    };
    std::vector<unsigned char> bytes(sizeof(MountPoint) + path_bytes, 0);
    auto *point = reinterpret_cast<MountPoint *>(bytes.data());
    point->tag = IO_REPARSE_TAG_MOUNT_POINT;
    point->size = static_cast<WORD>(8 + path_bytes);
    point->substitute_size = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    point->print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    point->print_size = static_cast<WORD>(print.size() * sizeof(wchar_t));
    auto *names = reinterpret_cast<wchar_t *>(bytes.data() + sizeof(MountPoint));
    std::copy(substitute.begin(), substitute.end(), names);
    std::copy(print.begin(), print.end(), names + substitute.size() + 1);
    DWORD returned = 0;
    const bool made =
        DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, bytes.data(), static_cast<DWORD>(bytes.size()),
                        nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(directory);
    if (!made) {
        RemoveDirectoryW(link.c_str());
    }
    return made;
}
bool Symbolic(const std::filesystem::path &link, const std::filesystem::path &target, bool directory) {
    DWORD flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(), flags | 0x2)) {
        return true;
    }
    return GetLastError() == ERROR_INVALID_PARAMETER &&
           CreateSymbolicLinkW(link.c_str(), target.c_str(), flags);
}
DWORD SpawnCas(const std::wstring &program, const std::filesystem::path &root, PROCESS_INFORMATION &process) {
    std::wstring command = L"\"" + program + L"\" --cas \"" + root.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    return CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                          nullptr, &startup, &process)
               ? ERROR_SUCCESS
               : GetLastError();
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--cas") {
        const auto written = extensions::WriteOwnSetting(
            argv[2], "race", "winner", std::to_string(GetCurrentProcessId()), 0, [] { return true; });
        return Ok(written) ? 0 : Code(written) == "version_conflict" ? 3 : 4;
    }
    wchar_t temporary[32768]{};
    if (!GetTempPathW(32768, temporary)) {
        return 2;
    }
    const auto workspace =
        std::filesystem::path(temporary) / (L"pulse-own-settings-" + std::to_wstring(GetCurrentProcessId()) +
                                            L"-" + std::to_wstring(GetTickCount64()));
    if (!workspace.is_absolute() || !CreateDirectoryW(workspace.c_str(), nullptr)) {
        return 2;
    }
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    } cleanup{workspace};
    const auto root = workspace / L"data", outside = workspace / L"outside";
    std::filesystem::create_directory(root);
    std::filesystem::create_directory(outside);
    const auto allow = [] { return true; };
    auto initial = extensions::ReadOwnSettings(root.wstring(), "Alpha");
    Check(Ok(initial) && Version(initial) == 0, "missing package settings read as empty version zero");
    auto first = extensions::WriteOwnSetting(root.wstring(), "Alpha", "theme", std::string("dark"), 0, allow);
    Check(Ok(first) && Version(first) == 1 && Value(first, "theme") == "dark",
          "first CAS write persists version and value");
    auto conflict =
        extensions::WriteOwnSetting(root.wstring(), "Alpha", "theme", std::string("light"), 0, allow);
    Check(Code(conflict) == "version_conflict" &&
              Value(extensions::ReadOwnSettings(root.wstring(), "Alpha"), "theme") == "dark",
          "stale writer cannot overwrite committed settings");
    const std::string unicode = "\xE4\xB8\xAD\xF0\x9F\x98\x80";
    auto second = extensions::WriteOwnSetting(root.wstring(), "Alpha", "unicode", unicode, 1, allow);
    Check(Ok(second) && Version(second) == 2 &&
              Value(extensions::ReadOwnSettings(root.wstring(), "Alpha"), "unicode") == unicode,
          "Unicode UTF8 survives durable roundtrip");
    Check(Ok(extensions::WriteOwnSetting(root.wstring(), "alpha", "theme", std::string("separate"), 0,
                                         allow)) &&
              Value(extensions::ReadOwnSettings(root.wstring(), "Alpha"), "theme") == "dark",
          "case-distinct package identities remain isolated on Windows");
    auto removed = extensions::WriteOwnSetting(root.wstring(), "Alpha", "theme", std::nullopt, 2, allow);
    Check(Ok(removed) && Version(removed) == 3 && !removed.find("data")->find("values")->find("theme"),
          "deletion removes only selected key and advances version");
    Check(Code(extensions::WriteOwnSetting(root.wstring(), "Alpha", "x", std::string("x"), 3,
                                           [] { return false; })) == "not_authorized",
          "initial authorization denial does not mutate data");
    int checks = 0;
    const auto before = Bytes(File(root, "Alpha"));
    auto revoked = extensions::WriteOwnSetting(root.wstring(), "Alpha", "x", std::string("x"), 3,
                                               [&] { return ++checks < 3; });
    Check(Code(revoked) == "not_authorized" && checks == 3 && Bytes(File(root, "Alpha")) == before,
          "revocation immediately before atomic commit preserves original bytes");
    Check(Code(extensions::WriteOwnSetting(root.wstring(), "Alpha", "bad key", std::string("x"), 3, allow)) ==
              "invalid_argument",
          "invalid setting key rejected");
    Check(Code(extensions::WriteOwnSetting(root.wstring(), "Alpha", "large", std::string(1025, 'x'), 3,
                                           allow)) == "invalid_argument",
          "value byte bound enforced");
    Check(Code(extensions::WriteOwnSetting(root.wstring(), "Alpha", "bad", std::string("\xC0\xAF"), 3,
                                           allow)) == "invalid_argument",
          "malformed UTF8 rejected");
    for (int i = 0; i < 64; ++i) {
        if (!Ok(extensions::WriteOwnSetting(root.wstring(), "keys", "key" + std::to_string(i),
                                            std::string("v"), static_cast<uint64_t>(i), allow))) {
            Check(false, "populate 64-key boundary");
            break;
        }
    }
    Check(Code(extensions::WriteOwnSetting(root.wstring(), "keys", "overflow", std::string("v"), 64,
                                           allow)) == "limit_exceeded" &&
              Version(extensions::ReadOwnSettings(root.wstring(), "keys")) == 64,
          "sixty-fifth key rejected without advancing version");
    for (int i = 0; i < 15; ++i) {
        if (!Ok(extensions::WriteOwnSetting(root.wstring(), "bytes", "key" + std::to_string(i),
                                            std::string(1024, 'x'), static_cast<uint64_t>(i), allow))) {
            Check(false, "populate bounded aggregate payload");
            break;
        }
    }
    Check(Code(extensions::WriteOwnSetting(root.wstring(), "bytes", "overflow", std::string(1024, 'x'), 15,
                                           allow)) == "limit_exceeded",
          "serialized aggregate stays within sixteen KiB");
    Put(File(root, "corrupt"), "{bad json}");
    Check(Code(extensions::ReadOwnSettings(root.wstring(), "corrupt")) == "corrupt_data" &&
              Code(extensions::WriteOwnSetting(root.wstring(), "corrupt", "key", std::string("value"), 0,
                                               allow)) == "corrupt_data" &&
              Bytes(File(root, "corrupt")) == "{bad json}",
          "corrupt persisted bytes are refused and never overwritten");
    Put(File(root, "oversize"), std::string(16385, 'x'));
    Check(Code(extensions::ReadOwnSettings(root.wstring(), "oversize")) == "corrupt_data",
          "oversized persisted file refused before parsing");
    Put(File(root, "exhausted"), R"({"version":9007199254740991,"values":{}})");
    Check(Code(extensions::WriteOwnSetting(root.wstring(), "exhausted", "key", std::string("value"),
                                           (uint64_t{1} << 53) - 1, allow)) == "version_exhausted",
          "version cannot exceed exact JSON integer domain");
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    PROCESS_INFORMATION a{}, b{};
    const bool started_a = SpawnCas(executable, root, a) == ERROR_SUCCESS,
               started_b = SpawnCas(executable, root, b) == ERROR_SUCCESS;
    Check(started_a && started_b, "two real processes start concurrent CAS writers");
    DWORD exit_a = 4, exit_b = 4;
    if (started_a) {
        if (WaitForSingleObject(a.hProcess, 15000) != WAIT_OBJECT_0) {
            TerminateProcess(a.hProcess, 4);
            WaitForSingleObject(a.hProcess, INFINITE);
        }
        GetExitCodeProcess(a.hProcess, &exit_a);
        CloseHandle(a.hThread);
        CloseHandle(a.hProcess);
    }
    if (started_b) {
        if (WaitForSingleObject(b.hProcess, 15000) != WAIT_OBJECT_0) {
            TerminateProcess(b.hProcess, 4);
            WaitForSingleObject(b.hProcess, INFINITE);
        }
        GetExitCodeProcess(b.hProcess, &exit_b);
        CloseHandle(b.hThread);
        CloseHandle(b.hProcess);
    }
    Check(((exit_a == 0 && exit_b == 3) || (exit_a == 3 && exit_b == 0)) &&
              Version(extensions::ReadOwnSettings(root.wstring(), "race")) == 1,
          "cross-process file lock serializes CAS with one winner");
    const auto external = outside / L"untouched.txt";
    Put(external, "must stay unchanged");
    const auto link_root = workspace / L"junction";
    if (Junction(link_root, outside)) {
        std::filesystem::create_directory(outside / L"nested");
        Check(Code(extensions::WriteOwnSetting((link_root / L"nested").wstring(), "pkg", "key",
                                               std::string("value"), 0, allow)) == "unsafe_path" &&
                  !std::filesystem::exists(outside / L"nested" / L"extension-settings"),
              "ancestor junction cannot redirect settings outside host root");
        Check(RemoveDirectoryW(link_root.c_str()) != FALSE, "remove isolated junction itself");
    } else {
        std::printf("[SKIP] directory junction unavailable, error=%lu\n", GetLastError());
    }
    const auto target_link = File(root, "linked");
    if (Symbolic(target_link, external, false)) {
        Check(Code(extensions::ReadOwnSettings(root.wstring(), "linked")) == "unsafe_path" &&
                  Code(extensions::WriteOwnSetting(root.wstring(), "linked", "key", std::string("value"), 0,
                                                   allow)) == "unsafe_path" &&
                  Bytes(external) == "must stay unchanged",
              "target symbolic link is never followed or overwritten");
        Check(DeleteFileW(target_link.c_str()) != FALSE, "remove isolated target symlink itself");
    } else {
        std::printf("[SKIP] file symlink unavailable, error=%lu\n", GetLastError());
    }
    Check(Bytes(external) == "must stay unchanged", "external sentinel remains unchanged");
    return failures ? 1 : 0;
}
