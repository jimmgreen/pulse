#include "../../app/extensions/native/directories_adapter.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace pulse::extensions;
using namespace pulse::extensions::native;
#if defined(PULSE_DIRECTORIES_TEST)
namespace pulse::extensions::native { void ExhaustDirectoryIdsForTest() noexcept; }
#endif
namespace {
int failures = 0, checks = 0;
void Check(bool okay, const char *label) {
    std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", label);
    ++checks; failures += !okay;
}
std::string Utf8(const std::wstring &text) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(count), 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count,
                         nullptr, nullptr);
    return result;
}
PulseStringView View(const std::string &text) { return {text.data(), static_cast<uint32_t>(text.size())}; }
std::string Copy(PulseStringView text) { return text.length ? std::string(text.data, text.length) : std::string(); }
struct Temp {
    std::wstring path;
    Temp() {
        wchar_t base[32768]{};
        GetTempPathW(static_cast<DWORD>(std::size(base)), base);
        path = std::wstring(base) + L"pulse-native-directory-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64());
        if (!CreateDirectoryW(path.c_str(), nullptr)) throw std::runtime_error("fixture_create_failed");
    }
    ~Temp() {
        std::error_code error;
        for (int attempt = 0; attempt < 50; ++attempt) {
            std::filesystem::remove_all(L"\\\\?\\" + path, error);
            if (!error) return;
            Sleep(20);
        }
        std::printf("[FAIL] fixture cleanup error=%d\n", error.value());
        ++failures;
    }
};
bool File(const std::wstring &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool okay = WriteFile(file, "abc", 3, &written, nullptr) && written == 3;
    CloseHandle(file);
    return okay;
}
struct Row {
    uint64_t id = 0, size = 0;
    uint32_t flags = 0;
    std::string name;
};
struct Capture {
    unsigned calls = 0;
    PulseStatus status = -1;
    DWORD thread = 0;
    uint32_t operation = 0, next = 0, system_error = 0;
    uint64_t directory = 0, snapshot = 0, total = 0;
    std::string error, path;
    std::vector<Row> rows;
    std::function<void()> hook;
    static void PULSE_CALL Done(void *user, const PulseDirectoryReply *reply) {
        auto &self = *static_cast<Capture *>(user);
        ++self.calls; self.status = reply->status; self.operation = reply->operation; self.thread = GetCurrentThreadId();
        self.directory = reply->directory_id; self.snapshot = reply->snapshot_id; self.total = reply->total;
        self.next = reply->next_offset; self.system_error = reply->system_error;
        self.error = Copy(reply->error); self.path = Copy(reply->path); self.rows.clear();
        for (uint32_t index = 0; index < reply->count; ++index) {
            const auto &row = reply->rows[index];
            self.rows.push_back({row.entry_id, row.size_bytes, row.flags, Copy(row.name)});
        }
        if (self.hook) self.hook();
    }
};
struct Changes {
    unsigned ready = 0, invalidated = 0, errors = 0, calls = 0;
    uint64_t sequence = 0;
    bool ordered = true;
    DWORD thread = 0;
    std::function<void(uint32_t)> hook;
    static void PULSE_CALL Done(void *user, const PulseDirectoryChange *change) {
        auto &self = *static_cast<Changes *>(user);
        ++self.calls; self.thread = GetCurrentThreadId();
        self.ordered = self.ordered && change->sequence > self.sequence;
        self.sequence = change->sequence;
        if (change->kind == PULSE_DIRECTORY_WATCH_READY) ++self.ready;
        if (change->kind == PULSE_DIRECTORY_INVALIDATED) ++self.invalidated;
        if (change->kind == PULSE_DIRECTORY_WATCH_ERROR) ++self.errors;
        if (self.hook) self.hook(change->kind);
    }
};
bool Pump(DirectoriesAdapter &adapter, const std::function<bool()> &done, DWORD timeout = 8000) {
    const auto end = GetTickCount64() + timeout;
    do {
        adapter.Poll();
        if (done()) return true;
        Sleep(5);
    } while (GetTickCount64() < end);
    return done();
}
bool Open(DirectoriesAdapter &adapter, const PulseDirectoriesV1 &api, const std::wstring &path, Capture &capture) {
    const auto text = Utf8(path);
    uint64_t request = 0;
    return api.open(api.context, View(text), Capture::Done, &capture, &request) == PULSE_OK && request &&
           Pump(adapter, [&] { return capture.calls != 0; });
}
}

