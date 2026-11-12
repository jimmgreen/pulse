#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../app/extensions/authorization.h"
#include "../app/extensions/native/commands_adapter.h"
#include "../app/extensions/native/fileops_adapter.h"
#include "../app/extensions/native/module.h"
#include "../app/extensions/native/tasks_adapter.h"
#include "../app/extensions/package_revision.h"

#include <pulse/client.hpp>
#include <pulse/fileops.h>
#include <pulse/ui_mount.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
    }
}

struct EventHandle {
    HANDLE handle = nullptr;

    explicit EventHandle(bool manual_reset = true, bool initial_state = false) noexcept
        : handle(CreateEventW(nullptr, manual_reset ? TRUE : FALSE, initial_state ? TRUE : FALSE, nullptr)) {}

    ~EventHandle() {
        if (handle) {
            CloseHandle(handle);
        }
    }

    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;

    void Signal() const noexcept {
        if (handle) {
            SetEvent(handle);
        }
    }

    bool Wait(DWORD timeout_ms = 5000) const noexcept {
        return handle && WaitForSingleObject(handle, timeout_ms) == WAIT_OBJECT_0;
    }
};

struct ScratchDirectory {
    fs::path root;

    ScratchDirectory() {
        const std::string tag = "tmp_compat_" + std::to_string(GetCurrentProcessId()) + "_" +
                                std::to_string(GetTickCount64());
        root = fs::absolute(fs::path("bench_data") / tag);
        fs::create_directories(root);
    }

    ~ScratchDirectory() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
};

#pragma pack(push, 8)
enum CompatFixtureMode : uint32_t {
    COMPAT_MODE_NORMAL_PROBE = 0u,
    COMPAT_MODE_EXTENDED_PLUGIN_SIZE = 1u,
    COMPAT_MODE_UNDERSIZED_PLUGIN_SIZE = 2u,
    COMPAT_MODE_WRONG_PLUGIN_VERSION = 3u,
    COMPAT_MODE_DEGRADATION_FALLBACK = 4u,
};

struct CompatSharedState {
    uint32_t mode;
    uint32_t observed_host_size;
    uint32_t observed_host_version;
    int32_t entry_calls;
    int32_t activate_calls;
    int32_t deactivate_calls;
    int32_t destroy_calls;
    PulseStatus ui_mount_status;
    PulseStatus future_commands_status;
    PulseStatus unknown_quantum_status;
    uint32_t ui_mount_nullopt;
    uint32_t future_commands_nullopt;
    uint32_t unknown_quantum_nullopt;
    PulseStatus commands_v1_status;
    PulseStatus tasks_v1_status;
    PulseStatus register_cmd_status;
    PulseStatus register_collect_cmd_status;
    uint64_t registered_command_id;
    int32_t fallback_cmd_invocations;
    uint64_t fallback_task_id;
    std::atomic<int32_t> fallback_task_runs;
    uint64_t collected_tasks;
};

struct CompatControlTableV1 {
    uint32_t size;
    uint32_t version;
    CompatSharedState* state;
};
#pragma pack(pop)

static_assert(sizeof(CompatControlTableV1) == 16);

constexpr std::string_view kCompatControlInterface = "test.compat.control";
constexpr uint32_t kCompatControlVersion = 1u;

bool NameEquals(PulseStringView view, std::string_view expected) noexcept {
    return view.data != nullptr && view.length == expected.size() &&
           std::memcmp(view.data, expected.data(), expected.size()) == 0;
}

