#include "../../app/extensions/native/module.h"
#include "../../app/extensions/package_revision.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>
namespace {
struct Counters { uint32_t size, version; int32_t* values; };
PulseStatus PULSE_CALL Query(void* context, PulseStringView name, uint32_t version, void* out, uint32_t size) {
    if (version != 1 || size != sizeof(Counters) || name.length != 13 || std::memcmp(name.data, "test.counters", 13)) return PULSE_ERROR_UNAVAILABLE;
    *static_cast<Counters*>(out) = {sizeof(Counters), 1, static_cast<int32_t*>(context)};
    return PULSE_OK;
}
struct DependencyFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::path("bench_data") /
        ("native-loader-dependency-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    explicit DependencyFixture(const std::filesystem::path& source) {
        if (!std::filesystem::create_directories(root)) throw std::runtime_error("Cannot create native dependency fixture");
        std::filesystem::copy_file(source / "native_with_dependency.dll", root / "plugin.dll");
        std::filesystem::copy_file(source / "native_private_dependency.dll", root / "native_private_dependency.dll");
    }
    ~DependencyFixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; };
    const auto path = [&](const wchar_t* name) { return (std::filesystem::path(argv[1]) / name).wstring(); };
    int32_t counts[3]{};
    const PulseHostV1 host{sizeof(PulseHostV1), 1, counts, Query};
    pulse::extensions::native::Module module;
    check(module.Load(L"relative.dll", host) == PULSE_ERROR_ARGUMENT, "relative DLL rejected");
    check(module.Load(path(L"native_good.dll"), host) == PULSE_OK, "real independently linked DLL loaded");
    check(module.Activate() == PULSE_OK && module.Activate() == PULSE_OK && counts[0] == 1, "activation is idempotent");
    PulseStatus other = PULSE_OK;
    std::thread wrong([&] { other = module.Deactivate(); }); wrong.join();
    check(other == PULSE_ERROR_STATE && module.Active(), "non-owner thread cannot change lifecycle");
    check(module.Deactivate() == PULSE_OK && module.Deactivate() == PULSE_OK && counts[1] == 1, "repeated deactivation occurs once");
    check(module.Close() == PULSE_OK && module.Close() == PULSE_OK && counts[2] == 1, "destruction occurs once in DLL CRT");
    check(module.Load(path(L"native_bad_abi.dll"), host) == PULSE_ERROR_ABI && counts[2] == 2 && !module.Loaded(), "incompatible ABI cleans initialized instance");
    check(module.Load(path(L"native_missing.dll"), host) == PULSE_ERROR_ENTRY && !module.Loaded(), "missing entry export rejected");
    const int32_t destroyed_before_preview = counts[2];
    check(module.Load(path(L"native_no_preview.dll"), host) == PULSE_ERROR_SDK_PREVIEW && !module.Loaded() &&
        counts[2] == destroyed_before_preview, "plugin without SDK preview refused before its entry runs");
    check(module.Load(path(L"native_old_preview.dll"), host) == PULSE_ERROR_SDK_PREVIEW && !module.Loaded() &&
        counts[2] == destroyed_before_preview, "old SDK preview refused before entry runs");
    check(module.Load(path(L"native_init_fail.dll"), host) == PULSE_ERROR_INITIALIZATION && counts[2] == 3 && !module.Loaded(), "initialization failure destroys partial instance");
    check(module.Load(path(L"native_good.dll"), host) == PULSE_OK && module.Activate() == PULSE_OK && module.Close() == PULSE_OK && counts[1] == 2 && counts[2] == 4, "active close deactivates before destruction");
    const auto image_path = path(L"native_good.dll");
    std::ifstream image(std::filesystem::path(image_path), std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(image), std::istreambuf_iterator<char>()};
    image.close();
    const auto digest = pulse::extensions::PackageRevision(bytes);
    check(!bytes.empty() && digest.size() == 64, "fixture digest computed from complete native image");
    check(module.LoadVerified(image_path, std::string(64, '0'), host) == PULSE_ERROR_LOAD && !module.Loaded(),
        "digest mismatch rejects image before loading");
    check(module.LoadVerified(image_path, digest, host) == PULSE_OK && module.Activate() == PULSE_OK,
        "verified native image loads and activates");
    HANDLE writer = CreateFileW(image_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
        "verified image denies write access while active");
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    HANDLE remover = CreateFileW(image_path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(remover == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
        "verified image denies deletion while active");
    if (remover != INVALID_HANDLE_VALUE) CloseHandle(remover);
    pulse::extensions::native::Module second;
    int32_t second_counts[3]{};
    const PulseHostV1 second_host{sizeof(PulseHostV1), 1, second_counts, Query};
    check(second.LoadVerified(image_path, digest, second_host) == PULSE_OK && second.Activate() == PULSE_OK,
        "same image supports separate active host instances");
    check(module.Close() == PULSE_OK && !module.Loaded() && second.Active() && second_counts[1] == 0 && second_counts[2] == 0,
        "closing one instance leaves another instance active");
    check(second.Close() == PULSE_OK && second_counts[1] == 1 && second_counts[2] == 1,
        "second instance deactivates and destroys independently");
    writer = CreateFileW(image_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(writer != INVALID_HANDLE_VALUE, "last unload releases verified image file lock");
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    try {
        DependencyFixture fixture{std::filesystem::path(argv[1])};
        const auto dependent_path = (fixture.root / "plugin.dll").wstring();
        std::ifstream dependent(std::filesystem::path(dependent_path), std::ios::binary);
        const std::string dependent_bytes{std::istreambuf_iterator<char>(dependent), std::istreambuf_iterator<char>()};
        dependent.close();
        const auto dependent_digest = pulse::extensions::PackageRevision(dependent_bytes);
        check(!dependent_bytes.empty() && dependent_digest.size() == 64, "dependent native fixture has a complete image digest");
        check(module.LoadVerified(dependent_path, dependent_digest, host) == PULSE_ERROR_LOAD && !module.Loaded() &&
            !GetModuleHandleW(L"native_private_dependency.dll"), "verified package rejects undeclared adjacent dependency before entry");
        writer = CreateFileW(dependent_path.c_str(), GENERIC_WRITE | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(writer != INVALID_HANDLE_VALUE, "dependency load failure releases verified file write and delete lock");
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        HANDLE directory = CreateFileW(fixture.root.c_str(), DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        check(directory != INVALID_HANDLE_VALUE, "dependency load failure releases verified ancestor directory lock");
        if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
        check(module.Load(dependent_path, host) == PULSE_OK && module.Activate() == PULSE_OK &&
            GetModuleHandleW(L"native_private_dependency.dll"), "explicit development loader preserves adjacent dependency compatibility");
        check(module.Close() == PULSE_OK && !GetModuleHandleW(L"native_private_dependency.dll"),
            "development loader unloads dependent fixture before verified retry");
        check(module.LoadVerified(dependent_path, dependent_digest, host) == PULSE_ERROR_LOAD && !module.Loaded(),
            "verified retry never inherits development dependency search flags");
        module.Close();
        const auto hash_file = [](const std::filesystem::path& file) {
            std::ifstream input(file, std::ios::binary);
            return pulse::extensions::PackageRevision(std::string{std::istreambuf_iterator<char>(input), {}});
        };
        std::ofstream(fixture.root / "License.txt") << "fixture license";
        pulse::extensions::NativeRuntime package;
        package.library = "plugin.dll"; package.sha256 = dependent_digest;
        package.files = {{"native_private_dependency.dll", hash_file(fixture.root / "native_private_dependency.dll")},
                         {"License.txt", hash_file(fixture.root / "License.txt")}};
        check(module.LoadVerified(dependent_path, package, host) == PULSE_OK && module.Activate() == PULSE_OK,
            "fully verified package loads its declared private DLL");
        writer = CreateFileW((fixture.root / "License.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, 0, nullptr);
        check(writer == INVALID_HANDLE_VALUE, "package resource stays locked until unload");
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        module.Close();
        std::ofstream(fixture.root / "License.txt") << "tampered license";
        check(module.LoadVerified(dependent_path, package, host) == PULSE_ERROR_LOAD && !module.Loaded(),
            "tampered non-PE resource prevents plugin entry");
        std::ofstream(fixture.root / "License.txt") << "fixture license";
        std::filesystem::remove(fixture.root / "native_private_dependency.dll");
        check(module.LoadVerified(dependent_path, package, host) == PULSE_ERROR_LOAD && !module.Loaded(),
            "missing declared dependency rejects complete package");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[FAIL] dependency fixture: %s\n", error.what());
        ++failures;
    }
    return failures ? 1 : 0;
}
