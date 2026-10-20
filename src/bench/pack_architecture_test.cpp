// Path-only regression: placeholder tools are never executed or loaded.
#include "../common/pack_architecture.h"
#include "../app/pack_catalog.h"
#include "../common/preview_packs.h"
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string_view>
#include <system_error>

namespace {
using namespace pulse;
using namespace pulse::packs;

static_assert(std::wstring_view(ArchitectureName(Architecture::X64)) == L"x64");
static_assert(std::wstring_view(ArchitectureName(Architecture::Arm64)) == L"arm64");
static_assert(ProcessMachine(Architecture::X64) == IMAGE_FILE_MACHINE_AMD64);
static_assert(ProcessMachine(Architecture::Arm64) == IMAGE_FILE_MACHINE_ARM64);
#if defined(_M_ARM64) || defined(__aarch64__)
static_assert(ProcessArchitecture() == Architecture::Arm64);
static_assert(ProcessMachine() == IMAGE_FILE_MACHINE_ARM64);
static_assert(&app::kMediaPackRelease == &app::kMediaPackArm64Release);
static_assert(&app::kImagePackRelease == &app::kImagePackArm64Release);
static_assert(&app::kRawPackRelease == &app::kRawPackArm64Release);
static_assert(&app::kArchivePackRelease == &app::kArchivePackArm64Release);
#elif defined(_M_X64) || defined(__x86_64__)
static_assert(ProcessArchitecture() == Architecture::X64);
static_assert(ProcessMachine() == IMAGE_FILE_MACHINE_AMD64);
#else
#error Pack architecture tests require an x64 or ARM64 process target
#endif

int g_failed = 0;
bool Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++g_failed;
        std::printf("  last Win32 error: %lu\n", GetLastError());
    }
    return ok;
}

class IsolatedProfile {
public:
    ~IsolatedProfile() { if (!root_.empty()) Cleanup(); }

    bool Start() {
        wchar_t buffer[32768];
        const DWORD previous_count = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
                                                             static_cast<DWORD>(std::size(buffer)));
        if (previous_count >= std::size(buffer)) return false;
        if (previous_count) previous_.assign(buffer, previous_count);
        const DWORD temp_count = GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
        if (!temp_count || temp_count >= std::size(buffer)) return false;
        const std::wstring prefix = std::wstring(buffer, temp_count) + L"pulse-pack-architecture-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-";
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const std::wstring candidate = prefix + std::to_wstring(attempt);
            if (CreateDirectoryW(candidate.c_str(), nullptr)) { root_ = candidate; break; }
            if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
        }
        if (root_.empty()) return false;
        redirected_ = SetEnvironmentVariableW(L"LOCALAPPDATA", root_.c_str()) != FALSE;
        return redirected_;
    }

    bool Cleanup() {
        bool restored = true;
        if (redirected_) {
            restored = SetEnvironmentVariableW(L"LOCALAPPDATA", previous_.empty() ? nullptr : previous_.c_str()) != FALSE;
            if (restored) redirected_ = false;
        }
        std::error_code error;
        // root_ is only assigned after exclusively creating this temporary directory.
        if (!root_.empty()) std::filesystem::remove_all(root_, error);
        if (!error && restored) root_.clear();
        return restored && !error;
    }

    const std::wstring& Root() const { return root_; }

private:
    std::wstring root_;
    std::wstring previous_;
    bool redirected_ = false;
};

bool WriteFixture(const std::wstring& path, const std::wstring& text) {
    return CreateDirectoryChain(DirectoryOf(path)) && WriteUtf8FileAtomic(path, text);
}

bool SetWriteTime(const std::wstring& path, uint64_t value) {
    const HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const FILETIME time{static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)};
    const bool ok = SetFileTime(file, nullptr, nullptr, &time) != FALSE;
    CloseHandle(file);
    return ok;
}

constexpr uint64_t kFixtureTime = 132537600000000000ull;