std::string ReadBinaryFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct SelectionProvider {
    uint64_t revision = 7;
    std::string directory = "C:\\bench_compat";
    std::string entry_path = "C:\\bench_compat\\archive.7z";
    std::string entry_name = "archive.7z";

    static PulseStatus PULSE_CALL GetSummary(void* user, PulseContextChangeFn callback, void* ctx) noexcept {
        if (!user || !callback) {
            return PULSE_ERROR_ARGUMENT;
        }
        const auto* self = static_cast<SelectionProvider*>(user);
        PulseContextSnapshot snap{};
        snap.size = sizeof(snap);
        snap.selection_revision = self->revision;
        snap.loading = 0;
        callback(ctx, &snap);
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL GetSelection(void* user, PulseContextSelectionFn callback, void* ctx) noexcept {
        if (!user || !callback) {
            return PULSE_ERROR_ARGUMENT;
        }
        const auto* self = static_cast<SelectionProvider*>(user);
        const PulseContextSelectionEntry row{
            sizeof(row),
            0u,
            pulse::sdk::View(self->entry_path),
            pulse::sdk::View(self->entry_name),
        };
        const PulseContextSelectionSnapshot selection{
            sizeof(selection),
            self->revision,
            pulse::sdk::View(self->directory),
            &row,
            1u,
        };
        callback(ctx, &selection);
        return PULSE_OK;
    }

    PulseContextV1 Table() noexcept {
        PulseContextV1 table{};
        table.size = sizeof(table);
        table.version = PULSE_CONTEXT_VERSION_1;
        table.context = this;
        table.get = &GetSummary;
        table.get_selection = &GetSelection;
        return table;
    }
};

struct HeadlessHostContext {
    CompatSharedState* shared = nullptr;
    pulse::extensions::native::FileOpsAdapter* fileops = nullptr;
    pulse::extensions::native::CommandsAdapter* commands = nullptr;
    pulse::extensions::native::TasksAdapter* tasks = nullptr;

    static uint32_t ExpectedFileOpsSize(uint32_t version) noexcept {
        switch (version) {
        case PULSE_FILEOPS_VERSION_1:
            return sizeof(PulseFileOpsV1);
        case PULSE_FILEOPS_VERSION_2:
            return sizeof(PulseFileOpsV2);
        case PULSE_FILEOPS_VERSION_3:
            return sizeof(PulseFileOpsV3);
        case PULSE_FILEOPS_VERSION_4:
            return sizeof(PulseFileOpsV4);
        default:
            return 0;
        }
    }

    static PulseStatus PULSE_CALL QueryInterface(void* raw_ctx,
                                                 PulseStringView name,
                                                 uint32_t version,
                                                 void* out_table,
                                                 uint32_t out_size) noexcept {
        auto* self = static_cast<HeadlessHostContext*>(raw_ctx);
        if (!self || !out_table) {
            return PULSE_ERROR_ARGUMENT;
        }
        if (NameEquals(name, kCompatControlInterface)) {
            if (version != kCompatControlVersion || out_size < sizeof(CompatControlTableV1)) {
                return PULSE_ERROR_ABI;
            }
            if (!self->shared) {
                return PULSE_ERROR_UNAVAILABLE;
            }
            const CompatControlTableV1 table{sizeof(table), kCompatControlVersion, self->shared};
            std::memcpy(out_table, &table, sizeof(table));
            return PULSE_OK;
        }
        if (NameEquals(name, PULSE_FILEOPS_INTERFACE_NAME)) {
            if (!self->fileops) {
                return PULSE_ERROR_UNAVAILABLE;
            }
            const uint32_t required = ExpectedFileOpsSize(version);
            if (required == 0 || out_size < required) {
                return PULSE_ERROR_ABI;
            }
            return self->fileops->Query(version, out_table, out_size);
        }
        if (NameEquals(name, PULSE_COMMANDS_INTERFACE_NAME)) {
            if (!self->commands) {
                return PULSE_ERROR_UNAVAILABLE;
            }
            if (version != PULSE_COMMANDS_VERSION_1 || out_size < sizeof(PulseCommandsV1)) {
                return PULSE_ERROR_ABI;
            }
            return self->commands->Query(version, out_table, out_size);
        }
        if (NameEquals(name, PULSE_TASKS_INTERFACE_NAME)) {
            if (!self->tasks) {
                return PULSE_ERROR_UNAVAILABLE;
            }
            if (version != PULSE_TASKS_VERSION_1 || out_size < sizeof(PulseTasksV1)) {
                return PULSE_ERROR_ABI;
            }
            return self->tasks->Query(version, out_table, out_size);
        }
        // Headless host intentionally omits experimental UI surfaces such as pulse.ui.mount.
        return PULSE_ERROR_UNAVAILABLE;
    }
};

// ============================================================================
// Dimension 1: PulseHostV1 / PulsePluginV1 size & version negotiation
// ============================================================================
void TestDimension1HostAndPluginNegotiation(const fs::path& fixture_dll, const fs::path& scratch_dir) {
    using pulse::extensions::native::Module;

    // 1.1 Extended PulseHostV1 (sizeof(PulseHostV1) + 32) clamped to sizeof(PulseHostV1) (24 bytes)
    {
        CompatSharedState shared{};
        shared.mode = COMPAT_MODE_NORMAL_PROBE;
        HeadlessHostContext host_ctx{&shared, nullptr, nullptr, nullptr};

        #pragma pack(push, 8)
        struct ExtendedHost {
            PulseHostV1 base;
            uint8_t future_fields[32];
        };
        #pragma pack(pop)

        ExtendedHost ext{};
        std::memset(ext.future_fields, 0xCC, sizeof(ext.future_fields));
        ext.base.size = static_cast<uint32_t>(sizeof(ExtendedHost));
        ext.base.version = PULSE_ABI_VERSION_1;
        ext.base.context = &host_ctx;
        ext.base.query_interface = &HeadlessHostContext::QueryInterface;

        Module mod;
        const PulseStatus load_status = mod.Load(fixture_dll.wstring(), ext.base);
        Check(load_status == PULSE_OK && mod.Loaded(),
              "D1.1: Extended PulseHostV1 (size = sizeof + 32) loads cleanly");
        Check(shared.observed_host_size == sizeof(PulseHostV1) &&
                  shared.observed_host_version == PULSE_ABI_VERSION_1,
              "D1.2: Module::Load clamps host_.size passed to plugin entry to sizeof(PulseHostV1) (24 bytes)");
        Check(mod.Activate() == PULSE_OK && shared.activate_calls == 1 &&
                  mod.Deactivate() == PULSE_OK && shared.deactivate_calls == 1 &&
                  mod.Close() == PULSE_OK && shared.destroy_calls == 1 && !mod.Loaded(),
              "D1.3: Extended-host loaded fixture activates, deactivates, and destroys once");
    }

    // 1.2 Verified load path also clamps extended PulseHostV1 and releases file lock on Close
    {
        const fs::path verified_copy = scratch_dir / "compat_verified.dll";
        fs::copy_file(fixture_dll, verified_copy, fs::copy_options::overwrite_existing);
        const std::string dll_bytes = ReadBinaryFile(verified_copy);
        const std::string sha256 = pulse::extensions::PackageRevision(dll_bytes);

        CompatSharedState shared{};
        shared.mode = COMPAT_MODE_NORMAL_PROBE;
        HeadlessHostContext host_ctx{&shared, nullptr, nullptr, nullptr};
        PulseHostV1 extended_host{
            static_cast<uint32_t>(sizeof(PulseHostV1) + 32u),
            PULSE_ABI_VERSION_1,
            &host_ctx,
            &HeadlessHostContext::QueryInterface,
        };

        Module mod;
        Check(!sha256.empty() && mod.LoadVerified(verified_copy.wstring(), sha256, extended_host) == PULSE_OK &&
                  shared.observed_host_size == sizeof(PulseHostV1) && mod.Close() == PULSE_OK,
              "D1.4: Module::LoadVerified clamps extended PulseHostV1 size and unloads cleanly");
    }

    // 1.3 Extended PulsePluginV1 (sizeof(PulsePluginV1) + 24) loads cleanly on V1 host
    {
        CompatSharedState shared{};
        shared.mode = COMPAT_MODE_EXTENDED_PLUGIN_SIZE;
        HeadlessHostContext host_ctx{&shared, nullptr, nullptr, nullptr};
        const PulseHostV1 host{sizeof(PulseHostV1), PULSE_ABI_VERSION_1, &host_ctx, &HeadlessHostContext::QueryInterface};

        Module mod;
        Check(mod.Load(fixture_dll.wstring(), host) == PULSE_OK &&
                  mod.Activate() == PULSE_OK && shared.activate_calls == 1 &&
                  mod.Close() == PULSE_OK && shared.deactivate_calls == 1 && shared.destroy_calls == 1,
              "D1.5: Plugin advertising extended PulsePluginV1 (sizeof + 24) loads and runs cleanly on V1 host");
    }

    // 1.4 Undersized host.size and wrong host.version rejected with PULSE_ERROR_ABI before entry
    {
        CompatSharedState shared{};
        HeadlessHostContext host_ctx{&shared, nullptr, nullptr, nullptr};
        Module mod;

        const PulseHostV1 undersized_host{
            static_cast<uint32_t>(sizeof(PulseHostV1) - 1u),
            PULSE_ABI_VERSION_1,
            &host_ctx,
            &HeadlessHostContext::QueryInterface,
        };
        const PulseHostV1 wrong_ver_zero{
            sizeof(PulseHostV1), 0u, &host_ctx, &HeadlessHostContext::QueryInterface};
        const PulseHostV1 wrong_ver_two{
            sizeof(PulseHostV1), 2u, &host_ctx, &HeadlessHostContext::QueryInterface};

        Check(mod.Load(fixture_dll.wstring(), undersized_host) == PULSE_ERROR_ABI &&
                  mod.Load(fixture_dll.wstring(), wrong_ver_zero) == PULSE_ERROR_ABI &&
                  mod.Load(fixture_dll.wstring(), wrong_ver_two) == PULSE_ERROR_ABI &&
                  !mod.Loaded() && shared.entry_calls == 0,
              "D1.6: Undersized host.size and wrong host.version fail closed with PULSE_ERROR_ABI");
    }

    // 1.5 Undersized plugin->size and wrong plugin->version fail with PULSE_ERROR_ABI AND destroy(instance) once
    {
        CompatSharedState undersized_state{};
        undersized_state.mode = COMPAT_MODE_UNDERSIZED_PLUGIN_SIZE;
        HeadlessHostContext ctx_under{&undersized_state, nullptr, nullptr, nullptr};
        const PulseHostV1 host_under{
            sizeof(PulseHostV1), PULSE_ABI_VERSION_1, &ctx_under, &HeadlessHostContext::QueryInterface};

        Module mod;
        const PulseStatus under_status = mod.Load(fixture_dll.wstring(), host_under);
        Check(under_status == PULSE_ERROR_ABI && !mod.Loaded() &&
                  undersized_state.entry_calls == 1 && undersized_state.destroy_calls == 1,
              "D1.7: Undersized plugin->size rejected with PULSE_ERROR_ABI and destroys instance once");

        CompatSharedState wrong_ver_state{};
        wrong_ver_state.mode = COMPAT_MODE_WRONG_PLUGIN_VERSION;
        HeadlessHostContext ctx_ver{&wrong_ver_state, nullptr, nullptr, nullptr};
        const PulseHostV1 host_ver{
            sizeof(PulseHostV1), PULSE_ABI_VERSION_1, &ctx_ver, &HeadlessHostContext::QueryInterface};

        const PulseStatus ver_status = mod.Load(fixture_dll.wstring(), host_ver);
        Check(ver_status == PULSE_ERROR_ABI && !mod.Loaded() &&
                  wrong_ver_state.entry_calls == 1 && wrong_ver_state.destroy_calls == 1 &&
                  wrong_ver_state.activate_calls == 0,
              "D1.8: Wrong plugin->version != 1 rejected with PULSE_ERROR_ABI and invokes destroy(instance) once");
    }
}

// ============================================================================
// Dimension 2: Cross-version PulseFileOps V1..V4 prefix negotiation & canary
// ============================================================================
void TestDimension2FileOpsCrossVersionCanaries() {
    using namespace pulse::extensions::authorization;
    using namespace pulse::extensions::native;

    Registry registry(
        {},
        []() -> uint64_t { return 1000u; },
        [](std::span<uint8_t> out) {
            static uint8_t counter = 1;
            std::fill(out.begin(), out.end(), counter++);
            return true;
        });
    const PackageIdentity pkg{0, "compat.fileops", "rev1", 1};
    registry.SyncPackages({{pkg, true}});
    const OwnerHandle owner = *registry.RegisterOwner(0x701u);
    const PrincipalBinding caller = *registry.CaptureBinding(pkg, owner);

    bool live = true;
    uint64_t next_request_id = 100;
    uint32_t cancel_count = 0;
    std::unordered_map<uint64_t, uint32_t> request_items;

    FileOpsBackend backend;
    backend.live = [&](const pulse::extensions::CallerBinding& b) {
        return live && registry.ValidateBinding(b);
    };
    backend.submit = [&](const pulse::extensions::CallerBinding&, uint32_t, PulseFileOperationRequestId* id) {
        *id = ++next_request_id;
        request_items[*id] = 0;
        return PULSE_OK;
    };
    backend.submit_to_directory = [&](const pulse::extensions::CallerBinding&, uint32_t, PulseDirectoryId,
                                      PulseFileOperationRequestId* id) {
        *id = ++next_request_id;
        request_items[*id] = 0;
        return PULSE_OK;
    };
    backend.submit_entries = [&](const pulse::extensions::CallerBinding&, uint32_t, PulseDirectoryId, uint64_t,
                                 const uint64_t*, uint32_t count, PulseDirectoryId, PulseFileOperationRequestId* id) {
        *id = ++next_request_id;
        request_items[*id] = count;
        return PULSE_OK;
    };
    backend.submit_directory_tree = [&](const pulse::extensions::CallerBinding&, PulseDirectoryId, uint64_t, uint64_t,
                                        PulseDirectoryId, PulseFileOperationRequestId* id) {
        *id = ++next_request_id;
        request_items[*id] = 1;
        return PULSE_OK;
    };
    backend.get = [&](const pulse::extensions::CallerBinding&, PulseFileOperationRequestId id, PulseFileOperation* out) {
        out->state = PULSE_FILEOPS_RUNNING;
        out->request_id = id;
        out->task_id = id + 500;
        out->total_items = request_items[id];
        out->completed_items = 0;
        out->percent = 50;
        out->terminal = 0;
        out->succeeded = 0;
        return PULSE_OK;
    };
    backend.get_item = [&](const pulse::extensions::CallerBinding&, PulseFileOperationRequestId, uint32_t index,
                           char* path_utf8, uint32_t capacity, PulseFileOperationItem* out) {
        if (index != 0) {
            return PULSE_ERROR_ARGUMENT;
        }
        constexpr std::string_view kRelPath = "sub/file.txt";
        const uint32_t needed = static_cast<uint32_t>(kRelPath.size() + 1);
        out->index = 0;
        out->state = PULSE_FILEOPS_ITEM_COMPLETED;
        out->system_error = 0;
        out->transferred_bytes = 64;
        out->path_bytes = needed;
        if (path_utf8 && capacity >= needed) {
            std::memcpy(path_utf8, kRelPath.data(), kRelPath.size());
            path_utf8[kRelPath.size()] = '\0';
        } else if (path_utf8 && capacity < needed) {
            return PULSE_ERROR_ARGUMENT;
        }
        return PULSE_OK;
    };
    backend.cancel = [&](const pulse::extensions::CallerBinding&, PulseFileOperationRequestId) {
        ++cancel_count;
        return PULSE_OK;
    };

    FileOpsAdapter adapter(caller, std::move(backend));
    HeadlessHostContext host_ctx{nullptr, &adapter, nullptr, nullptr};

    constexpr uint8_t kCanary = 0xA5u;
    const auto canaries_intact = [&](const std::array<uint8_t, 128>& buf, size_t start_offset) {
        for (size_t i = start_offset; i < buf.size(); ++i) {
            if (buf[i] != kCanary) {
                return false;
            }
        }
        return true;
    };

    // Query V1
    std::array<uint8_t, 128> buf_v1{};
    buf_v1.fill(kCanary);
    const PulseStatus s_v1 = HeadlessHostContext::QueryInterface(
        &host_ctx, pulse::sdk::View(PULSE_FILEOPS_INTERFACE_NAME), 1u, buf_v1.data(), sizeof(PulseFileOpsV1));
    PulseFileOpsV1 v1{};
    std::memcpy(&v1, buf_v1.data(), sizeof(v1));
    PulseFileOperationRequestId req_v1 = 0;
    PulseFileOperation op_v1{};
    op_v1.size = sizeof(op_v1);
    const bool v1_calls_ok =
        s_v1 == PULSE_OK && v1.size == sizeof(PulseFileOpsV1) && v1.version == 1u &&
        v1.context != nullptr && v1.submit != nullptr && v1.get != nullptr && v1.cancel != nullptr &&
        v1.submit(v1.context, PULSE_FILEOPS_COPY_SELECTION_TO_OTHER_PANE, &req_v1) == PULSE_OK &&
        v1.get(v1.context, req_v1, &op_v1) == PULSE_OK && op_v1.request_id == req_v1 &&
        v1.cancel(v1.context, req_v1) == PULSE_OK;
    Check(v1_calls_ok && canaries_intact(buf_v1, sizeof(PulseFileOpsV1)),
          "D2.1: PulseFileOpsV1 query negotiates exact 40-byte table, executes V1 ops, and preserves 0xA5 canaries");

    // Query V2
    std::array<uint8_t, 128> buf_v2{};
    buf_v2.fill(kCanary);
    const PulseStatus s_v2 = HeadlessHostContext::QueryInterface(
        &host_ctx, pulse::sdk::View(PULSE_FILEOPS_INTERFACE_NAME), 2u, buf_v2.data(), sizeof(PulseFileOpsV2));
    PulseFileOpsV2 v2{};
    std::memcpy(&v2, buf_v2.data(), sizeof(v2));
    PulseFileOperationRequestId req_v2 = 0;
    const bool v2_calls_ok =
        s_v2 == PULSE_OK && v2.size == sizeof(PulseFileOpsV2) && v2.version == 2u &&
        v2.submit == v1.submit && v2.get == v1.get && v2.cancel == v1.cancel &&
        v2.submit_to_directory != nullptr &&
        v2.submit_to_directory(v2.context, PULSE_FILEOPS_COPY_SELECTION_TO_DIRECTORY, 11u, &req_v2) == PULSE_OK &&
        v2.cancel(v2.context, req_v2) == PULSE_OK;
    Check(v2_calls_ok && canaries_intact(buf_v2, sizeof(PulseFileOpsV2)),
          "D2.2: PulseFileOpsV2 query preserves V1 prefix, executes submit_to_directory, and preserves 0xA5 canaries");

    // Query V3
    std::array<uint8_t, 128> buf_v3{};
    buf_v3.fill(kCanary);
    const PulseStatus s_v3 = HeadlessHostContext::QueryInterface(
        &host_ctx, pulse::sdk::View(PULSE_FILEOPS_INTERFACE_NAME), 3u, buf_v3.data(), sizeof(PulseFileOpsV3));
    PulseFileOpsV3 v3{};
    std::memcpy(&v3, buf_v3.data(), sizeof(v3));
    PulseFileOperationRequestId req_v3 = 0;
    const uint64_t entry_id = 77u;
    const bool v3_calls_ok =
        s_v3 == PULSE_OK && v3.size == sizeof(PulseFileOpsV3) && v3.version == 3u &&
        v3.submit == v1.submit && v3.get == v1.get && v3.cancel == v1.cancel &&
        v3.submit_to_directory == v2.submit_to_directory && v3.submit_entries != nullptr &&
        v3.submit_entries(v3.context, PULSE_FILEOPS_COPY_ENTRIES_TO_DIRECTORY, 10u, 20u, &entry_id, 1u, 11u, &req_v3) == PULSE_OK &&
        v3.cancel(v3.context, req_v3) == PULSE_OK;
    Check(v3_calls_ok && canaries_intact(buf_v3, sizeof(PulseFileOpsV3)),
          "D2.3: PulseFileOpsV3 query preserves V2 prefix, executes submit_entries, and preserves 0xA5 canaries");

    // Query V4
    std::array<uint8_t, 128> buf_v4{};
    buf_v4.fill(kCanary);
    const PulseStatus s_v4 = HeadlessHostContext::QueryInterface(
        &host_ctx, pulse::sdk::View(PULSE_FILEOPS_INTERFACE_NAME), 4u, buf_v4.data(), sizeof(PulseFileOpsV4));
    PulseFileOpsV4 v4{};
    std::memcpy(&v4, buf_v4.data(), sizeof(v4));
    PulseFileOperationRequestId req_v4 = 0;
    PulseFileOperationItem item{};
    item.size = sizeof(item);
    char rel_path[32]{};
    const bool v4_calls_ok =
        s_v4 == PULSE_OK && v4.size == sizeof(PulseFileOpsV4) && v4.version == 4u &&
        v4.submit == v1.submit && v4.get == v1.get && v4.cancel == v1.cancel &&
        v4.submit_to_directory == v2.submit_to_directory && v4.submit_entries == v3.submit_entries &&
        v4.submit_directory_tree != nullptr && v4.get_item != nullptr &&
        v4.submit_directory_tree(v4.context, 10u, 20u, 88u, 11u, &req_v4) == PULSE_OK &&
        v4.get_item(v4.context, req_v4, 0u, nullptr, 0u, &item) == PULSE_OK && item.path_bytes == 13u &&
        v4.get_item(v4.context, req_v4, 0u, rel_path, sizeof(rel_path), &item) == PULSE_OK &&
        std::string_view(rel_path) == "sub/file.txt";
    Check(v4_calls_ok && canaries_intact(buf_v4, sizeof(PulseFileOpsV4)) && cancel_count == 3u,
          "D2.4: PulseFileOpsV4 query preserves V3 prefix, executes tree + get_item, and preserves 0xA5 canaries");

    // Negative version & undersized out_size checks
    std::array<uint8_t, 128> neg_buf{};
    neg_buf.fill(kCanary);
    const PulseStatus s_ver0 = HeadlessHostContext::QueryInterface(
        &host_ctx, pulse::sdk::View(PULSE_FILEOPS_INTERFACE_NAME), 0u, neg_buf.data(), sizeof(PulseFileOpsV4));
    const PulseStatus s_ver5 = HeadlessHostContext::QueryInterface(
        &host_ctx, pulse::sdk::View(PULSE_FILEOPS_INTERFACE_NAME), 5u, neg_buf.data(), sizeof(PulseFileOpsV4));
    const PulseStatus s_direct_ver0 = adapter.Query(0u, neg_buf.data(), sizeof(PulseFileOpsV4));
    const PulseStatus s_direct_ver5 = adapter.Query(5u, neg_buf.data(), sizeof(PulseFileOpsV4));

    bool undersized_rejected = true;
    for (const auto [ver, exact_sz] : {
             std::pair{1u, static_cast<uint32_t>(sizeof(PulseFileOpsV1))},
             std::pair{2u, static_cast<uint32_t>(sizeof(PulseFileOpsV2))},
             std::pair{3u, static_cast<uint32_t>(sizeof(PulseFileOpsV3))},
             std::pair{4u, static_cast<uint32_t>(sizeof(PulseFileOpsV4))},
         }) {
        if (HeadlessHostContext::QueryInterface(
                &host_ctx, pulse::sdk::View(PULSE_FILEOPS_INTERFACE_NAME), ver, neg_buf.data(), exact_sz - 1u) !=
            PULSE_ERROR_ABI) {
            undersized_rejected = false;
        }
        if (adapter.Query(ver, neg_buf.data(), exact_sz - 1u) == PULSE_OK) {
            undersized_rejected = false;
        }
    }
    Check(s_ver0 == PULSE_ERROR_ABI && s_ver5 == PULSE_ERROR_ABI &&
              s_direct_ver0 == PULSE_ERROR_ABI && s_direct_ver5 == PULSE_ERROR_ABI &&
              undersized_rejected && canaries_intact(neg_buf, 0),
          "D2.5: Version 0, version 5, and undersized out_size fail with PULSE_ERROR_ABI without touching 0xA5 canaries");

    adapter.Close();
}

// ============================================================================
// Dimension 3: Graceful degradation on unavailable/future/experimental surfaces
// ============================================================================
void TestDimension3GracefulDegradationFallback(const fs::path& fixture_dll) {
    using namespace pulse::extensions::native;

    const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x301u));
    bool live = true;
    SelectionProvider selection;

    CommandsAdapter commands(owner, "compat.pkg", selection.Table(), [&] { return live; });
    TasksBackend task_backend;
    task_backend.create = [](const TaskSnapshot&) { return true; };
    task_backend.update = [](const TaskSnapshot&) {};
    task_backend.finish = [](const TaskSnapshot&) {};
    task_backend.cancelled = [](uint64_t) { return false; };
    task_backend.release = [](uint64_t) {};
    TasksAdapter tasks(owner, [&] { return live; }, std::move(task_backend), {}, "compat.pkg");

    CompatSharedState shared{};
    shared.mode = COMPAT_MODE_DEGRADATION_FALLBACK;
    HeadlessHostContext host_ctx{&shared, nullptr, &commands, &tasks};
    const PulseHostV1 host{sizeof(PulseHostV1), PULSE_ABI_VERSION_1, &host_ctx, &HeadlessHostContext::QueryInterface};

    Module mod;
    const PulseStatus load_status = mod.Load(fixture_dll.wstring(), host);
    Check(load_status == PULSE_OK &&
              shared.ui_mount_status == PULSE_ERROR_UNAVAILABLE && shared.ui_mount_nullopt == 1u &&
              shared.future_commands_status == PULSE_ERROR_ABI && shared.future_commands_nullopt == 1u &&
              shared.unknown_quantum_status == PULSE_ERROR_UNAVAILABLE && shared.unknown_quantum_nullopt == 1u,
          "D3.1: Unavailable pulse.ui.mount, future pulse.commands v99, and unknown quantum return expected errors/nullopt");

    Check(shared.commands_v1_status == PULSE_OK && shared.tasks_v1_status == PULSE_OK &&
              shared.register_cmd_status == PULSE_OK && shared.registered_command_id != 0 &&
              mod.Activate() == PULSE_OK,
          "D3.2: Plugin degrades gracefully to pulse.commands V1 + pulse.tasks V1 and registers fallback command");

    const auto entries = CommandsAdapter::Entries(owner);
    const bool has_fallback = std::any_of(entries.begin(), entries.end(), [](const CommandInfo& info) {
        return info.identity == "native:compat.pkg:compat.fallback.run" && info.enabled;
    });
    const bool dispatched = CommandsAdapter::InvokeIdentity(owner, "native:compat.pkg:compat.fallback.run");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        tasks.Poll();
        if (shared.fallback_task_runs.load(std::memory_order_acquire) == 1 && !tasks.CallbackActive()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    tasks.Poll();

    const bool collected_ok = CommandsAdapter::InvokeIdentity(owner, "native:compat.pkg:compat.fallback.collect");
    Check(has_fallback && dispatched && collected_ok &&
              shared.fallback_cmd_invocations == 1 && shared.fallback_task_id != 0 &&
              shared.fallback_task_runs.load(std::memory_order_acquire) == 1 &&
              shared.collected_tasks == 1u && tasks.RetainedCount() == 0,
          "D3.3: Fallback command executes end-to-end with TaskGroup completion and clean release");

    Check(mod.Close() == PULSE_OK && shared.destroy_calls == 1,
          "D3.4: Degraded fallback plugin unloads and destroys cleanly");
    tasks.Close();
    commands.Close();
}

