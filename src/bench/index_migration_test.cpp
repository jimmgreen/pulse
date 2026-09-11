#include "../index/index_migration.h"
#include "../common/command_line.h"
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace pulse::index;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    check(SameIndexLocation(L"C:\\ProgramData\\Pulse\\Index", L"c:/ProgramData/Pulse/Index/"),
        "unchanged location normalizes case and trailing separator");
    check(!SameIndexLocation(L"C:\\Index", L"D:\\Index") && !SameIndexLocation(L"", L""),
        "different or empty locations do not skip migration");
    for (const std::wstring value : {L"D:\\Folder With Space\\", L"D:\\中文\\索引", L"", L"embedded\\\"quote"}) {
        const auto command = L"helper.exe " + pulse::QuoteWindowsArgument(value);
        int count = 0;
        auto arguments = CommandLineToArgvW(command.c_str(), &count);
        check(arguments && count == 2 && arguments[1] == value, "elevated command path round trip");
        if (arguments) LocalFree(arguments);
    }
    const auto root = fs::absolute(fs::path("bench_data") / ("migration-" + std::to_string(GetCurrentProcessId())));
    const auto source = root / L"原索引", target = root / L"新索引";
    const fs::path base = L"v9/0123456789abcdef/base-a.bin";
    auto write = [](const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    };
    write(source / base, "base-data");
    write(source / L"Volumes/0123456789abcdef/wal-a.log", "latest-updates");
    write(source / L"v9/volumes/abcdef0123456789/manifest.json", "manifest");
    write(source / L"config.json", "keep-config");
    write(source / L"v9/0123456789abcdef/my-notes.txt", "keep-user-data");
    write(source / L"my-file.txt", "keep-user-data");
    std::wstring error;
    IndexMigration migration;
    check(!CopyIndexForMigration(source.wstring(), (source / L"nested").wstring(), migration, error), "nested target rejected");
    check(ResolveIndexMigrationTarget(L"D:\\") == L"D:\\Index", "drive root resolves to Index child");
    check(ResolveIndexMigrationTarget(L"D:/") == L"D:\\Index", "forward slash drive root resolves to Index child");
    check(ResolveIndexMigrationTarget(target.wstring()) == target.wstring(), "explicit destination preserved");
    check(ResolveIndexMigrationTarget(L"\\\\server\\share\\folder") == L"\\\\server\\share\\folder",
          "explicit UNC directory preserved");
    fs::create_directories(source / L"Volumes/1FE19AC33F476154");
    fs::create_directories(source / L"user-empty-folder");
    check(CopyIndexForMigration(source.wstring(), source.wstring() + L"\\", migration, error) && migration.files.empty(), "same normalized location is a no-op");
    check(CopyIndexForMigration(source.wstring(), target.wstring(), migration, error), "unicode destination migration copied and verified");
    check(migration.files.size() == 3, "only index artifacts copied");
    check(fs::exists(source / base) && fs::exists(target / base), "original retained before commit");
    IndexMigration invalid = migration;
    invalid.files[0].relative = L"..\\outside.bin";
    check(!RemoveMigratedIndexSource(invalid, error) && fs::exists(source / base), "cleanup refuses paths outside index root");
    IndexMigration collision;
    check(CopyIndexForMigration(source.wstring(), target.wstring(), collision, error),
          "verified existing copies can resume without overwriting");
    DiscardIndexMigrationCopies(collision);
    check(fs::exists(target / base), "rollback preserves preexisting verified copies");
    check(RemoveMigratedIndexSource(migration, error), "committed migration cleans old index");
    check(!fs::exists(source / base) && fs::exists(target / base), "new index survives cleanup");
    check(!fs::exists(source / L"Volumes"), "empty shards absent from copy inventory cleaned");
    check(fs::exists(source / L"user-empty-folder"), "unrelated empty directory preserved");
    check(fs::exists(source / L"config.json") && fs::exists(source / L"my-file.txt") &&
          fs::exists(source / L"v9/0123456789abcdef/my-notes.txt"), "configuration and user files preserved");
    const auto rollback = root / L"rollback";
    check(CopyIndexForMigration(target.wstring(), rollback.wstring(), migration, error), "prepare rollback fixture");
    DiscardIndexMigrationCopies(migration);
    check(fs::exists(target / base) && !fs::exists(rollback / base), "failed activation discards copies, keeps original");
    check(CopyIndexForMigration(target.wstring(), rollback.wstring(), migration, error), "retry after rollback succeeds");
    HANDLE locked = CreateFileW((target / base).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked != INVALID_HANDLE_VALUE && !RemoveMigratedIndexSource(migration, error), "locked source reports cleanup failure");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    check(RemoveMigratedIndexSource(migration, error), "cleanup retry handles previously removed files");
    check(!fs::exists(target), "empty former index root removed");
    check(CopyIndexForMigration(rollback.wstring(), target.wstring(), migration, error), "prepare changed-source fixture");
    write(rollback / base, "changed-after-copy");
    check(!RemoveMigratedIndexSource(migration, error) && fs::exists(rollback / base), "changed source is not deleted");
    write(target / base, "changed-after-copy");
    write(rollback / base, "different-existing-index");
    check(!CopyIndexForMigration(target.wstring(), rollback.wstring(), migration, error) &&
          migration.failure == ERROR_ALREADY_EXISTS, "different destination index is never overwritten");
    const auto empty_source = root / L"empty-source";
    fs::create_directories(empty_source / L"Volumes/1FE19AC33F476154");
    check(CopyIndexForMigration(empty_source.wstring(), (root / L"empty-target").wstring(), migration, error) &&
          RemoveMigratedIndexSource(migration, error) && !fs::exists(empty_source),
          "migration with only empty shards removes old root");
    // Exact fixture created by this test; no external paths are enumerated for removal.
    fs::remove_all(root);
    return failures ? 1 : 0;
}