std::wstring Manifest(Architecture architecture, PackId id) {
    return PacksRoot(architecture) + L"\\" + PackKey(id) + L"\\installed.json";
}

std::wstring VersionDirectory(Architecture architecture, PackId id, const wchar_t* version) {
    return PacksRoot(architecture) + L"\\" + PackKey(id) + L"\\" + version;
}

bool SeedPack(Architecture architecture, PackId id, const wchar_t* version) {
    const std::wstring directory = VersionDirectory(architecture, id, version);
    const std::wstring marker = L"{\"version\":\"" + std::wstring(version) + L"\",\"dir\":\"" + version + L"\"}";
    return WriteFixture(directory + L"\\" + PackMainTool(id), L"placeholder, never executed") &&
        WriteFixture(directory + L"\\fixture.dll", L"placeholder, never loaded") &&
        WriteFixture(Manifest(architecture, id), marker);
}

void CatalogTests() {
    const app::PackRelease* releases[] = {&app::kMediaPackRelease, &app::kImagePackRelease,
                                         &app::kRawPackRelease, &app::kArchivePackRelease};
    for (uint32_t index = 0; index < kPackCount; ++index) {
        const auto& release = *releases[index];
        Check(std::wstring_view(release.key) == PackKey(static_cast<PackId>(index)), "catalog key agrees with native pack routing");
        if (!release.file_count) {
            Check(!release.files && !*release.version && !*release.base_url,
                  "unpublished native packs stay unavailable instead of falling back to another architecture");
            continue;
        }
        Check(release.files && PackUpdateAvailable(L"", release.version), "published native catalog has files and a usable version");
        const bool arm_url = std::wstring_view(release.base_url).find(L"-arm64/") != std::wstring_view::npos;
        Check(arm_url == (ProcessArchitecture() == Architecture::Arm64), "published catalog URL belongs to the compiled process architecture");
    }
}

void PathAndSettingsTests(const std::wstring& profile) {
    const std::wstring shared = profile + L"\\Pulse\\packs";
    Check(PacksSharedRoot() == shared, "shared root preserves the existing path");
    Check(PacksRoot(Architecture::X64) == shared, "x64 root preserves the existing path");
    Check(PacksRoot(Architecture::Arm64) == shared + L"\\arm64", "ARM64 root is isolated from legacy x64 packs");
    Check(PacksRoot() == PacksRoot(ProcessArchitecture()), "default root follows the compiled process architecture");
    Check(std::wstring_view(ArchitectureName()) ==
        (ProcessArchitecture() == Architecture::Arm64 ? L"arm64" : L"x64"), "default architecture name matches the process");
    Check(PacksSettingsPath() == shared + L"\\packs.json", "settings stay in the architecture-independent legacy location");

    PackSettings settings;
    settings.enabled[0] = false;
    settings.enabled[2] = false;
    settings.use_custom_ffmpeg = true;
    settings.custom_ffmpeg = profile + L"\\用户工具\\ffmpeg.exe";
    settings.auto_update = false;
    settings.download_source = 2;
    settings.remove_on_uninstall = false;
    Check(SavePackSettings(settings), "first settings save creates the shared parent directory");
    Check(IsRegularFile(shared + L"\\packs.json"), "first settings save writes the shared file");
    Check(GetFileAttributesW((shared + L"\\arm64").c_str()) == INVALID_FILE_ATTRIBUTES,
        "saving shared settings does not create an architecture-specific pack root");
    Check(SerializePackSettings(LoadPackSettings()) == SerializePackSettings(settings),
        "enabled flags, custom FFmpeg, update, source and uninstall preferences survive round trip");

    PackSettings shadow;
    for (bool& enabled : shadow.enabled) enabled = false;
    shadow.custom_ffmpeg = L"must not be read";
    const std::wstring shadow_path = PacksRoot(Architecture::Arm64) + L"\\packs.json";
    Check(WriteFixture(shadow_path, SerializePackSettings(shadow)), "create conflicting ARM-local settings fixture");
    Check(SerializePackSettings(LoadPackSettings()) == SerializePackSettings(settings),
        "ARM-local settings cannot replace existing shared preferences");
    Check(SavePackSettings(PackSettings{}), "restore shared enabled defaults for resolution fixtures");
}