// ============================================================================
// Dimension 4: Cancellation & terminal-state precedence (TasksAdapter)
// ============================================================================
void TestDimension4CancellationAndTerminalPrecedence() {
    using namespace pulse::extensions::native;

    const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x401u));
    bool live = true;
    std::mutex snap_mutex;
    std::unordered_map<uint64_t, TaskSnapshot> finished_snapshots;

    TasksBackend backend;
    backend.create = [](const TaskSnapshot&) { return true; };
    backend.update = [](const TaskSnapshot&) {};
    backend.finish = [&](const TaskSnapshot& snap) {
        std::lock_guard lock(snap_mutex);
        finished_snapshots[snap.id] = snap;
    };
    backend.cancelled = [](uint64_t) { return false; };
    backend.release = [](uint64_t) {};

    TasksAdapter adapter(owner, [&] { return live; }, std::move(backend), {}, "compat.tasks");
    PulseTasksV1 api{};
    Check(adapter.Query(PULSE_TASKS_VERSION_1, &api, sizeof(api)) == PULSE_OK,
          "D4.0: TasksAdapter provides PulseTasksV1 table");

    // 4A: Cooperative cancellation, transition 0 -> 1, single on_cancel, idempotent duplicate Cancel
    {
        struct CancelTaskState {
            EventHandle progress_ready;
            EventHandle allow_poll_cancel;
            EventHandle worker_exited;
            std::atomic<uint32_t> initial_cancelled{99u};
            std::atomic<uint32_t> observed_cancelled{0u};
            std::atomic<uint32_t> cancel_callback_count{0u};
        } state;

        PulseTaskDescriptor desc{};
        desc.size = sizeof(desc);
        desc.title = pulse::sdk::View("Cancellable Worker");
        desc.type = pulse::sdk::View("compat.cancel");
        desc.user = &state;
        desc.work = [](void* raw, const PulseTaskContext* ctx) noexcept -> PulseTaskState {
            auto* s = static_cast<CancelTaskState*>(raw);
            s->initial_cancelled.store(ctx->cancelled(ctx->context), std::memory_order_release);

            PulseTaskProgress prog{};
            prog.size = sizeof(prog);
            prog.percent = 35;
            prog.completed_items = 7;
            prog.total_items = 20;
            prog.current_item = pulse::sdk::View("chunk-07.bin");
            ctx->report(ctx->context, &prog);
            s->progress_ready.Signal();

            s->allow_poll_cancel.Wait(5000);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                if (ctx->cancelled(ctx->context) != 0u) {
                    s->observed_cancelled.store(1u, std::memory_order_release);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            s->worker_exited.Signal();
            return s->observed_cancelled.load(std::memory_order_acquire) ? PULSE_TASK_CANCELLED : PULSE_TASK_FAILED;
        };
        desc.on_cancel = [](void* raw, PulseTaskId) noexcept {
            static_cast<CancelTaskState*>(raw)->cancel_callback_count.fetch_add(1u, std::memory_order_acq_rel);
        };

        PulseTaskId task_id = 0;
        const PulseStatus create_status = api.create(api.context, &desc, &task_id);
        const bool reported = state.progress_ready.Wait(5000);
        adapter.Poll();

        PulseTaskState running_state = PULSE_TASK_FAILED;
        uint32_t running_percent = 0;
        std::string running_item;
        struct RunningProbe {
            PulseTaskState* st;
            uint32_t* pct;
            std::string* item;
        } probe{&running_state, &running_percent, &running_item};
        api.get(
            api.context,
            task_id,
            [](void* raw, const PulseTaskSnapshot* snap) {
                auto* p = static_cast<RunningProbe*>(raw);
                *p->st = snap->state;
                *p->pct = snap->progress.percent;
                p->item->assign(snap->progress.current_item.data, snap->progress.current_item.length);
            },
            &probe);

        Check(create_status == PULSE_OK && reported && running_state == PULSE_TASK_RUNNING &&
                  running_percent == 35u && running_item == "chunk-07.bin" &&
                  state.initial_cancelled.load(std::memory_order_acquire) == 0u,
              "D4.1: Background task publishes Running progress with initial is_cancelled == 0");

        const PulseStatus first_cancel = api.cancel(api.context, task_id);
        // Duplicate Cancel while Cancelling (before worker exits)
        const PulseStatus dup_cancel_cancelling = api.cancel(api.context, task_id);
        adapter.Poll();
        state.allow_poll_cancel.Signal();
        const bool exited = state.worker_exited.Wait(5000);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && adapter.ActiveWorkerCount() != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        adapter.Poll();

        // Duplicate Cancel after terminal Cancelled
        const PulseStatus dup_cancel_terminal = api.cancel(api.context, task_id);
        const PulseStatus dup_adapter_cancel = adapter.Cancel(task_id);
        adapter.Poll();

        PulseTaskState final_state = PULSE_TASK_RUNNING;
        api.get(
            api.context,
            task_id,
            [](void* raw, const PulseTaskSnapshot* snap) { *static_cast<PulseTaskState*>(raw) = snap->state; },
            &final_state);

        Check(first_cancel == PULSE_OK && dup_cancel_cancelling == PULSE_OK &&
                  dup_cancel_terminal == PULSE_ERROR_STATE && dup_adapter_cancel == PULSE_ERROR_STATE &&
                  exited && state.observed_cancelled.load(std::memory_order_acquire) == 1u &&
                  state.cancel_callback_count.load(std::memory_order_acquire) == 1u &&
                  final_state == PULSE_TASK_CANCELLED,
              "D4.2: Cancel transitions is_cancelled 0->1, fires on_cancel once, reports Cancelled, and duplicate Cancel is idempotent");

        Check(api.release(api.context, task_id) == PULSE_OK,
              "D4.3: Cancelled terminal task releases cleanly");
    }

    // 4B: Task completes (PULSE_TASK_COMPLETED) BEFORE host calls Cancel -> Completed preserved, on_cancel never called
    {
        struct FastCompleteState {
            EventHandle done_event;
            std::atomic<uint32_t> cancel_callback_count{0u};
        } state;

        PulseTaskDescriptor desc{};
        desc.size = sizeof(desc);
        desc.title = pulse::sdk::View("Fast Complete Worker");
        desc.type = pulse::sdk::View("compat.complete");
        desc.user = &state;
        desc.work = [](void* raw, const PulseTaskContext* ctx) noexcept -> PulseTaskState {
            PulseTaskProgress prog{};
            prog.size = sizeof(prog);
            prog.percent = 100;
            prog.completed_items = 3;
            prog.total_items = 3;
            prog.current_item = pulse::sdk::View("done.bin");
            ctx->report(ctx->context, &prog);
            static_cast<FastCompleteState*>(raw)->done_event.Signal();
            return PULSE_TASK_COMPLETED;
        };
        desc.on_cancel = [](void* raw, PulseTaskId) noexcept {
            static_cast<FastCompleteState*>(raw)->cancel_callback_count.fetch_add(1u, std::memory_order_acq_rel);
        };

        PulseTaskId task_id = 0;
        const PulseStatus create_status = api.create(api.context, &desc, &task_id);
        state.done_event.Wait(5000);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline && adapter.ActiveWorkerCount() != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        adapter.Poll();

        const PulseStatus late_cancel_api = api.cancel(api.context, task_id);
        const PulseStatus late_cancel_adapter = adapter.Cancel(task_id);
        adapter.Poll();

        PulseTaskState terminal_state = PULSE_TASK_RUNNING;
        api.get(
            api.context,
            task_id,
            [](void* raw, const PulseTaskSnapshot* snap) { *static_cast<PulseTaskState*>(raw) = snap->state; },
            &terminal_state);

        Check(create_status == PULSE_OK &&
                  late_cancel_api == PULSE_ERROR_STATE && late_cancel_adapter == PULSE_ERROR_STATE &&
                  terminal_state == PULSE_TASK_COMPLETED &&
                  state.cancel_callback_count.load(std::memory_order_acquire) == 0u &&
                  api.release(api.context, task_id) == PULSE_OK,
              "D4.4: Late Cancel after PULSE_TASK_COMPLETED does not overwrite Completed state or invoke on_cancel");
    }

    adapter.Close();
}

