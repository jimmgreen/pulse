#include "../app/folder_statistics.h"
#include "../fs/bounded_enumeration.h"
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <thread>
#include <vector>
#include <winioctl.h>

using namespace pulse;
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
void File(const std::wstring& path, DWORD bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("fixture create failed");
    std::vector<char> data(bytes, 'a');
    DWORD written = 0;
    const bool ok = WriteFile(file, data.data(), bytes, &written, nullptr) && written == bytes;
    CloseHandle(file);
    if (!ok) throw std::runtime_error("fixture write failed");
}
auto Deadline(DWORD ms = 8000) {
    return [until = GetTickCount64() + ms] { return GetTickCount64() >= until; };
}
app::FolderStatistics Verified(app::FolderStatisticsService& service, const std::wstring& path) {
    auto cancelled = Deadline();
    auto value = service.Read(path, cancelled);
    while (!value.verified) value = service.Read(path, cancelled, value.version);
    return value;
}

bool MakeJunction(const std::wstring& link, const std::wstring& target) {
    const auto substitute = L"\\??\\" + target;
    struct MountPoint {
        DWORD tag;
        WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
        wchar_t path[2 * MAX_PATH + 4];
    } data{};
    if (substitute.size() + target.size() + 2 > std::size(data.path) ||
        !CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
    data.print_length = static_cast<WORD>(target.size() * sizeof(wchar_t));
    data.length = static_cast<WORD>(8 + data.print_offset + data.print_length + sizeof(wchar_t));
    std::memcpy(data.path, substitute.c_str(), data.print_offset);
    std::memcpy(reinterpret_cast<BYTE*>(data.path) + data.print_offset, target.c_str(),
        data.print_length + sizeof(wchar_t));
    DWORD written = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, data.length + 8,
        nullptr, 0, &written, nullptr) != FALSE;
    CloseHandle(handle);
    return ok;
}