void IsolationTests() {
    constexpr bool native_x64 = ProcessArchitecture() == Architecture::X64;
    for (uint32_t index = 0; index < kPackCount; ++index) {
        const PackId id = static_cast<PackId>(index);
        Check(SeedPack(Architecture::X64, id, L"legacy-x64"), "create legacy x64 pack manifest, executable and companion");
        const InstalledPack legacy = ReadInstalledPack(id, Architecture::X64);
        Check(legacy.present && legacy.version == L"legacy-x64" &&
            legacy.directory == VersionDirectory(Architecture::X64, id, L"legacy-x64"), "legacy x64 installation remains available to x64");
        Check(!ReadInstalledPack(id, Architecture::Arm64).present,
            "legacy x64 installed.json and tools are not an ARM64 installation");
        Check(ReadInstalledPack(id).present == native_x64, "default installed-pack lookup uses the process architecture");
        Check(ResolvePackUncached(id).source == (native_x64 ? ToolSource::Pack : ToolSource::None),
            "native resolution never falls back from ARM64 to legacy x64");
        const std::wstring expected_tool = native_x64 ? legacy.directory + L"\\" + PackMainTool(id) : std::wstring{};
        const std::wstring expected_dll = native_x64 ? legacy.directory + L"\\fixture.dll" : std::wstring{};
        Check(PackToolPath(id, PackMainTool(id)) == expected_tool && PackToolPath(id, L"fixture.dll") == expected_dll,
            "cached tool lookup cannot expose legacy x64 executable or DLL to ARM64");
    }

    for (uint32_t index = 0; index < kPackCount; ++index) {
        const PackId id = static_cast<PackId>(index);
        Check(SeedPack(Architecture::Arm64, id, L"native-arm64"), "create separate ARM64 installation fixture");
        Check(SetWriteTime(Manifest(Architecture::Arm64, id), kFixtureTime + 1000 + index),
            "give ARM64 installation marker a deterministic timestamp");
        const InstalledPack arm = ReadInstalledPack(id, Architecture::Arm64);
        Check(arm.present && arm.version == L"native-arm64" &&
            arm.directory == VersionDirectory(Architecture::Arm64, id, L"native-arm64"), "ARM64 reads only its own installation");
        Check(ReadInstalledPack(id, Architecture::X64).version == L"legacy-x64", "ARM64 installation leaves the legacy x64 marker unchanged");
        const std::wstring expected = VersionDirectory(ProcessArchitecture(), id, native_x64 ? L"legacy-x64" : L"native-arm64");
        Check(ResolvePackUncached(id).directory == expected && ResolvePack(id).directory == expected,
            "uncached and cached native lookup select the correct coexisting installation");
        Check(PackToolPath(id, PackMainTool(id)) == expected + L"\\" + PackMainTool(id) &&
            PackToolPath(id, L"fixture.dll") == expected + L"\\fixture.dll", "native executable and companion resolve inside one architecture root");
    }
}

