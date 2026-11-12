#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../app/extensions/native/crash_guard.h"
#include "../app/extensions/native_image_verify.h"
#include "../app/extensions/package_revision.h"
#include "../app/extensions/runtime.h"
#include "../common/crash_reporter.h"
#include "../common/json_value.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace crash_guard = pulse::extensions::native::crash_guard;

namespace {

constexpr char kPackageId[] = "crash.test.pkg";
constexpr char kPackageName[] = "Crash Test Package";
constexpr wchar_t kPackageNameWide[] = L"Crash Test Package";
constexpr DWORD kExpectedCrashExitCode = 0xC0000005u;

volatile int* g_host_crash_sink = nullptr;
int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* description) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", description);
    std::fflush(stdout);
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
    }
}

__declspec(noinline) void TriggerHostCrashOutsideModules() {
    volatile int* target = g_host_crash_sink;
    *target = 0xCAFE;
}

// Isolated from C++ stack objects with destructors (avoids MSVC C2712).
// Because SEH evaluates the __except filter before unwinding caller frames,
// ActivationScope and CallScope in the caller remain active during ReportFatal.
__declspec(noinline) void InvokeFatalAndTerminate(void (*fn)(), const char* context) noexcept {
    __try {
        fn();
    } __except (pulse::crash::ReportFatal(GetExceptionInformation(), context), EXCEPTION_EXECUTE_HANDLER) {
        TerminateProcess(GetCurrentProcess(), kExpectedCrashExitCode);
    }
}

std::string ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool WriteBytes(const fs::path& path, std::string_view bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!bytes.empty()) {
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(out);
}