int wmain(int argc, wchar_t **argv) {
    try {
        authorization::Registry registry;
        authorization::PackageIdentity identity{0, "directory.fixture", "revision", 1};
        registry.SyncPackages({{identity, true}});
        const auto owner = registry.RegisterOwner(0x1234);
        const auto caller = registry.CaptureBinding(identity, *owner);
        bool enabled = true;
        std::vector<std::pair<std::wstring, bool>> opened;
        DirectoriesBackend backend;
        backend.live = [&](const CallerBinding &binding) { return enabled && registry.ValidateBinding(binding); };
        backend.open_item = [&](const CallerBinding &, const std::wstring &path, bool folder) {
            opened.emplace_back(path, folder); return PULSE_OK;
        };
        Temp temp;
        const auto main_path = temp.path + L"\\main";
        const auto empty_path = temp.path + L"\\empty";
        CreateDirectoryW(main_path.c_str(), nullptr); CreateDirectoryW(empty_path.c_str(), nullptr);
        bool fixture_okay = true;
        for (int index = 0; index < 258; ++index)
            fixture_okay = File(main_path + L"\\file-" + std::to_wstring(index) + L".txt") && fixture_okay;
        fixture_okay = File(main_path + L"\\目录-β.txt") && fixture_okay;
        fixture_okay = CreateDirectoryW((main_path + L"\\folder").c_str(), nullptr) && fixture_okay;
        Check(fixture_okay, "create isolated real 260-entry fixture including Unicode and folder");
        DirectoriesAdapter adapter(*caller, backend);
        PulseDirectoriesV1 api{};
        Check(adapter.Query(1, &api, sizeof(api)) == PULSE_OK && api.size == sizeof(api) && api.version == 1,
              "query additive native directories table");
        Check(adapter.Query(2, &api, sizeof(api)) == PULSE_ERROR_ABI &&
              adapter.Query(1, &api, sizeof(api) - 1) == PULSE_ERROR_ABI &&
              adapter.Query(1, nullptr, sizeof(api)) == PULSE_ERROR_ARGUMENT,
              "ABI version, output size and null table rejected");
        uint64_t request = 0;
        Capture rejected;
        bool invalid_paths = true;
        for (const std::string path : {"relative", "C:relative", "\\rooted", "shell:Desktop", "\\\\.\\pipe\\pulse",
                "\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1", "\\\\server", "\\\\.\\share", "\\\\..\\share",
                "\\\\?\\UNC\\.\\share", "\\\\?\\UNC\\?\\share", "\\\\server\\.", "C:\\NUL", "C:\\foo:stream"})
            invalid_paths = api.open(api.context, View(path), Capture::Done, &rejected, &request) == PULSE_ERROR_ARGUMENT &&
                            !request && invalid_paths;
        const std::string invalid_utf8("\xff", 1);
        invalid_paths = api.open(api.context, View(invalid_utf8), Capture::Done, &rejected, &request) == PULSE_ERROR_ARGUMENT && invalid_paths;
        Check(invalid_paths && !rejected.calls, "relative, virtual, device, incomplete UNC, ADS and invalid UTF-8 paths rejected");
        Capture listing;
        const auto text = Utf8(main_path);
        Check(api.open(api.context, View(text), Capture::Done, &listing, &request) == PULSE_OK && request && !listing.calls,
              "open accepts asynchronously without invoking the callback inline");
        Check(Pump(adapter, [&] { return listing.calls != 0; }) && listing.status == PULSE_OK && listing.total == 260 &&
              listing.directory && listing.snapshot && listing.rows.empty() && listing.thread == GetCurrentThreadId(),
              "worker enumeration returns an owner-thread immutable snapshot");
        const uint64_t directory = listing.directory;
        uint64_t snapshot = listing.snapshot;
        const auto transfer_target = adapter.ResolveTransferTarget(directory);
        Check(transfer_target && Utf8(transfer_target->path) == listing.path && transfer_target->valid(),
              "opened directory resolves a normalized path copy and a live transfer target lease");
        Check(!adapter.ResolveTransferTarget(0) && !adapter.ResolveTransferTarget(UINT64_MAX),
              "unknown directory handles cannot resolve a transfer target");
        Capture page;
        Check(api.list(api.context, directory, snapshot, 0, 256, Capture::Done, &page, &request) == PULSE_OK && !page.calls,
              "list accepts an explicit offset and defers its callback");
        Check(Pump(adapter, [&] { return page.calls != 0; }) && page.rows.size() == 256 && page.next == 256 && page.total == 260,
              "first immutable page respects the 256-row cap");
        std::vector<Row> all = page.rows;
        Capture tail;
        api.list(api.context, directory, snapshot, 256, 256, Capture::Done, &tail, &request);
        Check(Pump(adapter, [&] { return tail.calls != 0; }) && tail.rows.size() == 4 && tail.next == UINT32_MAX,
              "last page reports UINT32_MAX exhaustion");
        all.insert(all.end(), tail.rows.begin(), tail.rows.end());
        std::set<uint64_t> ids;
        for (const auto &row : all) ids.insert(row.id);
        Check(ids.size() == 260 && std::any_of(all.begin(), all.end(), [](const Row &row) { return row.name == "目录-β.txt" && row.size == 3; }),
              "entry IDs are unique and borrowed Unicode names and metadata copy correctly");
        Check(api.list(api.context, directory, snapshot, 0, 257, Capture::Done, &page, &request) == PULSE_ERROR_ARGUMENT &&
              api.list(api.context, directory, snapshot, 0, 0, Capture::Done, &page, &request) == PULSE_ERROR_ARGUMENT &&
              api.list(api.context, directory, snapshot, 261, 1, Capture::Done, &page, &request) == PULSE_ERROR_ARGUMENT,
              "invalid list limits and offsets are rejected");
        DirectoriesAdapter foreign(*caller, backend);
        const auto other = foreign.Table();
        Check(other.list(other.context, directory, snapshot, 0, 1, Capture::Done, &page, &request) == PULSE_ERROR_STATE &&
              other.close(other.context, directory) == PULSE_ERROR_STATE && !foreign.ResolveTransferTarget(directory),
              "foreign attachment cannot use or close a directory handle");
        Capture foreign_directory;
        Check(Open(foreign, other, empty_path, foreign_directory) && foreign_directory.status == PULSE_OK &&
              !adapter.ResolveTransferTarget(foreign_directory.directory),
              "another attachment's successfully opened directory cannot resolve a transfer target");
        other.close(other.context, foreign_directory.directory);
        const auto file = *std::find_if(all.begin(), all.end(), [](const Row &row) { return row.name == "file-0.txt"; });
        const auto folder = *std::find_if(all.begin(), all.end(), [](const Row &row) { return row.name == "folder"; });
        const auto tree_source = adapter.ResolveTreeTransferSource(directory, snapshot, folder.id);
        Check(tree_source && tree_source->leaf == L"folder" &&
              transfer_target && tree_source->path == transfer_target->path + L"\\folder" && tree_source->valid(),
              "current ordinary folder snapshot entry resolves an attachment-bound tree source lease");
        Check(!adapter.ResolveTreeTransferSource(directory, snapshot, file.id) &&
              !adapter.ResolveTreeTransferSource(directory, snapshot + 1, folder.id) &&
              !adapter.ResolveTreeTransferSource(foreign_directory.directory, snapshot, folder.id),
              "tree source resolution rejects files, stale snapshots and foreign directory handles");
        Capture dispatched;
        api.open_entry(api.context, directory, snapshot, file.id, Capture::Done, &dispatched, &request);
        Check(Pump(adapter, [&] { return dispatched.calls != 0; }) && dispatched.status == PULSE_OK && opened.size() == 1 &&
              !opened.back().second, "worker verifies snapshot file before host dispatch");
        Capture folder_dispatch;
        api.open_entry(api.context, directory, snapshot, folder.id, Capture::Done, &folder_dispatch, &request);
        Check(Pump(adapter, [&] { return folder_dispatch.calls != 0; }) && folder_dispatch.status == PULSE_OK &&
              opened.size() == 2 && opened.back().second, "snapshot folder delegates to normal host navigation");
        DeleteFileW((main_path + L"\\file-0.txt").c_str());
        Capture missing_entry;
        api.open_entry(api.context, directory, snapshot, file.id, Capture::Done, &missing_entry, &request);
        Check(Pump(adapter, [&] { return missing_entry.calls != 0; }) && missing_entry.status == PULSE_ERROR_STATE &&
              missing_entry.system_error && opened.size() == 2, "removed snapshot entry fails verification without dispatch");
        Check(adapter.ResolveTransferTarget(directory).has_value() && transfer_target && transfer_target->valid(),
              "native directory transfer leases need no resource declaration");
        PulseStatus wrong_thread = PULSE_OK, wrong_poll_query = PULSE_OK, wrong_close = PULSE_OK;
        bool wrong_target = true, wrong_target_valid = true;
        std::thread worker([&] {
            uint64_t ignored = 0;
            wrong_thread = api.list(api.context, directory, snapshot, 0, 1, Capture::Done, &rejected, &ignored);
            PulseDirectoriesV1 result{};
            wrong_poll_query = adapter.Query(1, &result, sizeof(result));
            wrong_close = adapter.Close(); adapter.Poll();
            wrong_target = adapter.ResolveTransferTarget(directory).has_value();
            wrong_target_valid = transfer_target && transfer_target->valid();
        });
        worker.join();
        Check(wrong_thread == PULSE_ERROR_STATE && wrong_poll_query == PULSE_ERROR_STATE && wrong_close == PULSE_ERROR_STATE,
              "operations, Query, Poll and Close enforce the owner thread");
        Check(!wrong_target && !wrong_target_valid && transfer_target && transfer_target->valid(),
              "transfer targets reject non-owner threads without invalidating the owner-thread lease");
        Capture cancelled;
        api.list(api.context, directory, snapshot, 0, 1, Capture::Done, &cancelled, &request);
        const uint64_t cancelled_request = request;
        Check(api.cancel(api.context, request) == PULSE_OK && other.cancel(other.context, cancelled_request) == PULSE_ERROR_STATE,
              "request cancellation is attachment-bound");
        adapter.Poll();
        Check(!cancelled.calls, "cancel suppresses an already-queued completion");
        std::vector<uint64_t> pending;
        bool accepted32 = true;
        for (unsigned index = 0; index < 32; ++index) {
            accepted32 = api.list(api.context, directory, snapshot, 0, 1, Capture::Done, &cancelled, &request) == PULSE_OK && accepted32;
            pending.push_back(request);
        }
        Check(accepted32 && api.list(api.context, directory, snapshot, 0, 1, Capture::Done, &cancelled, &request) == PULSE_ERROR_STATE,
              "32 pending requests are bounded without dropping queued work");
        for (const auto id : pending) api.cancel(api.context, id);
        adapter.Poll();
        Check(!cancelled.calls, "cancelling a full pending queue suppresses every callback");
        Capture refreshed;
        File(main_path + L"\\new.txt");
        api.refresh(api.context, directory, Capture::Done, &refreshed, &request);
        Check(transfer_target && transfer_target->valid(), "pending refresh preserves the directory transfer target lease");
        Check(Pump(adapter, [&] { return refreshed.calls != 0; }) && refreshed.status == PULSE_OK && refreshed.snapshot != snapshot,
              "refresh replaces the immutable snapshot with a new generation");
        Check(transfer_target && transfer_target->valid() && adapter.ResolveTransferTarget(directory),
              "successful refresh preserves the directory transfer target lease");
        Check(api.list(api.context, directory, snapshot, 0, 1, Capture::Done, &page, &request) == PULSE_ERROR_STATE &&
              api.open_entry(api.context, directory, snapshot, folder.id, Capture::Done, &rejected, &request) == PULSE_ERROR_STATE,
              "successful refresh invalidates old snapshot and entry references");
        Check(tree_source && !tree_source->valid() &&
              !adapter.ResolveTreeTransferSource(directory, snapshot, folder.id),
              "successful refresh immediately invalidates a captured tree-source lease");
        snapshot = refreshed.snapshot;
        const auto hidden = temp.path + L"\\temporarily-renamed";
        std::filesystem::rename(main_path, hidden);
        Capture failed_refresh;
        api.refresh(api.context, directory, Capture::Done, &failed_refresh, &request);
        Check(Pump(adapter, [&] { return failed_refresh.calls != 0; }) && failed_refresh.status == PULSE_ERROR_STATE &&
              failed_refresh.snapshot == snapshot && failed_refresh.total == refreshed.total,
              "failed refresh preserves the established immutable snapshot");
        Capture retained;
        api.list(api.context, directory, snapshot, 0, 1, Capture::Done, &retained, &request);
        Check(Pump(adapter, [&] { return retained.calls != 0; }) && retained.status == PULSE_OK,
              "preserved snapshot remains listable after refresh failure");
        std::filesystem::rename(hidden, main_path);
        Capture superseded, current;
        api.refresh(api.context, directory, Capture::Done, &superseded, &request);
        api.refresh(api.context, directory, Capture::Done, &current, &request);
        Check(Pump(adapter, [&] { return current.calls != 0; }) && !superseded.calls && current.status == PULSE_OK,
              "new refresh generation suppresses the previous refresh callback");
        snapshot = current.snapshot;
        Capture empty;
        Check(Open(adapter, api, empty_path, empty) && empty.status == PULSE_OK && !empty.total && empty.next == UINT32_MAX,
              "empty real directory is a successful exhausted snapshot");
        Capture absent;
        Check(Open(adapter, api, temp.path + L"\\missing", absent) && absent.status == PULSE_ERROR_STATE &&
              absent.system_error && !absent.directory, "missing directory reports a system error and releases its handle slot");
        Capture third, fourth;
        Open(adapter, api, empty_path, third); Open(adapter, api, empty_path, fourth);
        Check(third.status == PULSE_OK && fourth.status == PULSE_OK &&
              api.open(api.context, View(text), Capture::Done, &rejected, &request) == PULSE_ERROR_STATE,
              "at most four directory handles belong to one attachment");
        api.close(api.context, third.directory); api.close(api.context, fourth.directory);
        bool unc_accepted = true;
        for (const std::string path : {"\\\\localhost\\__pulse_native_missing_share__", "\\\\?\\UNC\\localhost\\__pulse_native_missing_share__"}) {
            unc_accepted = api.open(api.context, View(path), Capture::Done, &cancelled, &request) == PULSE_OK && unc_accepted;
            api.cancel(api.context, request);
        }
        Check(unc_accepted, "absolute UNC and extended UNC server/share paths remain accepted");
        std::wstring long_path = L"\\\\?\\" + temp.path;
        for (unsigned index = 0; index < 5; ++index) {
            long_path += L"\\" + std::wstring(55, L'x') + std::to_wstring(index);
            fixture_okay = CreateDirectoryW(long_path.c_str(), nullptr) && fixture_okay;
        }
        Capture long_directory;
        Check(fixture_okay && Open(adapter, api, long_path, long_directory) && long_directory.status == PULSE_OK && !long_directory.total,
              "real extended drive path longer than MAX_PATH opens successfully");
        api.close(api.context, long_directory.directory);
        std::vector<uint64_t> watch_ids;
        Changes bounded;
        bool eight_watches = true;
        for (unsigned index = 0; index < 8; ++index) {
            uint64_t id = 0;
            eight_watches = api.watch(api.context, directory, Changes::Done, &bounded, &id) == PULSE_OK && eight_watches;
            watch_ids.push_back(id);
        }
        Check(eight_watches && api.watch(api.context, directory, Changes::Done, &bounded, &request) == PULSE_ERROR_STATE,
              "watch handles are bounded at eight per attachment");
        for (const auto id : watch_ids) api.unwatch(api.context, id);
        adapter.Poll();
        Check(!bounded.calls, "unwatch suppresses queued watch startup notifications");
        Changes changes;
        uint64_t watch_id = 0;
        Check(api.watch(api.context, directory, Changes::Done, &changes, &watch_id) == PULSE_OK && !changes.calls &&
              Pump(adapter, [&] { return changes.ready || changes.errors; }) && changes.ready == 1 && !changes.errors,
              "real DirWatch defers READY until ReadDirectoryChangesW is armed");
        unsigned before = changes.invalidated;
        File(main_path + L"\\watched-create.txt");
        Check(Pump(adapter, [&] { return changes.invalidated > before; }), "armed real watch reports create invalidation");
        before = changes.invalidated;
        std::filesystem::rename(main_path + L"\\watched-create.txt", main_path + L"\\watched-rename.txt");
        Check(Pump(adapter, [&] { return changes.invalidated > before; }), "armed real watch reports rename invalidation");
        before = changes.invalidated;
        DeleteFileW((main_path + L"\\watched-rename.txt").c_str());
        Check(Pump(adapter, [&] { return changes.invalidated > before; }) && changes.ordered &&
              changes.thread == GetCurrentThreadId(), "armed real watch reports delete with owner-thread ordered callbacks");
        Check(other.unwatch(other.context, watch_id) == PULSE_ERROR_STATE && api.unwatch(api.context, watch_id) == PULSE_OK,
              "watch handles reject foreign attachment access");
        const auto changes_after_unwatch = changes.calls;
        File(main_path + L"\\after-unwatch.txt");
        adapter.Poll();
        Check(changes.calls == changes_after_unwatch, "unwatch immediately suppresses later changes");
        Changes reentrant;
        Capture reentrant_page;
        PulseStatus unwatch_inside = -1, list_inside = -1;
        uint64_t reentrant_watch = 0;
        reentrant.hook = [&](uint32_t kind) {
            if (kind != PULSE_DIRECTORY_WATCH_READY) return;
            unwatch_inside = api.unwatch(api.context, reentrant_watch);
            list_inside = api.list(api.context, directory, snapshot, 0, 1, Capture::Done, &reentrant_page, &request);
            adapter.Poll();
        };
        api.watch(api.context, directory, Changes::Done, &reentrant, &reentrant_watch);
        Check(Pump(adapter, [&] { return reentrant_page.calls != 0; }) && unwatch_inside == PULSE_OK && list_inside == PULSE_OK &&
              reentrant.ready == 1 && !reentrant.invalidated,
              "watch callback can unwatch, queue a list, and reenter Poll without invalid references");
        Capture closed_pending;
        api.list(api.context, empty.directory, empty.snapshot, 0, 1, Capture::Done, &closed_pending, &request);
        const auto closing_target = adapter.ResolveTransferTarget(empty.directory);
        Check(api.close(api.context, empty.directory) == PULSE_OK, "close accepts an owned directory handle");
        Check(closing_target && !closing_target->valid() && !adapter.ResolveTransferTarget(empty.directory),
              "directory close immediately invalidates its transfer target lease");
        adapter.Poll();
        Check(!closed_pending.calls, "directory close suppresses its queued completion");
        {
            bool live = true;
            auto functions = backend;
            functions.live = [&](const CallerBinding &binding) { return live && registry.ValidateBinding(binding); };
            DirectoriesAdapter stopped(*caller, functions);
            const auto stopped_api = stopped.Table();
            Capture established;
            const bool established_open = Open(stopped, stopped_api, empty_path, established);
            const auto stopped_target = stopped.ResolveTransferTarget(established.directory);
            Check(established_open && established.status == PULSE_OK && stopped_target && stopped_target->valid(),
                  "live owner provides an established directory transfer target lease");
            Capture disabled;
            stopped_api.open(stopped_api.context, View(text), Capture::Done, &disabled, &request);
            live = false;
            Check(stopped_target && !stopped_target->valid() && !stopped.ResolveTransferTarget(established.directory),
                  "owner deactivation immediately invalidates the directory transfer target lease");
            stopped.Poll(); Sleep(50); stopped.Poll();
            Check(!disabled.calls && stopped_api.open(stopped_api.context, View(text), Capture::Done, &disabled, &request) == PULSE_ERROR_STATE,
                  "owner/package deactivation cuts off worker completions and future calls");
        }
        {
            DirectoriesAdapter self_close(*caller, backend);
            const auto closing_api = self_close.Table();
            Capture closing;
            bool active_inside = false;
            PulseStatus close_inside = -1;
            closing.hook = [&] { active_inside = self_close.CallbackActive(); close_inside = self_close.Close(); };
            Check(Open(self_close, closing_api, empty_path, closing) && active_inside && close_inside == PULSE_ERROR_STATE &&
                  !self_close.CallbackActive() && self_close.Close() == PULSE_OK,
                  "callback can close adapter while unload remains deferred until callback returns");
        }
        Capture open_cancelled;
        const auto empty_text = Utf8(empty_path);
        api.open(api.context, View(empty_text), Capture::Done, &open_cancelled, &request);
        api.cancel(api.context, request);
        Sleep(50); adapter.Poll();
        Check(!open_cancelled.calls, "cancelled asynchronous open never invokes plugin code");
        Changes closing_watch;
        Capture closing_directory;
        Open(adapter, api, empty_path, closing_directory);
        PulseStatus directory_close_inside = -1;
        closing_watch.hook = [&](uint32_t kind) {
            if (kind == PULSE_DIRECTORY_WATCH_READY)
                directory_close_inside = api.close(api.context, closing_directory.directory);
        };
        uint64_t closing_watch_id = 0;
        api.watch(api.context, closing_directory.directory, Changes::Done, &closing_watch, &closing_watch_id);
        Check(Pump(adapter, [&] { return closing_watch.ready || closing_watch.errors; }) && closing_watch.ready == 1 &&
              directory_close_inside == PULSE_OK, "watch READY callback can close its own directory safely");
        Capture current_rows;
        uint32_t offset = 0;
        uint64_t current_folder_id = 0;
        for (unsigned page_index = 0; page_index < 2 && !current_folder_id; ++page_index) {
            current_rows = {};
            api.list(api.context, directory, snapshot, offset, 256, Capture::Done, &current_rows, &request);
            if (!Pump(adapter, [&] { return current_rows.calls != 0; })) break;
            const auto current_folder = std::find_if(current_rows.rows.begin(), current_rows.rows.end(),
                [](const Row& row) { return row.name == "folder"; });
            if (current_folder != current_rows.rows.end()) current_folder_id = current_folder->id;
            offset += static_cast<uint32_t>(current_rows.rows.size());
        }
        const auto closing_tree_source = current_folder_id
            ? adapter.ResolveTreeTransferSource(directory, snapshot, current_folder_id)
            : std::optional<DirectoryTreeTransferSource>{};
        Check(closing_tree_source && closing_tree_source->valid(),
              "current snapshot yields an established tree-source lease before adapter close");
        const auto stop_start = GetTickCount64();
        Check(adapter.Close() == PULSE_OK && GetTickCount64() - stop_start < 200,
              "adapter stop does not join or wait for filesystem workers on the UI thread");
        Check(transfer_target && !transfer_target->valid() && !adapter.ResolveTransferTarget(directory),
              "adapter close invalidates established directory transfer target leases");
        Check(closing_tree_source && !closing_tree_source->valid(),
              "adapter close invalidates established directory tree-source leases");
        if (argc > 1 && std::wstring_view(argv[1]) == L"--bounds") {
            DirectoriesAdapter bounds(*caller, backend);
            const auto bounds_api = bounds.Table();
            const auto entry_path = temp.path + L"\\entry-bound";
            CreateDirectoryW(entry_path.c_str(), nullptr);
            bool created = true;
            for (unsigned index = 0; index < 65537; ++index)
                created = File(entry_path + L"\\" + std::to_wstring(index)) && created;
            Capture over;
            Check(created && Open(bounds, bounds_api, entry_path, over) && over.status == PULSE_ERROR_STATE &&
                  over.error == "directory_limit_exceeded" && !over.directory && !over.total,
                  "65537 real entries fail the snapshot atomically instead of truncating");
            const auto names_path = temp.path + L"\\name-bound";
            CreateDirectoryW(names_path.c_str(), nullptr);
            created = true;
            const std::wstring prefix(250, L'测');
            for (unsigned index = 0; index < 22500; ++index)
                created = File(L"\\\\?\\" + names_path + L"\\" + prefix + std::to_wstring(index)) && created;
            Capture names_over;
            Check(created && Open(bounds, bounds_api, names_path, names_over) && names_over.status == PULSE_ERROR_STATE &&
                  names_over.error == "directory_name_limit_exceeded" && !names_over.total,
                  "over 16 MiB of real UTF-8 names fails atomically instead of truncating");
            bounds.Close();
        }
#if defined(PULSE_DIRECTORIES_TEST)
        {
            DirectoriesAdapter exhausted(*caller, backend);
            const auto exhausted_api = exhausted.Table();
            Capture directory_before_exhaustion;
            const bool opened_before = Open(exhausted, exhausted_api, empty_path, directory_before_exhaustion);
            ExhaustDirectoryIdsForTest();
            const auto id = directory_before_exhaustion.directory;
            const auto snap = directory_before_exhaustion.snapshot;
            Changes exhausted_watch;
            Check(opened_before && exhausted_api.open(exhausted_api.context, View(empty_text), Capture::Done, &rejected, &request) == PULSE_ERROR_STATE &&
                  exhausted_api.list(exhausted_api.context, id, snap, 0, 1, Capture::Done, &rejected, &request) == PULSE_ERROR_STATE &&
                  exhausted_api.refresh(exhausted_api.context, id, Capture::Done, &rejected, &request) == PULSE_ERROR_STATE &&
                  exhausted_api.watch(exhausted_api.context, id, Changes::Done, &exhausted_watch, &request) == PULSE_ERROR_STATE && !request,
                  "exhausted global IDs fail directory, request and watch allocations instead of wrapping or reusing handles");
            exhausted.Close();
        }
#endif
    } catch (const std::exception &error) {
        std::printf("[FAIL] fixture exception: %s\n", error.what()); ++failures;
    }
    std::printf("Checks: %d; failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}