void StampAndCacheTests(const std::wstring& profile) {
    const Architecture other = ProcessArchitecture() == Architecture::Arm64 ? Architecture::X64 : Architecture::Arm64;
    const std::wstring settings_path = PacksSettingsPath();
    const std::wstring native_marker = Manifest(ProcessArchitecture(), PackId::Media);
    const std::wstring foreign_marker = Manifest(other, PackId::Media);
    const std::wstring shadow_path = PacksRoot(Architecture::Arm64) + L"\\packs.json";
    Check(SetWriteTime(settings_path, kFixtureTime + 10) && SetWriteTime(native_marker, kFixtureTime + 20) &&
        SetWriteTime(foreign_marker, kFixtureTime + 30) && SetWriteTime(shadow_path, kFixtureTime + 40),
        "initialize deterministic settings and installation timestamps");
    const uint64_t before = PackStamp(PackId::Media);
    Check(SetWriteTime(foreign_marker, kFixtureTime + 31), "change only the other architecture installation timestamp");
    Check(PackStamp(PackId::Media) == before, "stamp ignores the other architecture installed.json");
    Check(SetWriteTime(shadow_path, kFixtureTime + 41), "change only ARM-local shadow settings timestamp");
    Check(PackStamp(PackId::Media) == before, "stamp ignores architecture-local shadow settings");
    Check(SetWriteTime(native_marker, kFixtureTime + 21), "change native installation timestamp");
    const uint64_t native_changed = PackStamp(PackId::Media);
    Check(native_changed != before, "stamp observes the native installed.json");
    Check(SetWriteTime(settings_path, kFixtureTime + 11), "change shared settings timestamp");
    Check(PackStamp(PackId::Media) != native_changed, "stamp observes shared settings on both process architectures");

    Check(ResolvePack(PackId::Media).source == ToolSource::Pack, "prime native resolution cache");
    PackSettings settings = LoadPackSettings();
    settings.enabled[0] = false;
    Check(SavePackSettings(settings) && SetWriteTime(settings_path, kFixtureTime + 12), "disable a pack in shared settings");
    Check(ResolvePack(PackId::Media).source == ToolSource::None && PackToolPath(PackId::Media, L"ffmpeg.exe").empty(),
        "shared settings immediately invalidate cached native tool resolution");

    settings.enabled[0] = true;
    settings.use_custom_ffmpeg = true;
    settings.custom_ffmpeg = profile + L"\\custom tools\\ffmpeg.exe";
    settings.download_source = 2;
    settings.auto_update = false;
    settings.remove_on_uninstall = false;
    Check(WriteFixture(settings.custom_ffmpeg, L"explicit custom path, never executed"), "create isolated custom FFmpeg placeholder");
    Check(SavePackSettings(settings) && SetWriteTime(settings_path, kFixtureTime + 13), "save explicit custom FFmpeg in shared preferences");
    const ResolvedPack custom = ResolvePack(PackId::Media);
    Check(custom.source == ToolSource::Custom && custom.directory == DirectoryOf(settings.custom_ffmpeg),
        "shared custom FFmpeg preference remains effective on the native process");
    Check(SerializePackSettings(LoadPackSettings()) == SerializePackSettings(settings),
        "native pack lookup does not discard shared custom path or download preferences");
}

void MissingProfileTests(const std::wstring& profile) {
    if (Check(SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr) != FALSE, "temporarily remove LOCALAPPDATA inside the fixture process")) {
        Check(PacksSharedRoot().empty() && PacksRoot(Architecture::X64).empty() && PacksRoot(Architecture::Arm64).empty() &&
            PacksSettingsPath().empty(), "missing LOCALAPPDATA never produces a relative architecture or settings path");
        Check(!SavePackSettings(PackSettings{}) && !ReadInstalledPack(PackId::Media).present && PackStamp(PackId::Media) == 0,
            "missing LOCALAPPDATA prevents settings writes and native pack lookup");
    }
    Check(SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str()) != FALSE, "restore isolated LOCALAPPDATA before cleanup");
}
} // namespace

int wmain() {
    IsolatedProfile fixture;
    if (!Check(fixture.Start(), "create an exclusive temporary profile and redirect LOCALAPPDATA")) return 1;
    CatalogTests();
    PathAndSettingsTests(fixture.Root());
    IsolationTests();
    StampAndCacheTests(fixture.Root());
    MissingProfileTests(fixture.Root());
    Check(fixture.Cleanup(), "restore original LOCALAPPDATA and remove isolated fixtures");
    std::printf("%d failure(s)\n", g_failed);
    return g_failed ? 1 : 0;
}