std::string LowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool RvaToOffset(std::string_view pe, DWORD rva, size_t& out_offset) {
    if (pe.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, pe.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return false;
    const auto nt_off = static_cast<size_t>(dos.e_lfanew);
    if (nt_off + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + sizeof(IMAGE_OPTIONAL_HEADER64) > pe.size()) return false;
    DWORD sig = 0;
    IMAGE_FILE_HEADER fh{};
    std::memcpy(&sig, pe.data() + nt_off, sizeof(sig));
    std::memcpy(&fh, pe.data() + nt_off + sizeof(DWORD), sizeof(fh));
    if (sig != IMAGE_NT_SIGNATURE) return false;
    const size_t sec_off = nt_off + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + fh.SizeOfOptionalHeader;
    if (sec_off + static_cast<size_t>(fh.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) > pe.size()) return false;
    for (WORD i = 0; i < fh.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sec{};
        std::memcpy(&sec, pe.data() + sec_off + static_cast<size_t>(i) * sizeof(sec), sizeof(sec));
        const DWORD span = (std::max)(sec.Misc.VirtualSize, sec.SizeOfRawData);
        if (rva >= sec.VirtualAddress && rva < sec.VirtualAddress + span) {
            const DWORD delta = rva - sec.VirtualAddress;
            if (delta >= sec.SizeOfRawData) return false;
            const size_t file_pos = static_cast<size_t>(sec.PointerToRawData) + delta;
            if (file_pos >= pe.size()) return false;
            out_offset = file_pos;
            return true;
        }
    }
    return false;
}

bool PeImportsForbiddenGuiDll(std::string_view pe, std::string& offending_dll) {
    offending_dll.clear();
    if (pe.size() < sizeof(IMAGE_DOS_HEADER)) return true;
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, pe.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return true;
    const auto nt_off = static_cast<size_t>(dos.e_lfanew);
    if (nt_off + sizeof(IMAGE_NT_HEADERS64) > pe.size()) return true;
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, pe.data() + nt_off, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return true;
    const auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return false;
    size_t imp_off = 0;
    if (!RvaToOffset(pe, dir.VirtualAddress, imp_off)) return true;
    while (imp_off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= pe.size()) {
        IMAGE_IMPORT_DESCRIPTOR desc{};
        std::memcpy(&desc, pe.data() + imp_off, sizeof(desc));
        if (desc.Name == 0 && desc.OriginalFirstThunk == 0 && desc.FirstThunk == 0) break;
        size_t name_off = 0;
        if (desc.Name != 0 && RvaToOffset(pe, desc.Name, name_off)) {
            const size_t max_len = pe.size() - name_off;
            const void* nul = std::memchr(pe.data() + name_off, '\0', max_len);
            if (nul) {
                const auto len = static_cast<size_t>(static_cast<const char*>(nul) - (pe.data() + name_off));
                const std::string dll_name = LowerAscii(std::string(pe.data() + name_off, len));
                if (dll_name == "user32.dll" || dll_name == "gdi32.dll") {
                    offending_dll = dll_name;
                    return true;
                }
            }
        }
        imp_off += sizeof(IMAGE_IMPORT_DESCRIPTOR);
    }
    return false;
}

struct StoreFixtureInfo {
    std::string entry_sha256;
    std::string sidecar_sha256;
    std::string manifest_revision;
};

bool WriteStorePackage(const fs::path& data_root, std::string_view entry_bytes,
                       std::string_view sidecar_bytes, StoreFixtureInfo& out) {
    if (!pulse::extensions::ValidateNativeImageBytes(entry_bytes) ||
        !pulse::extensions::ValidateNativeImageBytes(sidecar_bytes)) {
        return false;
    }
    out.entry_sha256 = pulse::extensions::PackageRevision(entry_bytes);
    out.sidecar_sha256 = pulse::extensions::PackageRevision(sidecar_bytes);
    if (out.entry_sha256.empty() || out.sidecar_sha256.empty()) return false;

    auto sidecar_item = pulse::json::Value::Object();
    sidecar_item.set("path", "crash_test_sidecar_dll.dll");
    sidecar_item.set("sha256", out.sidecar_sha256);
    auto files = pulse::json::Value::Array();
    files.push(std::move(sidecar_item));

    auto native = pulse::json::Value::Object();
    native.set("version", 1);
    native.set("architecture", "windows-x64");
    native.set("library", "crash_test_entry_dll.dll");
    native.set("sha256", out.entry_sha256);
    native.set("files", std::move(files));

    auto manifest = pulse::json::Value::Object();
    manifest.set("apiVersion", 1);
    manifest.set("id", kPackageId);
    manifest.set("name", kPackageName);
    manifest.set("native", std::move(native));

    const std::string manifest_bytes = pulse::json::Serialize(manifest);
    out.manifest_revision = pulse::extensions::PackageRevision(manifest_bytes);
    if (out.manifest_revision.empty()) return false;

    const fs::path store_root = data_root / L"extensions";
    const fs::path version_file = store_root / L"versions" / (out.manifest_revision + ".json");
    const fs::path pkg_dir = store_root / L"packages" / out.manifest_revision;
    if (!WriteBytes(version_file, manifest_bytes)) return false;
    if (!WriteBytes(data_root / L"pulse.extension.json", manifest_bytes)) return false;
    if (!WriteBytes(pkg_dir / L"crash_test_entry_dll.dll", entry_bytes)) return false;
    if (!WriteBytes(pkg_dir / L"crash_test_sidecar_dll.dll", sidecar_bytes)) return false;

    auto pkg_entry = pulse::json::Value::Object();
    pkg_entry.set("id", kPackageId);
    pkg_entry.set("name", kPackageName);
    pkg_entry.set("enabled", true);
    pkg_entry.set("activeRevision", out.manifest_revision);
    auto packages = pulse::json::Value::Array();
    packages.push(std::move(pkg_entry));

    auto registry = pulse::json::Value::Object();
    registry.set("formatVersion", 1);
    registry.set("packages", std::move(packages));
    return WriteBytes(store_root / L"registry.json", pulse::json::Serialize(registry));
}

std::string ReadCrashDiagnosticReports(const fs::path& data_root) {
    std::string combined;
    std::error_code ec;
    const fs::path crashes_dir = data_root / L"Diagnostics" / L"Crashes";
    if (!fs::exists(crashes_dir, ec)) return combined;
    for (const auto& entry : fs::directory_iterator(crashes_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto ext = entry.path().extension().wstring();
        if (ext == L".json" || ext == L".log") {
            combined += ReadBytes(entry.path());
            combined.push_back('\n');
        }
    }
    return combined;
}

DWORD SpawnChildMode(const fs::path& exe_path, const wchar_t* mode, const fs::path& data_root) {
    if (!SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data_root.c_str())) return 20;
    std::wstring cmd = L"\"" + exe_path.wstring() + L"\" --child " + mode + L" \"" + data_root.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_path.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return 21;
    }
    const DWORD wait = WaitForSingleObject(pi.hProcess, 30000);
    DWORD exit_code = 22;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exit_code);
    } else {
        TerminateProcess(pi.hProcess, 23);
        WaitForSingleObject(pi.hProcess, 5000);
        exit_code = 23;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exit_code;
}