void LinkedDirectories(const std::wstring& parent, const std::wstring& target) {
    const auto junction = parent + L"\\junction";
    const bool junction_created = MakeJunction(junction, target);
    Check(junction_created, "create an NTFS junction within the fixture");
    if (!junction_created) return;
    struct RemoveJunction {
        const std::wstring& path;
        ~RemoveJunction() { RemoveDirectoryW(path.c_str()); }
    } remove_junction{junction};

    app::FolderStatisticsService service;
    const auto parent_value = service.Read(parent, Deadline());
    const auto target_value = service.Read(target, Deadline());
    const auto junction_value = service.Read(junction, Deadline());
    const auto trailing_junction_value = service.Read(junction + L"\\", Deadline());
    std::printf("  bytes: parent=%llu target=%llu junction=%llu trailing_junction=%llu junction_files=%llu\n",
        parent_value.bytes, target_value.bytes, junction_value.bytes, trailing_junction_value.bytes, junction_value.files);
    Check(parent_value.bytes == 18 && parent_value.files == 2, "nested junction does not add target bytes or files again");
    Check(target_value.bytes == 11 && target_value.files == 1 && !target_value.partial,
        "directly selected ordinary directory still counts its content");
    Check(junction_value.bytes == 0 && junction_value.files == 0 && junction_value.directories == 0 &&
        junction_value.partial && !junction_value.verified, "directly selected junction does not traverse its target");
    Check(trailing_junction_value.bytes == 0 && trailing_junction_value.partial,
        "trailing separator does not make the junction traversable");

    const auto directory_link = parent + L"\\directory-symlink";
    if (CreateSymbolicLinkW(directory_link.c_str(), target.c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        RemoveJunction remove_directory_link{directory_link};
        const auto value = service.Read(directory_link, Deadline());
        std::printf("  bytes: directory_symlink=%llu\n", value.bytes);
        Check(value.bytes == 0 && value.files == 0 && value.partial,
            "directly selected directory symlink does not traverse its target");
    } else std::printf("[SKIP] directory symlink creation failed: Win32=%lu\n", GetLastError());

    const auto file_link = parent + L"\\file-symlink";
    const auto source_file = target + L"\\two";
    if (CreateSymbolicLinkW(file_link.c_str(), source_file.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        struct RemoveFileLink {
            const std::wstring& path;
            ~RemoveFileLink() { DeleteFileW(path.c_str()); }
        } remove_file_link{file_link};
        app::FolderStatisticsService fresh;
        const auto value = fresh.Read(parent, Deadline());
        std::printf("  bytes: parent_with_file_symlink=%llu\n", value.bytes);
        Check(value.bytes == 18, "file symlink does not add the target's bytes");
    } else std::printf("[SKIP] file symlink creation failed: Win32=%lu\n", GetLastError());
}
}
int main(int argc, char** argv) {
    const bool cold_only = argc == 2 && std::strcmp(argv[1], "--cold-start-only") == 0;
    const bool links_only = argc == 2 && std::strcmp(argv[1], "--links-only") == 0;
    const auto root = std::filesystem::absolute(L"bench_data").wstring() + L"\\statistics_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    std::filesystem::create_directories(root + L"\\A\\child");
    std::filesystem::create_directories(root + L"\\B");
    const auto a = root + L"\\A", b = root + L"\\B";
    File(a + L"\\one", 7);
    File(a + L"\\child\\two", 11);
    try {
        if (!cold_only) LinkedDirectories(a, a + L"\\child");
        if (links_only) {
            std::filesystem::remove_all(root);
            return failures ? 1 : 0;
        }
        {
            constexpr unsigned attempts = 8;
            uint64_t scans = 0;
            unsigned single_scan = 0;
            bool correct = true;
            for (unsigned i = 0; i < attempts; ++i) {
                app::FolderStatisticsService service;
                const auto start = std::chrono::steady_clock::now();
                const auto value = Verified(service, a);
                const auto count = service.ScansStarted();
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count();
                std::printf("  cold sample=%u scans=%llu ready_us=%lld\n", i + 1, count, elapsed);
                scans += count;
                single_scan += count == 1;
                correct = correct && value.verified && value.bytes == 18 && value.files == 2;
            }
            std::printf("  cold watcher startup: attempts=%u scans=%llu single_scan=%u\n", attempts, scans, single_scan);
            // Arming may legitimately miss the bounded budget on a busy or
            // slow machine. Report the scan count without weakening coverage.
            Check(correct, "cold root/parent watcher handshake preserves verified statistics");
        }
        if (cold_only) {
            std::filesystem::remove_all(root);
            return failures ? 1 : 0;
        }
        {
            bool cancelled = false;
            unsigned checks = 0;
            app::FolderStatisticsService service([](const auto&, const auto&) { return app::FolderStatistics{}; });
            const auto start = GetTickCount64();
            // Third check occurs after task creation while Read still holds
            // the entry lock: cancellation cannot race the 20ms arm budget.
            try { service.Read(root + L"\\not-armed", [&] { return ++checks >= 3; }); }
            catch (const fs::EnumerationCancelled&) { cancelled = true; }
            Check(cancelled && GetTickCount64() - start < 500 && service.ScansStarted() == 0,
                "cancelling an unarmed subscription exits before starting a provider");
        }
        {
            app::FolderStatisticsService service;
            auto initial = Verified(service, a);
            Check(initial.files == 2 && initial.directories == 1 && initial.bytes == 18 && !initial.partial,
                "recursive statistics match physical fixture");
            Verified(service, b);
            const auto scans = service.ScansStarted();
            auto cached = service.Read(a, Deadline());
            Check(cached.version == initial.version && service.ScansStarted() == scans,
                "A to B to A reuses verified revision without scanning");
            try { service.Read(a, Deadline(2300), cached.version); }
            catch (const fs::EnumerationCancelled&) {}
            Check(service.ScansStarted() == scans, "stable verified subscription does not periodically rescan");
            File(a + L"\\child\\three", 5);
            auto changed = service.Read(a, Deadline(), cached.version);
            Check(changed.files == 3 && changed.bytes == 23, "recursive watcher invalidates nested mutation");
            if (!MoveFileW(a.c_str(), (root + L"\\oldA").c_str())) throw std::runtime_error("fixture rename failed");
            std::filesystem::create_directories(a);
            File(a + L"\\replacement", 3);
            const auto replacement_deadline = Deadline();
            auto replaced = service.Read(a, replacement_deadline, changed.version);
            while (replaced.files != 1 || replaced.bytes != 3)
                replaced = service.Read(a, replacement_deadline, replaced.version);
            Check(replaced.files == 1 && replaced.bytes == 3, "root rename and replacement never reuse old directory totals");
        }
        {
            std::atomic_bool release{false}, entered{false}, cancel_first{false};
            app::FolderStatisticsService service([&](const auto&, const auto&) {
                entered = true;
                while (!release) Sleep(1);
                app::FolderStatistics result; result.bytes = 17; return result;
            });
            std::atomic_bool first_cancelled{false}, second_done{false};
            std::atomic<unsigned> second_checks{0};
            std::thread first([&] {
                try { service.Read(b, [&] { return cancel_first.load(); }); }
                catch (const fs::EnumerationCancelled&) { first_cancelled = true; }
            });
            while (!entered) Sleep(1);
            std::thread second([&] {
                const auto deadline = Deadline();
                try { second_done = service.Read(b, [&] { ++second_checks; return deadline(); }).bytes == 17; } catch (...) {}
            });
            while (second_checks < 2) Sleep(1);
            cancel_first = true;
            const auto start = GetTickCount64();
            first.join();
            Check(first_cancelled && GetTickCount64() - start < 500,
                "one window detaches promptly while another remains subscribed");
            release = true;
            second.join();
            Check(second_done && service.ScansStarted() == 1, "two windows share one physical statistics scan");
        }
        {
            app::FolderStatisticsService partial([](const auto&, const auto&) {
                app::FolderStatistics result; result.partial = true; return result;
            });
            auto first = partial.Read(b, Deadline());
            auto second = partial.Read(b, Deadline());
            Check(first.partial && !first.verified && second.version != first.version,
                "partial counts never become verified cache hits");
            app::FolderStatisticsService unwatched([](const auto&, const auto&) { return app::FolderStatistics{}; });
            auto unknown = unwatched.Read(root + L"\\missing", Deadline());
            Check(!unknown.verified, "unarmed watcher cannot verify results");
            app::FolderStatisticsService missing;
            Check(missing.Read(root + L"\\missing", Deadline()).partial, "unreadable root is explicitly partial");
            File(b + L"\\offline", 9);
            const bool marked = SetFileAttributesW((b + L"\\offline").c_str(), FILE_ATTRIBUTE_OFFLINE) != FALSE;
            app::FolderStatisticsService cloud;
            const auto placeholder = cloud.Read(b, Deadline());
            Check(marked && placeholder.partial && !placeholder.verified,
                "offline placeholder metadata never claims complete verified statistics");
            SetFileAttributesW((b + L"\\offline").c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW((b + L"\\offline").c_str());
        }
        {
            std::atomic_bool release{false}, entered{false}, cancel{false};
            const auto before = fs::active_local_enumerations.load();
            auto service = std::make_unique<app::FolderStatisticsService>([&](const auto&, const auto&) {
                entered = true;
                while (!release) Sleep(1);
                return app::FolderStatistics{};
            });
            std::thread reader([&] { try { service->Read(b, [&] { return cancel.load(); }); } catch (...) {} });
            while (!entered) Sleep(1);
            cancel = true;
            reader.join();
            const auto start = GetTickCount64();
            service.reset();
            Check(GetTickCount64() - start < 500 && fs::active_local_enumerations > before,
                "shutdown detaches but retired provider remains charged");
            release = true;
            const auto until = GetTickCount64() + 3000;
            while (fs::active_local_enumerations > before && GetTickCount64() < until) Sleep(1);
            Check(fs::active_local_enumerations == before, "retired statistics provider releases quota on actual exit");
        }
    } catch (const std::exception& error) {
        std::printf("[FAIL] %s\n", error.what()); ++failures;
    }
    // This unique fixture root is created solely by this test.
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