// ============================================================================
// Dimension 5: Borrowed-buffer synchronous copy & TaskGroup >16 retention
// ============================================================================
void TestDimension5BorrowedBuffersAndTaskGroupRetention() {
    using namespace pulse::extensions::native;

    const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x501u));
    bool live = true;
    SelectionProvider selection;

    // 5A: Mutable stack char buffers immediately wiped with 0xDD after ABI calls return
    {
        CommandsAdapter commands(owner, "compat.borrow", selection.Table(), [&] { return live; });
        PulseCommandsV1 cmd_api{};
        commands.Query(PULSE_COMMANDS_VERSION_1, &cmd_api, sizeof(cmd_api));

        char id_buf[32] = "borrowed.sync.cmd";
        char title_buf[64] = "Borrowed Title UTF-8 验证";
        char detail_buf[64] = "Extract {stem} 详情";
        char group_buf[32] = "Archive 归档组";
        char ext_buf[16] = ".7z";

        const PulseStringView ext_view{ext_buf, static_cast<uint32_t>(std::strlen(ext_buf))};
        PulseCommandDescriptor cmd_desc{};
        cmd_desc.size = sizeof(cmd_desc);
        cmd_desc.id = {id_buf, static_cast<uint32_t>(std::strlen(id_buf))};
        cmd_desc.title = {title_buf, static_cast<uint32_t>(std::strlen(title_buf))};
        cmd_desc.icon = PULSE_COMMAND_ICON_ARCHIVE;
        cmd_desc.kinds = PULSE_COMMAND_FILES;
        cmd_desc.min_selection = 1;
        cmd_desc.max_selection = 1;
        cmd_desc.extensions = &ext_view;
        cmd_desc.extension_count = 1;
        cmd_desc.invoke = [](void*, PulseCommandId, const PulseContextSelectionSnapshot*) noexcept {};
        cmd_desc.detail = {detail_buf, static_cast<uint32_t>(std::strlen(detail_buf))};
        cmd_desc.group = {group_buf, static_cast<uint32_t>(std::strlen(group_buf))};
        cmd_desc.flags = PULSE_COMMAND_GROUP_SUBMENU;

        PulseCommandId cmd_id = 0;
        const PulseStatus reg_status = cmd_api.register_command(cmd_api.context, &cmd_desc, &cmd_id);
        std::memset(id_buf, 0xDD, sizeof(id_buf));
        std::memset(title_buf, 0xDD, sizeof(title_buf));
        std::memset(detail_buf, 0xDD, sizeof(detail_buf));
        std::memset(group_buf, 0xDD, sizeof(group_buf));
        std::memset(ext_buf, 0xDD, sizeof(ext_buf));

        const auto cmd_entries = CommandsAdapter::Entries(owner);
        const bool cmd_copied_cleanly =
            reg_status == PULSE_OK && cmd_entries.size() == 1 && cmd_entries[0].enabled &&
            cmd_entries[0].identity == "native:compat.borrow:borrowed.sync.cmd" &&
            cmd_entries[0].title == "Borrowed Title UTF-8 验证" &&
            cmd_entries[0].group == "Archive 归档组" &&
            cmd_entries[0].detail == "Extract archive 详情" &&
            cmd_entries[0].title.find('\xDD') == std::string::npos &&
            cmd_entries[0].detail.find('\xDD') == std::string::npos;
        Check(cmd_copied_cleanly,
              "D5.1: CommandsAdapter synchronously copies borrowed stack buffers before 0xDD stack wipe");
        commands.Close();
    }

    // 5A (TasksAdapter create + report borrowed stack buffers) & 5B (TaskGroup 24-task burst >16 retention)
    {
        std::mutex backend_mutex;
        std::unordered_map<uint64_t, TaskSnapshot> created_snapshots;
        std::unordered_map<uint64_t, TaskSnapshot> finished_snapshots;
        size_t released_count = 0;

        TasksBackend backend;
        backend.create = [&](const TaskSnapshot& snap) {
            std::lock_guard lock(backend_mutex);
            created_snapshots[snap.id] = snap;
            return true;
        };
        backend.update = [](const TaskSnapshot&) {};
        backend.finish = [&](const TaskSnapshot& snap) {
            std::lock_guard lock(backend_mutex);
            finished_snapshots[snap.id] = snap;
        };
        backend.cancelled = [](uint64_t) { return false; };
        backend.release = [&](uint64_t) {
            std::lock_guard lock(backend_mutex);
            ++released_count;
        };

        TasksAdapter tasks(owner, [&] { return live; }, std::move(backend), {}, "compat.borrow");
        PulseTasksV1 tasks_api{};
        tasks.Query(PULSE_TASKS_VERSION_1, &tasks_api, sizeof(tasks_api));

        struct BorrowedTaskSync {
            EventHandle start_gate;
            EventHandle report_done;
            PulseStatus report_status = PULSE_ERROR_STATE;
        } sync;

        char task_title[64] = "Borrowed Task 标题";
        char task_type[32] = "compat.borrowed";
        char task_src[64] = "C:\\source\\input.7z";
        char task_dst[64] = "C:\\dest\\output";
        char task_step[32] = "Step 1 校验";
        PulseStringView step_view{task_step, static_cast<uint32_t>(std::strlen(task_step))};

        PulseTaskDescriptor tdesc{};
        tdesc.size = sizeof(tdesc);
        tdesc.title = {task_title, static_cast<uint32_t>(std::strlen(task_title))};
        tdesc.type = {task_type, static_cast<uint32_t>(std::strlen(task_type))};
        tdesc.source = {task_src, static_cast<uint32_t>(std::strlen(task_src))};
        tdesc.destination = {task_dst, static_cast<uint32_t>(std::strlen(task_dst))};
        tdesc.steps = &step_view;
        tdesc.step_count = 1;
        tdesc.user = &sync;
        tdesc.work = [](void* raw, const PulseTaskContext* ctx) noexcept -> PulseTaskState {
            auto* s = static_cast<BorrowedTaskSync*>(raw);
            s->start_gate.Wait(5000);

            char item_buf[64] = "nested/payload_条目.bin";
            char detail_buf[64] = "Progress detail 明细信息";
            char note_buf[64] = "Progress note 底部说明";
            char err_buf[64] = "Diagnostic message 诊断消息";

            PulseTaskProgress prog{};
            prog.size = sizeof(prog);
            prog.percent = 80;
            prog.completed_items = 4;
            prog.total_items = 5;
            prog.current_item = {item_buf, static_cast<uint32_t>(std::strlen(item_buf))};
            prog.detail = {detail_buf, static_cast<uint32_t>(std::strlen(detail_buf))};
            prog.note = {note_buf, static_cast<uint32_t>(std::strlen(note_buf))};
            prog.error = {err_buf, static_cast<uint32_t>(std::strlen(err_buf))};

            s->report_status = ctx->report(ctx->context, &prog);
            std::memset(item_buf, 0xDD, sizeof(item_buf));
            std::memset(detail_buf, 0xDD, sizeof(detail_buf));
            std::memset(note_buf, 0xDD, sizeof(note_buf));
            std::memset(err_buf, 0xDD, sizeof(err_buf));
            s->report_done.Signal();
            return PULSE_TASK_COMPLETED;
        };

        PulseTaskId borrowed_id = 0;
        const PulseStatus tcreate = tasks_api.create(tasks_api.context, &tdesc, &borrowed_id);
        std::memset(task_title, 0xDD, sizeof(task_title));
        std::memset(task_type, 0xDD, sizeof(task_type));
        std::memset(task_src, 0xDD, sizeof(task_src));
        std::memset(task_dst, 0xDD, sizeof(task_dst));
        std::memset(task_step, 0xDD, sizeof(task_step));

        sync.start_gate.Signal();
        sync.report_done.Wait(5000);
        const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < wait_deadline && tasks.ActiveWorkerCount() != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        tasks.Poll();

        struct BorrowedSnapshotCheck {
            std::string item;
            std::string detail;
            std::string note;
            std::string error;
            PulseTaskState state = PULSE_TASK_RUNNING;
        } observed;

        tasks_api.get(
            tasks_api.context,
            borrowed_id,
            [](void* raw, const PulseTaskSnapshot* snap) {
                auto* out = static_cast<BorrowedSnapshotCheck*>(raw);
                out->state = snap->state;
                out->item.assign(snap->progress.current_item.data, snap->progress.current_item.length);
                out->detail.assign(snap->progress.detail.data, snap->progress.detail.length);
                out->note.assign(snap->progress.note.data, snap->progress.note.length);
                out->error.assign(snap->progress.error.data, snap->progress.error.length);
            },
            &observed);

        TaskSnapshot created_copy;
        {
            std::lock_guard lock(backend_mutex);
            created_copy = created_snapshots[borrowed_id];
        }
        const bool task_borrowed_clean =
            tcreate == PULSE_OK && sync.report_status == PULSE_OK &&
            observed.state == PULSE_TASK_COMPLETED &&
            created_copy.title == "Borrowed Task 标题" &&
            created_copy.type == "compat.borrowed" &&
            created_copy.source == "C:\\source\\input.7z" &&
            created_copy.destination == "C:\\dest\\output" &&
            created_copy.steps.size() == 1 && created_copy.steps[0] == "Step 1 校验" &&
            observed.item == "nested/payload_条目.bin" &&
            observed.detail == "Progress detail 明细信息" &&
            observed.note == "Progress note 底部说明" &&
            observed.error == "Diagnostic message 诊断消息" &&
            created_copy.title.find('\xDD') == std::string::npos &&
            observed.detail.find('\xDD') == std::string::npos &&
            observed.error.find('\xDD') == std::string::npos;
        Check(task_borrowed_clean && tasks_api.release(tasks_api.context, borrowed_id) == PULSE_OK,
              "D5.2: TasksAdapter synchronously copies descriptor and progress stack buffers before 0xDD stack wipe");

        // 5B: Spawn 24 rapid short-lived tasks via pulse::sdk::TaskGroup BEFORE Collect()
        released_count = 0;
        pulse::sdk::TaskGroup group(tasks_api);
        constexpr size_t kBurstCount = 24;
        std::vector<PulseTaskId> burst_ids(kBurstCount, 0);
        std::vector<std::weak_ptr<uint64_t>> capture_lifetimes;
        capture_lifetimes.reserve(kBurstCount);
        std::atomic<size_t> executed_workers{0};

        bool all_started = true;
        for (size_t i = 0; i < kBurstCount; ++i) {
            auto token = std::make_shared<uint64_t>(0xB000u + i);
            capture_lifetimes.push_back(token);
            const std::string title = "Burst Task #" + std::to_string(i + 1);
            const PulseStatus st = group.Start(
                title,
                "compat.burst",
                [token, &executed_workers](const PulseTaskContext& ctx) -> PulseTaskState {
                    (void)*token;
                    PulseTaskProgress p{};
                    p.size = sizeof(p);
                    p.percent = 100;
                    p.completed_items = 1;
                    p.total_items = 1;
                    ctx.report(ctx.context, &p);
                    executed_workers.fetch_add(1, std::memory_order_acq_rel);
                    return PULSE_TASK_COMPLETED;
                },
                {},
                &burst_ids[i]);
            if (st != PULSE_OK || burst_ids[i] == 0) {
                all_started = false;
            }
        }

        Check(all_started && group.Pending() == kBurstCount,
              "D5.3: pulse::sdk::TaskGroup starts 24 burst tasks before Collect() with Pending() == 24");

        const auto burst_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < burst_deadline) {
            tasks.Poll();
            if (executed_workers.load(std::memory_order_acquire) == kBurstCount &&
                tasks.ActiveWorkerCount() == 0 && tasks.QueuedCount() == 0 && !tasks.CallbackActive()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        tasks.Poll();

        bool all_24_retained = (tasks.RetainedCount() == kBurstCount);
        for (const PulseTaskId id : burst_ids) {
            PulseTaskState st = PULSE_TASK_RUNNING;
            if (tasks_api.get(
                    tasks_api.context,
                    id,
                    [](void* raw, const PulseTaskSnapshot* snap) {
                        *static_cast<PulseTaskState*>(raw) = snap->state;
                    },
                    &st) != PULSE_OK ||
                st != PULSE_TASK_COMPLETED) {
                all_24_retained = false;
            }
        }
        Check(all_24_retained && group.Pending() == kBurstCount,
              "D5.4: TasksAdapter retains all 24 terminal owned records (>16 legacy cap) prior to TaskGroup::Collect()");

        const size_t collected = group.Collect();
        const bool captures_freed = std::all_of(
            capture_lifetimes.begin(), capture_lifetimes.end(), [](const std::weak_ptr<uint64_t>& w) {
                return w.expired();
            });
        Check(collected == kBurstCount && group.Pending() == 0 &&
                  tasks.RetainedCount() == 0 && tasks.ActiveWorkerCount() == 0 &&
                  released_count == kBurstCount && captures_freed,
              "D5.5: TaskGroup::Collect() releases all 24 records, drops Pending() 24->0, joins workers, and frees captures");

        tasks.Close();
    }
}