struct HostStartupSimulation {
    bool startup_ok = false;
    size_t package_count = 0;
    bool package_enabled = false;
    std::vector<std::wstring> newly_disabled;
};

HostStartupSimulation SimulateNextStartup(const fs::path& exe_path, const fs::path& data_root) {
    HostStartupSimulation sim;
    if (!SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data_root.c_str())) return sim;
    std::wstring arg0 = exe_path.wstring();
    std::wstring arg1 = L"--test-instance";
    wchar_t* argv_sim[]{arg0.data(), arg1.data()};
    const auto startup = pulse::extensions::PrepareStartup(2, argv_sim);
    if (startup.exit_requested) {
        pulse::extensions::PrepareShutdown();
        return sim;
    }
    sim.startup_ok = true;
    sim.newly_disabled = crash_guard::ApplyAtStartup(true);
    sim.package_count = pulse::extensions::PackageCount();
    if (sim.package_count > 0) {
        sim.package_enabled = pulse::extensions::PackageEnabled(0);
    }
    pulse::extensions::PrepareShutdown();
    return sim;
}

int RunChild(const std::wstring& mode, const fs::path& data_root, const fs::path& exe_path) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data_root.c_str())) return 30;

    std::wstring arg0 = exe_path.wstring();
    std::wstring arg1 = L"--test-instance";
    wchar_t* startup_argv[]{arg0.data(), arg1.data()};
    const auto startup = pulse::extensions::PrepareStartup(2, startup_argv);
    if (startup.exit_requested || pulse::extensions::PackageCount() != 1 || !pulse::extensions::PackageEnabled(0)) {
        return 31;
    }
    if (!pulse::crash::Initialize({pulse::crash::ProcessRole::App, false, data_root.wstring()})) {
        return 32;
    }
    const auto initial_disabled = crash_guard::ApplyAtStartup(true);
    if (!initial_disabled.empty() || !pulse::extensions::PackageEnabled(0)) {
        return 33;
    }

    const auto* manifest = pulse::extensions::PackageManifest(0);
    if (!manifest || !manifest->native) return 34;
    const std::wstring entry_path = pulse::extensions::PackageNativePath(0);
    const fs::path pkg_dir = fs::path(entry_path).parent_path();

    using PingFn = int (*)();
    using CrashFn = void (*)();

    if (mode == L"entry_activation") {
        HMODULE entry_mod = LoadLibraryExW(entry_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!entry_mod) return 40;
        const auto ping = reinterpret_cast<PingFn>(GetProcAddress(entry_mod, "EntryPing"));
        const auto crash_fn = reinterpret_cast<CrashFn>(GetProcAddress(entry_mod, "TriggerEntryCrash"));
        if (!ping || !crash_fn || ping() != 42) return 41;

        crash_guard::ActivationScope activation(0);
        crash_guard::CallScope call_scope(manifest->id.c_str());
        if (!fs::exists(data_root / L"extensions" / L"native-activating.txt")) return 42;
        if (!crash_guard::ActiveCallPackage() || std::string_view(crash_guard::ActiveCallPackage()) != manifest->id) {
            return 43;
        }
        InvokeFatalAndTerminate(crash_fn, "entry_activation");
        return 44;
    }

    if (mode == L"sidecar_runtime") {
        HMODULE entry_mod = nullptr;
        {
            crash_guard::ActivationScope activation(0);
            entry_mod = LoadLibraryExW(entry_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!entry_mod) return 50;
            const auto ping = reinterpret_cast<PingFn>(GetProcAddress(entry_mod, "EntryPing"));
            if (!ping || ping() != 42) return 51;
        }
        if (fs::exists(data_root / L"extensions" / L"native-activating.txt")) return 52;

        const fs::path sidecar_path = pkg_dir / L"crash_test_sidecar_dll.dll";
        HMODULE sidecar_mod = LoadLibraryExW(sidecar_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!sidecar_mod) return 53;
        const auto sidecar_ping = reinterpret_cast<PingFn>(GetProcAddress(sidecar_mod, "SidecarPing"));
        const auto sidecar_crash = reinterpret_cast<CrashFn>(GetProcAddress(sidecar_mod, "TriggerSidecarCrash"));
        if (!sidecar_ping || !sidecar_crash || sidecar_ping() != 7) return 54;

        if (!crash_guard::TrackPackageModules(0, entry_mod, pkg_dir.wstring(), &*manifest->native)) {
            return 55;
        }
        const char* attributed = crash_guard::FindAttributedPackage(reinterpret_cast<const void*>(sidecar_crash));
        if (!attributed || std::string_view(attributed) != manifest->id) {
            return 56;
        }
        InvokeFatalAndTerminate(sidecar_crash, "sidecar_runtime");
        return 57;
    }

    if (mode == L"host_outside_modules") {
        HMODULE entry_mod = LoadLibraryExW(entry_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        const fs::path sidecar_path = pkg_dir / L"crash_test_sidecar_dll.dll";
        HMODULE sidecar_mod = LoadLibraryExW(sidecar_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!entry_mod || !sidecar_mod) return 60;
        if (!crash_guard::TrackPackageModules(0, entry_mod, pkg_dir.wstring(), &*manifest->native)) {
            return 61;
        }
        if (crash_guard::FindAttributedPackage(reinterpret_cast<const void*>(&TriggerHostCrashOutsideModules)) != nullptr) {
            return 62;
        }
        crash_guard::CallScope call_scope(manifest->id.c_str());
        if (!crash_guard::ActiveCallPackage() || std::string_view(crash_guard::ActiveCallPackage()) != manifest->id) {
            return 63;
        }
        InvokeFatalAndTerminate(&TriggerHostCrashOutsideModules, "host_outside_modules");
        return 64;
    }

    return 39;
}

