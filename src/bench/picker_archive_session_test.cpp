#include "../ui/picker_archive_session.h"
#include "../../extensions/7zip/worker_client.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>

namespace disk = std::filesystem;
using pulse::ui::PickerArchiveSession;
namespace {
int failures = 0;
void Check(bool value, const char* name) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
    if (!value) ++failures;
}
void Write(const disk::path& file, const std::string& bytes) {
    disk::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}
std::string Read(const disk::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
bool Has(const std::vector<pulse::fs::DirEntry>& rows, const wchar_t* name, bool directory) {
    return std::any_of(rows.begin(), rows.end(), [&](const auto& row) {
        return row.name == name && row.is_dir == directory && row.full_path_get().empty();
    });
}
size_t Children(const disk::path& path) {
    size_t count = 0;
    if (disk::exists(path)) for (const auto& child : disk::directory_iterator(path)) { (void)child; ++count; }
    return count;
}
struct ZipMember { std::string name, bytes; uint32_t attributes = 0; };
void ZipFixture(const disk::path& path, const std::vector<ZipMember>& members) {
    // Stored ZIP records independent of the 7-Zip writer, including malformed names.
    std::string bytes, central;
    auto u16 = [](std::string& out, uint16_t n) { out += static_cast<char>(n); out += static_cast<char>(n >> 8); };
    auto u32 = [&](std::string& out, uint32_t n) { u16(out, static_cast<uint16_t>(n)); u16(out, static_cast<uint16_t>(n >> 16)); };
    for (const auto& member : members) {
        uint32_t crc = 0xffffffffu;
        for (unsigned char byte : member.bytes) {
            crc ^= byte;
            for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
        crc = ~crc;
        const auto offset = static_cast<uint32_t>(bytes.size());
        const auto size = static_cast<uint32_t>(member.bytes.size());
        const auto length = static_cast<uint16_t>(member.name.size());
        u32(bytes, 0x04034b50); u16(bytes, 20); u16(bytes, 0x800); u16(bytes, 0);
        u16(bytes, 0); u16(bytes, 0); u32(bytes, crc); u32(bytes, size); u32(bytes, size);
        u16(bytes, length); u16(bytes, 0); bytes += member.name; bytes += member.bytes;
        u32(central, 0x02014b50); u16(central, 0x314); u16(central, 20); u16(central, 0x800); u16(central, 0);
        u16(central, 0); u16(central, 0); u32(central, crc); u32(central, size); u32(central, size);
        u16(central, length); u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0);
        u32(central, member.attributes); u32(central, offset); central += member.name;
    }
    const auto offset = static_cast<uint32_t>(bytes.size());
    bytes += central;
    u32(bytes, 0x06054b50); u16(bytes, 0); u16(bytes, 0);
    u16(bytes, static_cast<uint16_t>(members.size())); u16(bytes, static_cast<uint16_t>(members.size()));
    u32(bytes, static_cast<uint32_t>(central.size())); u32(bytes, offset); u16(bytes, 0);
    Write(path, bytes);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::puts("usage: picker_archive_session_test WORKER_EXE FRESH_FIXTURE_ROOT"); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const std::wstring worker = argv[1];
        const disk::path root = argv[2];
        if (!root.is_absolute()) return 2;
        disk::create_directories(root);
        ZipFixture(root / L"implicit.zip", {{"隐式/文件.txt", "implicit"}});
        ZipFixture(root / L"duplicate.zip", {{"Name.txt", "a"}, {"name.txt", "b"}});
        ZipFixture(root / L"traversal.zip", {{"../outside.txt", "escape"}});
        ZipFixture(root / L"symlink.zip", {{"link", "target.txt", 0120777u << 16}});
        ZipFixture(root / L"mutable.zip", {{"before.txt", "before"}});
        ZipFixture(root / L"replacement.zip", {{"after.txt", "longer after content"}});
        const auto stage = root / L"staging";
        const auto source = root / L"源文件";
        const std::string bytes = std::string(200000, 'p') + "unicode content\n";
        Write(source / L"子目录" / L"报告.txt", bytes);
        Write(source / L"未选中.bin", std::string(5000, 'x'));
        Write(source / L"readonly.txt", "read-only fixture");
        SetFileAttributesW((source / L"readonly.txt").c_str(), FILE_ATTRIBUTE_READONLY);
        Write(root / L"root.txt", "root bytes\n");
        worker_client::ScopedWorkerExecutable scope(worker);
        auto provider = [&] { return worker; };
        for (const auto format : {engine::Format::Zip, engine::Format::SevenZip}) {
            const auto name = format == engine::Format::Zip ? L"case.zip" : L"case.7z";
            const auto archive = root / name;
            engine::CreateOptions options;
            options.format = format;
            auto created = engine::Create({source.wstring(), (root / L"root.txt").wstring()}, root.wstring(), name, options, {}, {});
            Check(created.status == engine::Status::Ok, "real worker creates archive fixture");
            const auto original = Read(archive);
            std::wstring delivered;
            {
                PickerArchiveSession session(provider, stage.wstring());
                Check(!session.Contains((root / L"directory.zip").wstring()), "unregistered .zip directory is not an archive");
                session.Register(archive.wstring());
                Check(session.Contains(archive.wstring() + L"\\源文件\\子目录") &&
                      !session.Contains(archive.wstring() + L".other"), "registered virtual path requires separator boundary");
                std::vector<pulse::fs::DirEntry> rows;
                Check(session.List(archive.wstring(), rows) == ERROR_SUCCESS && Has(rows, L"源文件", true) &&
                      Has(rows, L"root.txt", false), "real archive root lists folders and ordinary files without fake full_path");
                Check(session.List(archive.wstring() + L"\\源文件\\子目录", rows) == ERROR_SUCCESS &&
                      rows.size() == 1 && Has(rows, L"报告.txt", false) && rows[0].size == bytes.size(), "Unicode child folder has real file size");
                Check(session.List(archive.wstring(), rows, [] { return true; }) == ERROR_CANCELLED && rows.empty(), "cancelled list returns no rows");
                Check(session.List(archive.wstring() + L"\\root.txt", rows) == ERROR_PATH_NOT_FOUND, "ordinary archive file is not a navigable folder");
                auto cancel = session.Materialize({archive.wstring() + L"\\root.txt"}, [] { return true; });
                Check(cancel.error == ERROR_CANCELLED && cancel.paths.empty() && !cancel.lease, "pre-cancelled extraction returns no paths or lease");
                auto invalid = session.Materialize({archive.wstring() + L"\\root.txt", archive.wstring() + L"\\missing.txt"});
                Check(invalid.error != ERROR_SUCCESS && invalid.paths.empty() && !invalid.lease, "invalid selection fails atomically without partial delivery");
                auto folder = session.Materialize({archive.wstring() + L"\\源文件"});
                Check(folder.error != ERROR_SUCCESS && folder.paths.empty(), "folder selection never expands into whole archive extraction");
                auto extracted = session.Materialize({archive.wstring() + L"\\源文件\\子目录\\报告.txt", archive.wstring() + L"\\root.txt"});
                Check(extracted.error == ERROR_SUCCESS && extracted.paths.size() == 2 && extracted.lease &&
                      Read(extracted.paths[0]) == bytes && Read(extracted.paths[1]) == "root bytes\n", "real selective extraction preserves file bytes and input order");
                if (extracted.paths.size() == 2) {
                    const auto batch = disk::path(extracted.paths[1]).parent_path();
                    Check(!disk::exists(batch / L"源文件" / L"未选中.bin"), "unselected file is not extracted");
                    extracted.lease->Cleanup();
                    Check(!disk::exists(batch.parent_path()), "explicit worker cleanup removes only the owned undelivered batch");
                }
                auto committed = session.Materialize({archive.wstring() + L"\\root.txt"});
                if (!committed.paths.empty() && committed.lease) {
                    delivered = committed.paths[0];
                    committed.lease->Commit();
                }
                auto readonly = session.Materialize({archive.wstring() + L"\\源文件\\readonly.txt"});
                const auto readonly_path = readonly.paths.empty() ? std::wstring{} : readonly.paths[0];
                const bool readonly_ok = readonly.error == ERROR_SUCCESS && !readonly_path.empty() &&
                    (GetFileAttributesW(readonly_path.c_str()) & FILE_ATTRIBUTE_READONLY);
                if (readonly.lease) readonly.lease->Cleanup();
                Check(readonly_ok && !disk::exists(readonly_path) &&
                      (GetFileAttributesW((source / L"readonly.txt").c_str()) & FILE_ATTRIBUTE_READONLY),
                      "cleanup removes owned read-only extraction without changing source attributes");
                const size_t before = Children(stage);
                std::wstring abandoned;
                { auto result = session.Materialize({archive.wstring() + L"\\root.txt"});
                  if (!result.paths.empty()) abandoned = result.paths[0]; }
                for (unsigned wait = 0; wait < 100 && !abandoned.empty() && disk::exists(abandoned); ++wait) Sleep(20);
                Check(!abandoned.empty() && !disk::exists(abandoned) && Children(stage) == before, "uncommitted delivery asynchronously cleans its unique directory");
                bool observed_output = false;
                auto during = session.Materialize({archive.wstring() + L"\\源文件\\子目录\\报告.txt"}, [&] {
                    for (const auto& item : disk::recursive_directory_iterator(stage)) {
                        if (item.path().filename() == L"报告.txt") { observed_output = true; return true; }
                    }
                    return false;
                });
                Check(observed_output && during.error == ERROR_CANCELLED && during.paths.empty() && !during.lease &&
                      Children(stage) == before, "cancellation after output appears removes the partial batch and returns no paths");
            }
            Check(!delivered.empty() && Read(delivered) == "root bytes\n", "committed file remains readable after lease and session destruction");
            Check(Read(archive) == original, "listing and extraction leave original archive byte-for-byte unchanged");
        }
        {
            engine::CreateOptions protected_zip;
            protected_zip.format = engine::Format::Zip;
            protected_zip.password = L"fixture-password";
            const auto encrypted = root / L"zz-encrypted.zip";
            Check(engine::Create({(root / L"root.txt").wstring()}, root.wstring(), L"zz-encrypted.zip", protected_zip, {}, {}).status == engine::Status::Ok,
                  "real password-protected ZIP fixture is created");
            PickerArchiveSession session(provider, stage.wstring());
            const auto plain = root / L"case.zip";
            session.Register(plain.wstring());
            session.Register(encrypted.wstring());
            const auto before = Children(stage);
            auto failed = session.Materialize({plain.wstring() + L"\\root.txt", encrypted.wstring() + L"\\root.txt"});
            Check(failed.error == ERROR_PASSWORD_RESTRICTION && failed.paths.empty() && !failed.lease && Children(stage) == before,
                  "second archive password failure removes first archive output with no half-delivery");
        }
        // These stored ZIPs were generated independently of the engine under test.
        {
            PickerArchiveSession session(provider, stage.wstring());
            auto implicit = root / L"implicit.zip";
            session.Register(implicit.wstring());
            std::vector<pulse::fs::DirEntry> rows;
            Check(session.List(implicit.wstring(), rows) == ERROR_SUCCESS && Has(rows, L"隐式", true), "missing directory records produce navigable implicit directory");
            Check(session.List(implicit.wstring() + L"\\隐式", rows) == ERROR_SUCCESS && Has(rows, L"文件.txt", false), "implicit directory lists its real Unicode member");
            for (const auto* file : {L"duplicate.zip", L"traversal.zip", L"symlink.zip"}) {
                auto archive = root / file;
                session.Register(archive.wstring());
                Check(session.List(archive.wstring(), rows) != ERROR_SUCCESS && rows.empty(), "unsafe, case-colliding, or link archive is rejected without partial rows");
            }
            const auto mutable_archive = root / L"mutable.zip";
            session.Register(mutable_archive.wstring());
            Check(session.List(mutable_archive.wstring(), rows) == ERROR_SUCCESS && Has(rows, L"before.txt", false), "cache initially lists first archive identity");
            disk::copy_file(root / L"replacement.zip", mutable_archive, disk::copy_options::overwrite_existing);
            Check(session.List(mutable_archive.wstring(), rows) == ERROR_SUCCESS && Has(rows, L"after.txt", false) && !Has(rows, L"before.txt", false), "changed size or timestamp invalidates cached archive listing");
            PickerArchiveSession missing_runtime([] { return std::wstring{}; }, stage.wstring());
            missing_runtime.Register(implicit.wstring());
            Check(missing_runtime.List(implicit.wstring(), rows) == ERROR_MOD_NOT_FOUND && rows.empty(), "missing optional runtime maps to module-not-found");
            PickerArchiveSession missing_executable([&] { return (root / L"absent-worker.exe").wstring(); }, stage.wstring());
            missing_executable.Register(implicit.wstring());
            Check(missing_executable.List(implicit.wstring(), rows) == ERROR_FILE_NOT_FOUND && rows.empty(), "missing worker executable preserves file-not-found error");
        }
        engine::ShutdownWorkers();
        std::printf("RESULT failures=%d\n", failures);
        return failures ? 1 : 0;
    } catch (const std::exception& error) { std::printf("exception: %s\n", error.what()); return 2; }
}