fs::path ResolveFixtureDllPath(int argc, wchar_t** argv) {
    if (argc >= 2 && argv[1] && argv[1][0] != L'\0') {
        return fs::absolute(fs::path(argv[1]));
    }
    wchar_t exe_buf[32768]{};
    const DWORD len = GetModuleFileNameW(nullptr, exe_buf, static_cast<DWORD>(std::size(exe_buf)));
    const fs::path exe_dir = len > 0 ? fs::path(std::wstring(exe_buf, len)).parent_path() : fs::current_path();
    for (const wchar_t* candidate : {L"pulse_extension_abi_compat_fixture.dll", L"compat_fixture.dll"}) {
        const fs::path p = exe_dir / candidate;
        if (fs::exists(p)) {
            return fs::absolute(p);
        }
    }
    return exe_dir / L"compat_fixture.dll";
}

} // namespace

// Headless Low-Integrity thunks so commands_adapter.cpp never pulls in USER32.dll.
namespace headless_user32_thunks {
BOOL WINAPI StubIsWindow(HWND) noexcept {
    return FALSE;
}
BOOL WINAPI StubInvalidateRect(HWND, const RECT*, BOOL) noexcept {
    return FALSE;
}
} // namespace headless_user32_thunks

extern "C" void* __imp_IsWindow = reinterpret_cast<void*>(&headless_user32_thunks::StubIsWindow);
extern "C" void* __imp_InvalidateRect = reinterpret_cast<void*>(&headless_user32_thunks::StubInvalidateRect);

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    const fs::path fixture_dll = ResolveFixtureDllPath(argc, argv);
    Check(fs::is_regular_file(fixture_dll), "D0.0: Compatibility fixture DLL exists on disk");
    if (!fs::is_regular_file(fixture_dll)) {
        std::fprintf(stderr, "Missing fixture DLL at: %ls\n", fixture_dll.c_str());
        return 2;
    }

    {
        ScratchDirectory scratch;
        TestDimension1HostAndPluginNegotiation(fixture_dll, scratch.root);
    }
    TestDimension2FileOpsCrossVersionCanaries();
    TestDimension3GracefulDegradationFallback(fixture_dll);
    TestDimension4CancellationAndTerminalPrecedence();
    TestDimension5BorrowedBuffersAndTaskGroupRetention();

    std::printf("Summary: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
