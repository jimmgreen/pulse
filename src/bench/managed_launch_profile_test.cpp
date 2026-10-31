#include "../dialog_launcher/managed_launch_profile.h"
#include "../shell_bridge/runtime_config.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {
namespace model = pulse::dialog_launcher;
namespace cfg = pulse::shell_bridge::runtime;
namespace fs = std::filesystem;
unsigned checks = 0, failures = 0;
void Check(bool good, const char* label) {
    ++checks; if (!good) ++failures;
    std::cout << (good ? "[PASS] " : "[FAIL] ") << label << '\n';
}
std::wstring Self() {
    std::wstring path(32768, 0); const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return {}; path.resize(count); return path;
}
std::wstring CanonicalDirectory(const fs::path& path, DWORD& error) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = GetLastError(); return {}; }
    BY_HANDLE_FILE_INFORMATION info{};
    std::wstring result(32768, 0);
    if (!GetFileInformationByHandle(file, &info)) error = GetLastError();
    else if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) error = ERROR_DIRECTORY;
    else {
        const DWORD count = GetFinalPathNameByHandleW(file, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED);
        if (!count || count >= result.size()) error = count ? ERROR_FILENAME_EXCED_RANGE : GetLastError();
        else { result.resize(count); error = ERROR_SUCCESS; }
    }
    CloseHandle(file);
    if (error) result.clear();
    return result;
}
bool FixtureOutput(const fs::path& requested, const fs::path& executable, fs::path& output, DWORD& error) {
    output.clear(); error = ERROR_BAD_PATHNAME;
    if (!requested.is_absolute() || requested.lexically_normal() != requested || requested.filename().empty() ||
        requested.filename().wstring().find(L':') != std::wstring::npos) return false;
    const auto bench = CanonicalDirectory(executable.parent_path().parent_path() / L"bench_data", error);
    if (bench.empty()) return false;
    const auto fixture = CanonicalDirectory(requested.parent_path(), error);
    if (fixture.empty()) return false;
    const auto parent = CanonicalDirectory(fs::path(fixture).parent_path(), error);
    if (parent.empty()) return false;
    if (_wcsicmp(parent.c_str(), bench.c_str()) != 0 ||
        !fs::path(fixture).filename().wstring().starts_with(L"managed-launch-")) { error = ERROR_BAD_PATHNAME; return false; }
    output = fs::path(fixture) / requested.filename(); error = ERROR_SUCCESS; return true;
}
int FixtureReject(const char* stage, DWORD error, int argc, int code) {
    std::cerr << "fixture_rejected stage=" << stage << " error=" << error << " argc=" << argc << '\n' << std::flush;
    return code;
}
int LaunchFixture(int argc, wchar_t** argv) {
    // A standalone owned target, never launches another process or shows dialogs.
    if (argc != 4 && argc != 5) return FixtureReject("argc", ERROR_INVALID_PARAMETER, argc, 2);
    if (std::wstring_view(argv[3]) != L"two words") return FixtureReject("argv-exact", ERROR_INVALID_PARAMETER, argc, 2);
    if (argc == 5 && std::wstring_view(argv[4]) != L"--wait-release") return FixtureReject("argv-release", ERROR_INVALID_PARAMETER, argc, 2);
    const bool wait_release = argc == 5;
    fs::path requested_output; DWORD path_error = ERROR_SUCCESS;
    if (!FixtureOutput(fs::path(argv[2]), fs::path(Self()), requested_output, path_error))
        return FixtureReject("canonical-bench-child", path_error, argc, 2);
    const fs::path output = requested_output.parent_path() / (requested_output.stem().wstring() + L"-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + requested_output.extension().wstring());
    std::cout << "fixture_paths_valid=1 argv_exact=1 wait_release=" << wait_release << '\n';
    wchar_t control[32768]{};
    const DWORD count = GetEnvironmentVariableW(L"PULSE_DIALOG_CONTROL", control, ARRAYSIZE(control));
    if (!count || count >= ARRAYSIZE(control)) return FixtureReject("control-environment", GetLastError(), argc, 3);
    wchar_t nonce[40]{};
    const DWORD nonce_count = GetEnvironmentVariableW(L"PULSE_DIALOG_NONCE", nonce, ARRAYSIZE(nonce));
    if (!nonce_count || nonce_count >= ARRAYSIZE(nonce)) return FixtureReject("nonce-environment", GetLastError(), argc, 3);
    cfg::Ready ready{}; bool matched = false;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return FixtureReject("process-creation-time", GetLastError(), argc, 3);
    const auto deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        HANDLE file = CreateFileW((fs::path(control) / cfg::kReadyFile).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD read = 0; LARGE_INTEGER size{};
            matched = GetFileSizeEx(file, &size) && size.QuadPart == static_cast<LONGLONG>(sizeof(ready)) &&
                ReadFile(file, &ready, sizeof(ready), &read, nullptr) && read == sizeof(ready) &&
                ready.magic == cfg::kReadyMagic && ready.version == cfg::kVersion && ready.size == sizeof(ready) && !ready.reserved &&
                !ready.nonce[39] && wcscmp(ready.nonce, nonce) == 0 &&
                ready.target_pid == GetCurrentProcessId() && CompareFileTime(&ready.target_created, &created) == 0 &&
                SUCCEEDED(ready.status) && ready.installed_entries;
            CloseHandle(file); if (matched) break;
        }
        Sleep(20);
    }
    HANDLE report = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (report == INVALID_HANDLE_VALUE) return FixtureReject("create-report", GetLastError(), argc, 4);
    const std::string text = "pid=" + std::to_string(GetCurrentProcessId()) + "\nargv_exact=1\nruntime_handshake=" +
        std::to_string(matched) + "\nstatus=" + std::to_string(static_cast<unsigned long>(ready.status)) +
        "\ncoverage=" + std::to_string(ready.installed_entries) + "\n";
    DWORD written = 0; const bool saved = WriteFile(report, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    FlushFileBuffers(report); CloseHandle(report);
    if (wait_release && matched && saved) {
        const auto release = output.parent_path() / (L"release-" + std::to_wstring(GetCurrentProcessId()) + L".signal");
        const auto release_deadline = GetTickCount64() + 20000;
        while (GetTickCount64() < release_deadline) {
            if (GetFileAttributesW(release.c_str()) != INVALID_FILE_ATTRIBUTES) return 0;
            Sleep(20);
        }
        return 6; // A timed-out inspection is not reported as a successful fixture.
    }
    // Give the launcher time to observe the ready file before this owned target exits.
    Sleep(3000);
    return matched && saved ? 0 : 5;
}
}
int wmain(int argc, wchar_t** argv) {
    std::cout << std::unitbuf;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--launch-fixture") return LaunchFixture(argc, argv);
    if (argc != 1) return 2;
    model::ManagedLaunchProfile p;
#ifdef _WIN64
    p.machine = IMAGE_FILE_MACHINE_AMD64;
#else
    p.machine = IMAGE_FILE_MACHINE_I386;
#endif
    p.component_root = L"C:\\bundle\\v1"; p.host = p.component_root + L"\\pulse.exe";
    p.runtime = p.component_root + L"\\runtime.dll"; p.launcher = p.component_root + L"\\launcher.exe";
    p.target = L"C:\\apps\\target.exe"; p.cwd = L"C:\\work"; p.control_root = L"C:\\sessions";
    p.arguments = {L"", L"two words", L"quote\"slash\\", L"中文"};
    std::vector<unsigned char> encoded; model::ManagedLaunchProfile decoded;
    Check(model::EncodeProfile(p, encoded) == 0 && model::DecodeProfile(encoded, decoded) == 0 && decoded.arguments == p.arguments,
        "fixed-endian profile preserves exact argument boundaries and Unicode");
    bool truncation = true;
    for (size_t i = 0; i < encoded.size(); ++i) if (model::DecodeProfile(std::span(encoded).first(i), decoded) == 0 || !decoded.host.empty()) truncation = false;
    Check(truncation, "every truncation rejected with empty output");
    auto bad = encoded; bad[4] = 99; Check(model::DecodeProfile(bad, decoded) == ERROR_INVALID_DATA, "unknown version rejected");
    bad = encoded; bad[12] = 1; Check(model::DecodeProfile(bad, decoded) == ERROR_INVALID_DATA, "reserved field rejected");
    bad = encoded; bad[16] = 0xff; bad[17] = 0xff; bad[18] = 0xff; bad[19] = 0xff;
    Check(model::DecodeProfile(bad, decoded) == ERROR_INVALID_DATA, "declared string length bounded before allocation");
    bad = encoded; bad.push_back(0); Check(model::DecodeProfile(bad, decoded) == ERROR_INVALID_DATA, "trailing bytes rejected");
    bad = encoded; bad[20] = 0; bad[21] = 0; Check(model::DecodeProfile(bad, decoded) == ERROR_INVALID_DATA, "embedded NUL rejected");
    bad = encoded; bad[20] = 0; bad[21] = 0xd8; Check(model::DecodeProfile(bad, decoded) == ERROR_INVALID_DATA, "unpaired surrogate rejected");
    auto invalid = p; invalid.target = L"relative.exe"; Check(model::EncodeProfile(invalid, bad) == ERROR_INVALID_DATA && bad.empty(), "relative path rejected");
    invalid = p; invalid.target = L"\\\\server\\share\\app.exe"; Check(model::EncodeProfile(invalid, bad) == ERROR_INVALID_DATA, "UNC rejected by local profile scope");
    invalid = p; invalid.arguments.resize(65); Check(model::EncodeProfile(invalid, bad) == ERROR_INVALID_DATA, "argument count bounded");
    invalid = p; invalid.target = L"C:\\apps\\..\\app.exe"; Check(model::EncodeProfile(invalid, bad) == ERROR_INVALID_DATA, "parent traversal rejected");
    const auto bench = fs::path(Self()).parent_path().parent_path() / L"bench_data";
    CreateDirectoryW(bench.c_str(), nullptr);
    const auto fixture = bench / (L"managed-launch-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(fixture.c_str(), nullptr)) { Check(false, "unique fixture created"); return 1; }
    fs::path normal_output, extended_output; DWORD path_error = ERROR_SUCCESS;
    Check(FixtureOutput(fixture / L"report.tsv", Self(), normal_output, path_error), "fixture requires canonical direct bench child");
    const std::wstring extended_self = Self().starts_with(LR"(\\?\)") ? Self() : std::wstring(LR"(\\?\)") + Self();
    Check(FixtureOutput(fixture / L"report.tsv", extended_self, extended_output, path_error) && normal_output == extended_output,
        "extended executable prefix and ordinary output resolve to the same bench identity");
    Check(FixtureOutput(normal_output, extended_self, extended_output, path_error) && normal_output == extended_output,
        "extended output path preserves the direct bench child constraint");
    Check(!FixtureOutput(bench / L"report.tsv", extended_self, extended_output, path_error), "bench root itself is not a dedicated direct child fixture");
    const auto host = fixture / L"pulse.exe", launcher = fixture / L"launcher.exe", target = fixture / L"target.exe", runtime = fixture / L"runtime.dll";
    wchar_t system[32768]{}; const UINT system_count = GetSystemDirectoryW(system, ARRAYSIZE(system));
    const bool files = system_count && system_count < ARRAYSIZE(system) && CopyFileW(Self().c_str(), host.c_str(), TRUE) &&
        CopyFileW(Self().c_str(), launcher.c_str(), TRUE) && CopyFileW(Self().c_str(), target.c_str(), TRUE) &&
        CopyFileW((fs::path(system) / L"kernel32.dll").c_str(), runtime.c_str(), TRUE);
    Check(files, "owned version component fixtures copied without execution");
    if (files) {
        p.host = host.wstring(); p.runtime = runtime.wstring(); p.target = target.wstring(); p.cwd = fixture.wstring(); p.control_root = fixture.wstring();
        Check(model::CaptureProfile(p, launcher.wstring()) == 0, "capture absolute same-architecture bundle");
        Check(model::ValidateProfile(p, launcher.wstring()) == 0, "unchanged components validate");
        const auto profile = fixture / L"launch.profile";
        Check(model::SaveProfile(profile.wstring(), p) == 0 && model::LoadProfile(profile.wstring(), decoded) == 0 &&
            model::ValidateProfile(decoded, launcher.wstring()) == 0, "create load and validate reusable profile");
        Check(model::SaveProfile(profile.wstring(), p) == ERROR_FILE_EXISTS, "existing profile never overwritten");
        invalid = p; invalid.component_root += L"\\other-version";
        Check(model::ValidateProfile(invalid, launcher.wstring()) == ERROR_REVISION_MISMATCH, "saved version directory cannot silently change");
        invalid = p; invalid.components[1].file ^= 1;
        Check(model::ValidateProfile(invalid, launcher.wstring()) == ERROR_REVISION_MISMATCH, "changed component identity rejected");
        invalid = p; invalid.machine = p.machine == IMAGE_FILE_MACHINE_AMD64 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
        Check(model::ValidateProfile(invalid, launcher.wstring()) == ERROR_REVISION_MISMATCH, "saved architecture cannot silently change");
        invalid = p; invalid.runtime = target.wstring();
        Check(model::CaptureProfile(invalid, launcher.wstring()) == ERROR_BAD_EXE_FORMAT, "runtime must be a DLL image");
        Check(DeleteFileW(target.c_str()) && CopyFileW(Self().c_str(), target.c_str(), TRUE) &&
            model::ValidateProfile(p, launcher.wstring()) == 0, "target replacement allowed after fresh path and architecture validation");
        HANDLE changed = CreateFileW(runtime.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        FILETIME time{}; GetSystemTimeAsFileTime(&time); time.dwHighDateTime ^= 1;
        const bool changed_time = changed != INVALID_HANDLE_VALUE && SetFileTime(changed, nullptr, nullptr, &time);
        if (changed != INVALID_HANDLE_VALUE) CloseHandle(changed);
        Check(changed_time && model::ValidateProfile(p, launcher.wstring()) == ERROR_REVISION_MISMATCH, "real component metadata change invalidates saved version");
        bool disabled = true;
        Check(model::ReadDisableMarker(fixture.wstring(), disabled) == 0 && !disabled, "missing local disable marker is distinguished from historical handshake");
        const auto marker = fixture / L"disable";
        HANDLE marker_file = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        const bool marker_created = marker_file != INVALID_HANDLE_VALUE;
        if (marker_created) CloseHandle(marker_file);
        Check(marker_created && model::ReadDisableMarker(fixture.wstring(), disabled) == 0 && disabled, "real disable marker reports disabled despite existing profile");
        Check(model::ReadDisableMarker((fixture / L"missing").wstring(), disabled) != 0, "missing control directory is unknown rather than enabled");
        Check(DeleteFileW(marker.c_str()) != FALSE, "remove owned disable marker");
        Check(DeleteFileW(profile.c_str()) != FALSE, "remove owned profile");
    }
    bool cleanup = true;
    for (const auto& path : {host, launcher, target, runtime}) if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) cleanup = false;
    Check(cleanup && RemoveDirectoryW(fixture.c_str()), "remove only known owned fixture files and directory");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
