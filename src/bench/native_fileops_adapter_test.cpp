#include <windows.h>
#include "../app/extensions/native/fileops_adapter.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

using namespace pulse::extensions;
using namespace pulse::extensions::native;
using namespace pulse::extensions::authorization;

namespace {
int failed = 0;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    failed += !value;
}
struct Fixture {
    Registry registry{{}, [] { return 100u; }, [](std::span<uint8_t> out) {
        static uint8_t next = 1;
        std::fill(out.begin(), out.end(), next++);
        return true;
    }};
    PackageIdentity package{0, "fileops.test", "revision", 1};
    OwnerHandle owner;
    PrincipalBinding binding;
    bool live = true;
    uint64_t last_directory = 0;
    uint64_t next_id = 41;
    uint32_t state = PULSE_FILEOPS_PREPARING;
    uint64_t last_items_request = 0;
    uint32_t cancel_calls = 0;
    uint32_t release_calls = 0;
    uint32_t consume_calls = 0;
    uint64_t consumed_request = 0;
    std::unordered_map<uint64_t, CallerBinding> retained;
    Fixture() : owner(Setup()), binding(*registry.CaptureBinding(package, owner)) {}
    OwnerHandle Setup() {
        registry.SyncPackages({{package, true}});
        return *registry.RegisterOwner(77);
    }
    FileOpsBackend Backend() {
        FileOpsBackend backend;
        backend.live = [this](const CallerBinding& value) { return live && registry.ValidateBinding(value); };
        backend.submit = [this](const CallerBinding& caller, uint32_t, PulseFileOperationRequestId* id) {
            *id = next_id++;
            retained.emplace(*id, caller);
            return PULSE_OK;
        };
        backend.get = [this](const CallerBinding& caller, PulseFileOperationRequestId id, PulseFileOperation* out) {
            const auto found = retained.find(id);
            if (found == retained.end() || found->second.Package() != caller.Package() ||
                found->second.Owner() != caller.Owner()) return PULSE_ERROR_STATE;
            std::memset(out, 0, sizeof(*out));
            out->size = sizeof(*out);
            out->request_id = id;
            out->state = state;
            out->task_id = state >= PULSE_FILEOPS_QUEUED ? 900 : 0;
            out->transferred_bytes = 12;
            out->total_bytes = 20;
            out->completed_items = state >= PULSE_FILEOPS_QUEUED ? 2 : 1;
            out->total_items = 2;
            out->percent = 60;
            out->terminal = state >= PULSE_FILEOPS_COMPLETED;
            out->succeeded = state == PULSE_FILEOPS_COMPLETED;
            return PULSE_OK;
        };
        backend.cancel = [this](const CallerBinding&, PulseFileOperationRequestId) {
            ++cancel_calls;
            state = PULSE_FILEOPS_CANCELLING;
            return PULSE_OK;
        };
        backend.release = [this](const CallerBinding& caller) {
            ++release_calls;
            std::erase_if(retained, [&](const auto& item) {
                return item.second.Package() == caller.Package() && item.second.Owner() == caller.Owner();
            });
        };
        backend.consume = [this](const CallerBinding& caller, PulseFileOperationRequestId id) {
            const auto found = retained.find(id);
            if (found != retained.end() && found->second.Package() == caller.Package() && found->second.Owner() == caller.Owner()) {
                ++consume_calls;
                consumed_request = id;
                retained.erase(found);
            }
        };
        backend.submit_to_directory = [this](const CallerBinding& caller, uint32_t, PulseDirectoryId directory,
            PulseFileOperationRequestId* id) {
            if (directory != 123) return PULSE_ERROR_STATE;
            last_directory = directory;
            *id = next_id++;
            retained.emplace(*id, caller);
            return PULSE_OK;
        };
        backend.submit_entries = [this](const CallerBinding& caller, uint32_t, PulseDirectoryId source,
            uint64_t snapshot, const uint64_t* entries, uint32_t count, PulseDirectoryId destination,
            PulseFileOperationRequestId* id) {
            if (source != 321 || snapshot != 654 || !entries || !count || destination != 123) return PULSE_ERROR_STATE;
            last_directory = destination;
            *id = next_id++;
            retained.emplace(*id, caller);
            return PULSE_OK;
        };
        backend.submit_directory_tree = [this](const CallerBinding& caller, PulseDirectoryId source,
            uint64_t snapshot, uint64_t entry, PulseDirectoryId destination, PulseFileOperationRequestId* id) {
            if (source != 321 || snapshot != 654 || entry != 9 || destination != 123) return PULSE_ERROR_STATE;
            *id = next_id++;
            retained.emplace(*id, caller);
            return PULSE_OK;
        };
        backend.get_item = [this](const CallerBinding&, PulseFileOperationRequestId id, uint32_t index,
            char* path, uint32_t capacity, PulseFileOperationItem* out) {
            last_items_request = id;
            static constexpr char kPath[] = "nested\\file.txt";
            out->size = sizeof(*out); out->index = index; out->state = PULSE_FILEOPS_ITEM_COMPLETED;
            out->path_bytes = sizeof(kPath); out->transferred_bytes = 7;
            if (!path || capacity < sizeof(kPath)) return PULSE_ERROR_ARGUMENT;
            std::memcpy(path, kPath, sizeof(kPath));
            return PULSE_OK;
        };
        return backend;
    }
};
}