struct ScopedDirectoryCleanup {
    fs::path path;
    ~ScopedDirectoryCleanup() {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    wchar_t module_buf[32768]{};
    const DWORD module_len = GetModuleFileNameW(nullptr, module_buf, 32768);
    if (!module_len || module_len >= 32768) {
        std::fprintf(stderr, "Cannot resolve executable path.\n");
        return 2;
    }
    const fs::path exe_path(module_buf);

    if (argc == 4 && std::wcscmp(argv[1], L"--child") == 0) {
        return RunChild(argv[2], fs::path(argv[3]), exe_path);
    }

    fs::path dll_dir = exe_path.parent_path();
    fs::path entry_dll_path;
    fs::path sidecar_dll_path;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--dll-dir") == 0 && i + 1 < argc) {
            dll_dir = argv[++i];
        } else if (std::wcscmp(argv[i], L"--entry-dll") == 0 && i + 1 < argc) {
            entry_dll_path = argv[++i];
        } else if (std::wcscmp(argv[i], L"--sidecar-dll") == 0 && i + 1 < argc) {
            sidecar_dll_path = argv[++i];
        } else if (argv[i][0] != L'-') {
            dll_dir = argv[i];
        }
    }
    if (entry_dll_path.empty()) entry_dll_path = dll_dir / L"crash_test_entry_dll.dll";
    if (sidecar_dll_path.empty()) sidecar_dll_path = dll_dir / L"crash_test_sidecar_dll.dll";

    Check(GetModuleHandleW(L"user32.dll") == nullptr && GetModuleHandleW(L"gdi32.dll") == nullptr,
          "test runner has neither USER32.dll nor GDI32.dll loaded");

    const std::string exe_bytes = ReadBytes(exe_path);
    const std::string entry_bytes = ReadBytes(entry_dll_path);
    const std::string sidecar_bytes = ReadBytes(sidecar_dll_path);
    Check(!exe_bytes.empty() && !entry_bytes.empty() && !sidecar_bytes.empty(),
          "located native_crash_attribution_test.exe, crash_test_entry_dll.dll, and crash_test_sidecar_dll.dll");
    if (entry_bytes.empty() || sidecar_bytes.empty()) {
        std::printf("%d passed, %d failed\n", g_passed, g_failed);
        return 1;
    }

    std::string forbidden_dll;
    Check(!PeImportsForbiddenGuiDll(exe_bytes, forbidden_dll) &&
              !PeImportsForbiddenGuiDll(entry_bytes, forbidden_dll) &&
              !PeImportsForbiddenGuiDll(sidecar_bytes, forbidden_dll),
          "static PE import tables of test runner, entry DLL, and sidecar DLL reference zero USER32/GDI32 symbols");

    const fs::path suite_root = exe_path.parent_path() /
        (L"crash-attr-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    ScopedDirectoryCleanup cleanup{suite_root};
    std::error_code ec;
    fs::create_directories(suite_root, ec);

    // Scenario 1: Entry DLL crash during activation (ActivationScope + CallScope).
    {
        const fs::path data_root = suite_root / L"s1";
        StoreFixtureInfo fixture;
        Check(WriteStorePackage(data_root, entry_bytes, sidecar_bytes, fixture),
              "Scenario 1: constructed valid isolated store with entry + sidecar SHA-256");

        const DWORD exit_code = SpawnChildMode(exe_path, L"entry_activation", data_root);
        Check(exit_code == kExpectedCrashExitCode,
              "Scenario 1: child terminated with real SEH EXCEPTION_ACCESS_VIOLATION during entry activation");

        const fs::path activating_file = data_root / L"extensions" / L"native-activating.txt";
        const std::string expected_record = std::string(kPackageId) + "\t" + fixture.entry_sha256 + "\n";
        Check(fs::exists(activating_file) && ReadBytes(activating_file) == expected_record,
              "Scenario 1: native-activating.txt preserved crashing package id and entry SHA-256");

        const auto next = SimulateNextStartup(exe_path, data_root);
        const fs::path disabled_file = data_root / L"extensions" / L"crash-disabled.txt";
        Check(next.startup_ok && next.package_count == 1 && !next.package_enabled,
              "Scenario 1: next startup automatically disables package that crashed during activation");
        Check(next.newly_disabled.size() == 1 && next.newly_disabled[0] == kPackageNameWide,
              "Scenario 1: ApplyAtStartup returns one-time disabled notification containing package display name");
        Check(fs::exists(disabled_file) && ReadBytes(disabled_file) == expected_record && !fs::exists(activating_file),
              "Scenario 1: crash-disabled.txt persists id\\tsha256 and cleans up native-activating.txt");

        const auto repeat = SimulateNextStartup(exe_path, data_root);
        Check(repeat.startup_ok && !repeat.package_enabled && repeat.newly_disabled.empty(),
              "Scenario 1: subsequent startup keeps package disabled without repeating notification");
    }

    // Scenario 2: Declared private sidecar DLL runtime crash + re-enable upon SHA-256 upgrade.
    {
        const fs::path data_root = suite_root / L"s2";
        StoreFixtureInfo fixture;
        Check(WriteStorePackage(data_root, entry_bytes, sidecar_bytes, fixture),
              "Scenario 2: constructed isolated store for declared sidecar runtime crash");

        const DWORD exit_code = SpawnChildMode(exe_path, L"sidecar_runtime", data_root);
        Check(exit_code == kExpectedCrashExitCode,
              "Scenario 2: child verified FindAttributedPackage(&TriggerSidecarCrash) and faulted inside sidecar DLL");

        const fs::path activating_file = data_root / L"extensions" / L"native-activating.txt";
        const fs::path crashed_file = data_root / L"extensions" / L"native-crashed.txt";
        const std::string expected_record = std::string(kPackageId) + "\t" + fixture.entry_sha256 + "\n";
        Check(!fs::exists(activating_file) && fs::exists(crashed_file) && ReadBytes(crashed_file) == expected_record,
              "Scenario 2: lock-free crash handler wrote native-crashed.txt with package id and entry SHA-256");

        const std::string reports = ReadCrashDiagnosticReports(data_root);
        Check(reports.find("\"extension\":\"crash.test.pkg\"") != std::string::npos,
              "Scenario 2: crash report JSON attributed fault directly to extension package");

        const auto next = SimulateNextStartup(exe_path, data_root);
        const fs::path disabled_file = data_root / L"extensions" / L"crash-disabled.txt";
        Check(next.startup_ok && next.package_count == 1 && !next.package_enabled &&
                  next.newly_disabled.size() == 1 && next.newly_disabled[0] == kPackageNameWide &&
                  fs::exists(disabled_file) && ReadBytes(disabled_file) == expected_record &&
                  !fs::exists(crashed_file),
              "Scenario 2: next startup auto-disables package after sidecar crash and persists crash-disabled.txt");

        std::string upgraded_entry_bytes = entry_bytes;
        upgraded_entry_bytes.append("UPG2", 4);
        StoreFixtureInfo upgraded_fixture;
        Check(WriteStorePackage(data_root, upgraded_entry_bytes, sidecar_bytes, upgraded_fixture) &&
                  upgraded_fixture.entry_sha256 != fixture.entry_sha256,
              "Scenario 2: upgraded store package to a new valid PE build with a distinct SHA-256");

        const auto after_upgrade = SimulateNextStartup(exe_path, data_root);
        Check(after_upgrade.startup_ok && after_upgrade.package_count == 1 &&
                  after_upgrade.package_enabled && after_upgrade.newly_disabled.empty() &&
                  !fs::exists(disabled_file),
              "Scenario 2: ApplyAtStartup clears stale crash-disabled record and enables upgraded package build");
    }

    // Scenario 3: Host crash outside all plugin modules while CallScope is active (diagnostic clue only, no false disable).
    {
        const fs::path data_root = suite_root / L"s3";
        StoreFixtureInfo fixture;
        Check(WriteStorePackage(data_root, entry_bytes, sidecar_bytes, fixture),
              "Scenario 3: constructed isolated store for host fault outside plugin modules");

        const DWORD exit_code = SpawnChildMode(exe_path, L"host_outside_modules", data_root);
        Check(exit_code == kExpectedCrashExitCode,
              "Scenario 3: child faulted inside host executable address range while CallScope was active");

        const fs::path activating_file = data_root / L"extensions" / L"native-activating.txt";
        const fs::path crashed_file = data_root / L"extensions" / L"native-crashed.txt";
        const fs::path disabled_file = data_root / L"extensions" / L"crash-disabled.txt";
        Check(!fs::exists(activating_file) && !fs::exists(crashed_file),
              "Scenario 3: native-crashed.txt is NOT written for faults outside plugin module images");

        const std::string reports = ReadCrashDiagnosticReports(data_root);
        Check(reports.find("\"extension\":\"\"") != std::string::npos &&
                  reports.find("ActiveExtensionCallClue: crash.test.pkg") != std::string::npos,
              "Scenario 3: crash diagnostics record ActiveExtensionCallClue package id without attributing module ownership");

        const auto next = SimulateNextStartup(exe_path, data_root);
        Check(next.startup_ok && next.package_count == 1 && next.package_enabled &&
                  next.newly_disabled.empty() && !fs::exists(disabled_file),
              "Scenario 3: next startup ApplyAtStartup keeps innocent extension enabled without false auto-disable");
    }

    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
