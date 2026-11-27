#include "../ui/folder_picker_loader.h"
#include "../ui/picker_archive_session.h"
#include "../../extensions/7zip/worker_client.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace disk = std::filesystem;
using namespace pulse::ui;
namespace {
int failures = 0;
constexpr UINT kListing = WM_APP + 141, kValidation = WM_APP + 142, kCommit = WM_APP + 143;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
void Write(const disk::path& path, const std::string& bytes) {
    disk::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}
std::string Read(const disk::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
const PickerEntry* Find(const PickerListing& listing, const wchar_t* name) {
    const auto found = std::find_if(listing.entries.begin(), listing.entries.end(),
        [&](const auto& entry) { return entry.name == name; });
    return found == listing.entries.end() ? nullptr : &*found;
}
std::unique_ptr<PickerValidation> WaitValidation(HWND window, bool remove = true) {
    const auto deadline = GetTickCount64() + 15000;
    MSG message{};
    while (GetTickCount64() < deadline) {
        if (PeekMessageW(&message, window, kValidation, kValidation, remove ? PM_REMOVE : PM_NOREMOVE)) {
            if (!remove) return std::make_unique<PickerValidation>();
            return PickerLoader::TakeValidation(message.lParam);
        }
        Sleep(2);
    }
    return {};
}
bool EmptyStaging(const disk::path& stage) {
    return !disk::exists(stage) || disk::is_empty(stage);
}
void WaitCleanup(const disk::path& stage) {
    for (unsigned wait = 0; wait < 250 && !EmptyStaging(stage); ++wait) Sleep(10);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::puts("usage: picker_archive_loader_test WORKER_EXE FRESH_FIXTURE_ROOT"); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const std::wstring worker = argv[1];
        const disk::path root = disk::path(argv[2]).make_preferred(), source = root / L"source", stage = root / L"staging";
        if (!root.is_absolute()) return 2;
        disk::create_directories(root / L"directory.zip");
        Write(source / L"a.txt", "alpha");
        Write(source / L"z.txt", "zulu bytes");
        Write(source / L"picture.png", "image fixture");
        Write(source / L"hidden.txt", "hidden");
        Write(source / L"protected.txt", "protected");
        Write(source / L"子目录" / L"nested.txt", "nested");
        Check(SetFileAttributesW((source / L"hidden.txt").c_str(), FILE_ATTRIBUTE_HIDDEN) &&
              SetFileAttributesW((source / L"protected.txt").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM),
              "fixture carries real hidden and protected attributes");
        const auto archive = root / L"browse.zip";
        worker_client::ScopedWorkerExecutable runtime(worker);
        engine::CreateOptions zip;
        zip.format = engine::Format::Zip;
        std::vector<std::wstring> sources;
        for (const auto& item : disk::directory_iterator(source)) sources.push_back(item.path().wstring());
        Check(engine::Create(sources, root.wstring(), L"browse.zip", zip, {}, {}).status == engine::Status::Ok,
              "real worker creates ZIP preserving source metadata");
        const auto archive_bytes = Read(archive);
        disk::copy_file(archive, root / L"hidden.zip");
        disk::copy_file(archive, root / L"protected.zip");
        SetFileAttributesW((root / L"hidden.zip").c_str(), FILE_ATTRIBUTE_HIDDEN);
        SetFileAttributesW((root / L"protected.zip").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        auto session = std::make_shared<PickerArchiveSession>([worker] { return worker; }, stage.wstring());
        PickerOptions options;
        options.archive = session;
        options.filters = {{L"Text", L"*.txt"}};
        auto outer = ReadPickerListing(root.wstring(), PickerMode::File, options);
        const auto* outer_archive = Find(outer, L"browse.zip");
        Check(!outer.error && outer_archive && outer_archive->archive && outer_archive->kind == PickerEntryKind::File,
              "outer archive bypasses caller text filter but remains a file");
        const auto* directory = Find(outer, L"directory.zip");
        Check(directory && !directory->archive && directory->kind == PickerEntryKind::Folder,
              "directory named .zip remains an ordinary directory");
        Check(!Find(outer, L"hidden.zip") && !Find(outer, L"protected.zip"), "outer archive exception preserves hidden and protected filtering");
        auto plain_options = options;
        plain_options.archive.reset();
        Check(!Find(ReadPickerListing(root.wstring(), PickerMode::File, plain_options), L"browse.zip"),
              "picker without archive session retains original strict outer filter");
        session->Register(archive.wstring());
        auto inside = ReadPickerListing(archive.wstring(), PickerMode::File, options);
        Check(!inside.error && Find(inside, L"a.txt") && Find(inside, L"z.txt") && Find(inside, L"子目录") &&
              !Find(inside, L"picture.png"), "inside archive applies caller filter while retaining navigation folders");
        Check(!Find(inside, L"hidden.txt") && !Find(inside, L"protected.txt"), "inside archive preserves hidden and protected metadata policy");
        Check(inside.entries.size() == 3 && inside.entries[0].name == L"子目录" && inside.entries[1].name == L"a.txt" &&
              inside.entries[2].name == L"z.txt", "archive listing uses normal folder-first name sorting");
        auto visible = options;
        visible.show_hidden = true;
        inside = ReadPickerListing(archive.wstring(), PickerMode::File, visible);
        Check(Find(inside, L"hidden.txt") && !Find(inside, L"protected.txt"), "show hidden does not expose protected archive members");
        visible.show_protected_os_files = true;
        Check(Find(ReadPickerListing(archive.wstring(), PickerMode::File, visible), L"protected.txt") != nullptr,
              "show protected exposes protected archive member explicitly");
        auto sorted = options;
        sorted.sort = PickerSort::Size;
        sorted.descending = true;
        inside = ReadPickerListing(archive.wstring(), PickerMode::File, sorted);
        Check(inside.entries.size() == 3 && inside.entries[0].kind == PickerEntryKind::Folder &&
              inside.entries[1].name == L"z.txt" && inside.entries[2].name == L"a.txt", "archive size sort uses actual member byte sizes");
        const HWND window = CreateWindowExW(0, L"STATIC", L"archive loader test", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "message-only receiver does not show or activate a desktop window");
        if (!window) return 1;
        {
            PickerLoader loader(window, kListing, kValidation, kCommit);
            loader.Validate(1, archive.wstring(), {L"子目录"}, PickerMode::File, options);
            auto navigation = WaitValidation(window);
            if (navigation) std::wprintf(L"NAV error=%lu navigate=%ls expected=%ls paths=%zu lease=%d\n", navigation->error,
                navigation->navigate.c_str(), (archive.wstring() + L"\\子目录").c_str(), navigation->paths.size(), !!navigation->archive_delivery);
            Check(navigation && !navigation->error && navigation->navigate == archive.wstring() + L"\\子目录" &&
                  navigation->paths.empty() && !navigation->archive_delivery, "real loader validates virtual directory as navigation, never delivery");
            loader.Validate(2, archive.wstring(), {L"z.txt", L"a.txt"}, PickerMode::File, options);
            auto selection = WaitValidation(window);
            if (selection) std::wprintf(L"SELECT error=%lu paths=%zu source=%ls failed=%ls lease=%d\n", selection->error,
                selection->paths.size(), selection->archive_source.c_str(), selection->failed_path.c_str(), !!selection->archive_delivery);
            Check(selection && !selection->error && selection->paths.size() == 2 && selection->archive_delivery &&
                  Read(selection->paths[0]) == "zulu bytes" && Read(selection->paths[1]) == "alpha" &&
                  selection->archive_source == archive.wstring(), "real loader materializes multi-selection into real files in selected order");
            selection.reset();
            WaitCleanup(stage);
            Check(EmptyStaging(stage), "discarded validation lease cleans all materialized files asynchronously");
            loader.Validate(3, archive.wstring(), {L"a.txt", L"picture.png"}, PickerMode::File, options);
            auto rejected = WaitValidation(window);
            Check(rejected && rejected->error && rejected->paths.empty() && !rejected->archive_delivery,
                  "post-materialization caller filter failure returns no partial result or lease");
            rejected.reset();
            WaitCleanup(stage);
            Check(EmptyStaging(stage), "failed final validation cleans extraction batch");
            loader.Load(4, archive.wstring() + L"\\子目录", PickerMode::File, options);
            bool listed = false, admitted = false;
            const auto deadline = GetTickCount64() + 15000;
            while (GetTickCount64() < deadline && (!listed || !admitted)) {
                MSG message{};
                if (PeekMessageW(&message, window, kCommit, kCommit, PM_REMOVE)) {
                    auto commit = PickerLoader::TakeFolderCommit(message.lParam);
                    admitted = commit && commit->generation == 4;
                }
                if (PeekMessageW(&message, window, kListing, kListing, PM_REMOVE)) {
                    auto listing = PickerLoader::Take(message.lParam);
                    listed = listing && listing->generation == 4 && !listing->error && Find(*listing, L"nested.txt");
                }
                Sleep(2);
            }
            Check(admitted && listed, "real loader admits and lists virtual folder through asynchronous Load");
            loader.Validate(5, archive.wstring(), {L"a.txt"}, PickerMode::File, options);
            Check(WaitValidation(window, false) != nullptr, "pending delivered payload is posted before closing loader");
        }
        WaitCleanup(stage);
        Check(EmptyStaging(stage), "closing loader drains posted validation and cancels its uncommitted lease");
        std::wstring retained;
        {
            PickerLoader loader(window, kListing, kValidation);
            loader.Validate(6, archive.wstring(), {L"a.txt"}, PickerMode::File, options);
            auto result = WaitValidation(window);
            if (result && !result->error && result->archive_delivery && result->paths.size() == 1) {
                retained = result->paths[0];
                result->archive_delivery->Commit();
            }
        }
        DestroyWindow(window);
        options.archive.reset();
        session.reset();
        Check(!retained.empty() && Read(retained) == "alpha", "committed loader result remains readable after picker teardown");
        Check(Read(archive) == archive_bytes, "complete loader flow leaves original archive bytes unchanged");
        engine::ShutdownWorkers();
        std::printf("RESULT failures=%d\n", failures);
        return failures ? 1 : 0;
    } catch (const std::exception& error) { std::printf("exception: %s\n", error.what()); return 2; }
}