int main() {
    static_assert(std::is_standard_layout_v<PulseFileOpsV1> && std::is_trivially_copyable_v<PulseFileOperation>);
    static_assert(sizeof(void*) != 8 || sizeof(PulseFileOpsV1) == 40);
    static_assert(sizeof(void*) != 8 || sizeof(PulseFileOpsV2) == 48);
    static_assert(sizeof(void*) != 8 || sizeof(PulseFileOpsV3) == 56);
    static_assert(sizeof(void*) != 8 || sizeof(PulseFileOpsV4) == 72);
    static_assert(offsetof(PulseFileOpsV2, submit_to_directory) == sizeof(PulseFileOpsV1));
    static_assert(offsetof(PulseFileOpsV3, submit_to_directory) == offsetof(PulseFileOpsV2, submit_to_directory));
    static_assert(offsetof(PulseFileOpsV3, submit_entries) == sizeof(PulseFileOpsV2));
    static_assert(offsetof(PulseFileOpsV4, submit_directory_tree) == sizeof(PulseFileOpsV3));
    Fixture fixture;
    FileOpsAdapter adapter(fixture.binding, fixture.Backend());
    PulseFileOpsV1 table{};
    Check(adapter.Query(PULSE_FILEOPS_VERSION_1, &table, sizeof(table)) == PULSE_OK, "query version one table");
    Check(table.submit && table.get && table.cancel, "all operation functions published");
    Check(adapter.Query(PULSE_FILEOPS_VERSION_1, &table, sizeof(table) - 1) == PULSE_ERROR_ARGUMENT, "small table rejected");
    Check(adapter.Query(99, &table, sizeof(table)) == PULSE_ERROR_ABI, "unknown ABI version rejected");

    PulseFileOperationRequestId request = 777;
    Check(table.submit(table.context, 99, &request) == PULSE_ERROR_ARGUMENT && request == 0, "unknown operation rejected and clears ID");
    Check(table.submit(table.context, PULSE_FILEOPS_COPY_SELECTION_TO_OTHER_PANE, &request) == PULSE_OK && request == 41,
          "native copy needs no permission declaration and keeps opaque request ID");

    PulseFileOperation out{};
    out.size = sizeof(out) - 1;
    Check(table.get(table.context, request, &out) == PULSE_ERROR_ARGUMENT, "small result record rejected");
    out.size = sizeof(out);
    Check(table.get(table.context, request + 1, &out) == PULSE_ERROR_STATE, "foreign request ID rejected");
    Check(table.get(table.context, request, &out) == PULSE_OK && out.state == PULSE_FILEOPS_PREPARING &&
          out.transferred_bytes == 12 && out.total_bytes == 20 && out.percent == 60,
          "request status and progress returned");
    Check(fixture.consume_calls == 0 && fixture.retained.contains(request), "nonterminal reads preserve backend retention");
    Check(table.cancel(table.context, request) == PULSE_OK && fixture.cancel_calls == 1,
          "owned request cancellation delegated");
    fixture.state = PULSE_FILEOPS_COMPLETED;
    Check(table.get(table.context, request, &out) == PULSE_OK && out.terminal && out.succeeded && out.task_id == 900,
          "completed task reaches terminal snapshot");
    Check(fixture.consume_calls == 1 && fixture.consumed_request == request && !fixture.retained.contains(request),
          "terminal get acknowledges the exact backend request once");
    Check(table.get(table.context, request, &out) == PULSE_ERROR_STATE, "terminal read releases request slot");

    Check(table.submit(table.context, PULSE_FILEOPS_DELETE_SELECTION_TO_RECYCLE_BIN, &request) == PULSE_OK && request == 42,
          "native recycle delete needs no permission declaration");

    request = 0;
    Check(table.submit(table.context, PULSE_FILEOPS_MOVE_SELECTION_TO_OTHER_PANE, &request) == PULSE_OK && request == 43,
          "move request submitted");
    fixture.live = false;
    Check(adapter.Close() == PULSE_OK && fixture.cancel_calls == 3 && fixture.release_calls == 1,
          "revoked attachment close cancels and releases remaining owned requests");
    Check(fixture.retained.empty() && fixture.consume_calls == 1, "revoked close releases unread requests without consuming them");
    Check(adapter.Close() == PULSE_OK && fixture.release_calls == 1, "close releases backend exactly once");
    Check(table.get(table.context, request, &out) == PULSE_ERROR_STATE, "closed adapter rejects later operations");

    Fixture wrong_thread_fixture;
    FileOpsAdapter wrong_thread(wrong_thread_fixture.binding, wrong_thread_fixture.Backend());
    PulseFileOpsV1 wrong_table{};
    wrong_thread.Query(PULSE_FILEOPS_VERSION_1, &wrong_table, sizeof(wrong_table));
    PulseStatus wrong_thread_status = PULSE_OK;
    std::thread worker([&] { wrong_thread_status = wrong_table.get(wrong_table.context, 1, &out); });
    worker.join();
    Check(wrong_thread_status == PULSE_ERROR_STATE, "wrong-thread call rejected");

    Fixture v2_fixture;
    FileOpsAdapter v2_adapter(v2_fixture.binding, v2_fixture.Backend());
    PulseFileOpsV2 v2{};
    Check(v2_adapter.Query(2, &v2, sizeof(v2)) == PULSE_OK && v2.version == 2 && v2.size == sizeof(v2) &&
        v2.submit && v2.get && v2.cancel && v2.submit_to_directory, "query V2 publishes additive directory submit");
    Check(v2_adapter.Query(2, &table, sizeof(table)) == PULSE_ERROR_ARGUMENT, "V2 cannot overwrite a V1-sized table");
    request = 999;
    Check(v2.submit(v2.context, PULSE_FILEOPS_COPY_SELECTION_TO_DIRECTORY, &request) == PULSE_ERROR_ARGUMENT && request == 0,
        "V1 submit never silently accepts explicit-directory actions");
    Check(v2.submit_to_directory(v2.context, PULSE_FILEOPS_COPY_SELECTION_TO_OTHER_PANE, 123, &request) == PULSE_ERROR_ARGUMENT && !request,
        "directory submit rejects other-pane operation");
    Check(v2.submit_to_directory(v2.context, PULSE_FILEOPS_COPY_SELECTION_TO_DIRECTORY, 0, &request) == PULSE_ERROR_ARGUMENT,
        "zero directory ID rejected");
    Check(v2.submit_to_directory(v2.context, 4, 124, &request) == PULSE_ERROR_STATE && !request,
        "unknown or foreign directory rejected by backend");
    Check(v2.submit_to_directory(v2.context, 4, 123, &request) == PULSE_OK && request == 41 && v2_fixture.last_directory == 123,
        "copy uses opaque owned directory without resource declarations");
    Check(v2.get(v2.context, request, &out) == PULSE_OK && out.request_id == request,
        "V2 shares owned status and progress");
    Check(v2.cancel(v2.context, request) == PULSE_OK, "V2 exact request cancellation delegated");
    v2_fixture.state = PULSE_FILEOPS_CANCELLED;
    Check(v2.get(v2.context, request, &out) == PULSE_OK && out.terminal && !out.succeeded,
        "V2 cancelled request releases its slot");
    Check(v2.submit_to_directory(v2.context, 5, 123, &request) == PULSE_OK && request == 42,
        "move uses same opaque destination API");
    PulseStatus thread_submit = PULSE_OK;
    std::thread v2_worker([&] { uint64_t id = 777; thread_submit = v2.submit_to_directory(v2.context, 4, 123, &id); });
    v2_worker.join();
    Check(thread_submit == PULSE_ERROR_STATE, "V2 directory submit rejects wrong thread");
    Check(v2_adapter.Close() == PULSE_OK && v2_fixture.cancel_calls == 2 && v2_fixture.release_calls == 1,
        "V2 attachment close cancels its outstanding destination request once");

    Fixture v3_fixture;
    FileOpsAdapter v3_adapter(v3_fixture.binding, v3_fixture.Backend());
    PulseFileOpsV3 v3{};
    Check(v3_adapter.Query(3, &v3, sizeof(v3)) == PULSE_OK && v3.version == 3 && v3.submit_entries,
        "query V3 publishes additive explicit-entry submit");
    const uint64_t entry_ids[] = {9, 10};
    request = 999;
    Check(v3.submit_entries(v3.context, PULSE_FILEOPS_COPY_ENTRIES_TO_DIRECTORY, 0, 654, entry_ids, 2, 123, &request) == PULSE_ERROR_ARGUMENT && !request,
        "V3 rejects zero source directory");
    Check(v3.submit_entries(v3.context, PULSE_FILEOPS_COPY_ENTRIES_TO_DIRECTORY, 321, 654, entry_ids, 2, 123, &request) == PULSE_OK && request == 41,
        "V3 submits snapshot entry IDs through the host backend");
    request = 999;
    Check(v3.submit_entries(v3.context, PULSE_FILEOPS_COPY_ENTRIES_TO_DIRECTORY, 321, 654, entry_ids, 65, 123, &request) == PULSE_ERROR_ARGUMENT && !request,
        "V3 enforces the 64-entry limit");
    Check(v3_adapter.Close() == PULSE_OK && v3_fixture.cancel_calls == 1 && v3_fixture.release_calls == 1,
        "V3 attachment close cancels and releases its request");

    Fixture v4_fixture;
    FileOpsAdapter v4_adapter(v4_fixture.binding, v4_fixture.Backend());
    PulseFileOpsV4 v4{};
    Check(v4_adapter.Query(4, &v4, sizeof(v4)) == PULSE_OK && v4.version == 4 && v4.submit_directory_tree && v4.get_item,
        "query V4 publishes recursive submit and per-file result query");
    Check(v4_adapter.Query(4, &v3, sizeof(v3)) == PULSE_ERROR_ARGUMENT,
        "V4 cannot overwrite a V3-sized table");
    request = 999;
    Check(v4.submit_directory_tree(v4.context, 0, 654, 9, 123, &request) == PULSE_ERROR_ARGUMENT && !request,
        "V4 rejects zero source handles");
    Check(v4.submit_directory_tree(v4.context, 321, 654, 9, 123, &request) == PULSE_OK && request == 41,
        "V4 submits current directory entry through same-attachment handles");
    v4_fixture.state = PULSE_FILEOPS_COMPLETED;
    out = {}; out.size = sizeof(out);
    Check(v4.get(v4.context, request, &out) == PULSE_OK && out.terminal,
        "V4 terminal operation result remains retained while item details are read");
    PulseFileOperationItem item{}; item.size = sizeof(item);
    Check(v4.get_item(v4.context, request, 1, nullptr, 0, &item) == PULSE_ERROR_ARGUMENT && item.path_bytes == 16,
        "V4 item path size query reports required UTF-8 storage");
    char item_path[16]{};
    Check(v4.get_item(v4.context, request, 1, item_path, sizeof(item_path), &item) == PULSE_OK &&
        std::string_view(item_path) == "nested\\file.txt" && item.state == PULSE_FILEOPS_ITEM_COMPLETED && item.transferred_bytes == 7,
        "V4 returns exact relative path and per-file terminal state");
    Check(v4.get(v4.context, request, &out) == PULSE_ERROR_STATE && !v4_fixture.retained.contains(request),
        "reading final V4 file item consumes its retained request");
    Check(v4_adapter.Close() == PULSE_OK && v4_fixture.release_calls == 1,
        "V4 attachment closes cleanly after item result consumption");

    Fixture bounded;
    FileOpsAdapter bounded_adapter(bounded.binding, bounded.Backend());
    const auto bounded_table = bounded_adapter.Table();
    std::vector<uint64_t> ids;
    for (int i = 0; i < 32; ++i) {
        uint64_t id = 0;
        if (bounded_table.submit(bounded_table.context, PULSE_FILEOPS_COPY_SELECTION_TO_OTHER_PANE, &id) != PULSE_OK)
            break;
        ids.push_back(id);
    }
    request = 999;
    Check(ids.size() == 32 && bounded_table.submit(bounded_table.context,
        PULSE_FILEOPS_COPY_SELECTION_TO_OTHER_PANE, &request) == PULSE_ERROR_STATE && !request && bounded.retained.size() == 32,
        "32 unread requests bound backend retention and reject the next submission");
    auto other_package = bounded.package; other_package.slot = 1; other_package.id = "other.fileops";
    bounded.registry.SyncPackages({{bounded.package, true}, {other_package, true}});
    const auto other_binding = *bounded.registry.CaptureBinding(other_package, bounded.owner);
    FileOpsAdapter foreign_adapter(other_binding, bounded.Backend());
    const auto foreign_table = foreign_adapter.Table();
    Check(ids.size() == 32 && foreign_table.get(foreign_table.context, ids.front(), &out) == PULSE_ERROR_STATE &&
        foreign_table.cancel(foreign_table.context, ids.front()) == PULSE_ERROR_STATE && bounded.consume_calls == 0,
        "another package cannot read cancel or consume the unread request");
    foreign_adapter.Close();
    Check(bounded.retained.size() == 32, "another package close preserves all retained owner requests");
    bounded.state = PULSE_FILEOPS_COMPLETED;
    Check(ids.size() == 32 && bounded_table.get(bounded_table.context, ids.front(), &out) == PULSE_OK &&
        out.terminal && bounded.retained.size() == 31 && bounded.consume_calls == 1,
        "oldest unread terminal remains consumable after more than sixteen later requests");
    Check(bounded_table.submit(bounded_table.context, PULSE_FILEOPS_MOVE_SELECTION_TO_OTHER_PANE, &request) == PULSE_OK &&
        bounded.retained.size() == 32, "consuming a terminal result makes one bounded submission slot available");
    bounded.live = false;
    bounded_adapter.Close();
    Check(bounded.retained.empty() && bounded.cancel_calls == 32 && bounded.consume_calls == 1,
        "revoked close cancels and releases exactly the remaining unread requests");

    std::printf("Checks complete; failures=%d\n", failed);
    return failed ? 1 : 0;
}
