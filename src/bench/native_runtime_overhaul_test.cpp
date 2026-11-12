// Standalone unit test for the native extension host runtime overhaul
// (PLG-07, PLG-08, PLG-09, PLG-10, PLG-16).
//
// Recommended CMake target (in cmake/PulseNativeSdk.cmake):
//   add_executable(pulse_native_runtime_overhaul_test
//       "${PULSE_NATIVE_ROOT}/src/bench/native_runtime_overhaul_test.cpp"
//       "${PULSE_NATIVE_ROOT}/src/app/extensions/native/tasks_adapter.cpp"
//       "${PULSE_NATIVE_ROOT}/src/app/extensions/native/commands_adapter.cpp"
//       "${PULSE_NATIVE_ROOT}/src/app/extensions/native/crash_guard.cpp")
//   target_include_directories(pulse_native_runtime_overhaul_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
//   target_compile_features(pulse_native_runtime_overhaul_test PRIVATE cxx_std_20)
//   target_compile_definitions(pulse_native_runtime_overhaul_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
//   target_compile_options(pulse_native_runtime_overhaul_test PRIVATE /W4 /permissive- /utf-8)
//   target_link_libraries(pulse_native_runtime_overhaul_test PRIVATE pulse_crash_reporter psapi user32)

#include "../app/extensions/manifest.h"
#include "../app/extensions/runtime.h"
#include "../app/extensions/native/commands_adapter.h"
#include "../app/extensions/native/crash_guard.h"
#include "../app/extensions/native/tasks_adapter.h"
#include "../common/crash_reporter.h"
#include <pulse/client.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace guard = pulse::extensions::native::crash_guard;

namespace {

std::array<pulse::extensions::Manifest, 2> g_manifests;
std::array<bool, 2> g_enabled{true, true};
int g_failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++g_failures;
}

bool EqualsStr(const char* actual, std::string_view expected) {
    return actual != nullptr && std::string_view(actual) == expected;
}

std::string ReadAllBytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

} // namespace

namespace pulse::extensions {
size_t PackageCount() { return g_manifests.size(); }
const Manifest* PackageManifest(size_t index) {
    return index < g_manifests.size() ? &g_manifests[index] : nullptr;
}
void SetPackageEnabled(size_t index, bool enabled) {
    if (index < g_enabled.size()) g_enabled[index] = enabled;
}
bool DevelopmentMode() { return false; }
} // namespace pulse::extensions

namespace {

void TestTasksOwnershipAndCapacity() {
    using namespace pulse::extensions::native;

    std::mutex backend_mutex;
    size_t backend_created = 0;
    size_t backend_finished = 0;
    size_t backend_released = 0;
    std::atomic<uint32_t> wake_notifications{0};
    std::atomic<uint32_t> clue_matches{0};
    bool live = true;

    TasksBackend backend;
    backend.create = [&](const TaskSnapshot&) {
        std::lock_guard lock(backend_mutex);
        ++backend_created;
        return true;
    };
    backend.update = [&](const TaskSnapshot&) {};
    backend.finish = [&](const TaskSnapshot&) {
        std::lock_guard lock(backend_mutex);
        ++backend_finished;
    };
    backend.cancelled = [](uint64_t) { return false; };
    backend.release = [&](uint64_t) {
        std::lock_guard lock(backend_mutex);
        ++backend_released;
    };

    const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x101));
    TasksAdapter adapter(
        owner,
        [&] { return live; },
        std::move(backend),
        [&] { wake_notifications.fetch_add(1, std::memory_order_relaxed); },
        "pulse.7zip");

    PulseTasksV1 api{};
    Check(adapter.Query(PULSE_TASKS_VERSION_1, &api, sizeof(api)) == PULSE_OK,
          "PLG-08: TasksAdapter queries PulseTasksV1 table");

    pulse::sdk::TaskGroup group(api);
    using LargeCapture = std::array<uint8_t, 4096>;
    std::vector<std::weak_ptr<LargeCapture>> burst_weaks;
    burst_weaks.reserve(24);

    bool all_started = true;
    for (size_t i = 0; i < 24; ++i) {
        auto payload = std::make_shared<LargeCapture>();
        payload->fill(static_cast<uint8_t>(i + 1));
        burst_weaks.push_back(payload);
        PulseTaskId id = 0;
        const PulseStatus st = group.Start(
            "Archive task",
            "archive.extract",
            [payload, &clue_matches](const PulseTaskContext& ctx) -> PulseTaskState {
                if (EqualsStr(guard::ActiveCallPackage(), "pulse.7zip"))
                    clue_matches.fetch_add(1, std::memory_order_relaxed);
                PulseTaskProgress progress{};
                progress.size = sizeof(progress);
                progress.percent = 100;
                progress.completed_items = 1;
                progress.total_items = 1;
                ctx.report(ctx.context, &progress);
                return (*payload)[0] != 0 ? PULSE_TASK_COMPLETED : PULSE_TASK_FAILED;
            },
            {},
            &id);
        if (st != PULSE_OK || id == 0) all_started = false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        adapter.Poll();
        if (!adapter.CallbackActive()) {
            adapter.Poll();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    Check(all_started && !adapter.CallbackActive(),
          "PLG-08: 24 fast tasks complete across 2-worker pool");
    Check(wake_notifications.load(std::memory_order_relaxed) > 0,
          "PLG-09: TasksAdapter emits event wakeup notifications on progress/completion");
    Check(clue_matches.load(std::memory_order_relaxed) == 24,
          "PLG-10: TasksAdapter worker threads install CallScope for active package");

    bool any_prematurely_freed = false;
    for (const auto& weak : burst_weaks) {
        if (weak.expired()) any_prematurely_freed = true;
    }
    Check(adapter.RetainedCount() == 24 && group.Pending() == 24 && !any_prematurely_freed && backend_released == 0,
          "PLG-08: un-released terminal tasks above 16 are never auto-evicted");

    const size_t collected_24 = group.Collect();
    bool all_freed = true;
    for (const auto& weak : burst_weaks) {
        if (!weak.expired()) all_freed = false;
    }
    Check(collected_24 == 24 && group.Pending() == 0 && adapter.RetainedCount() == 0 &&
              backend_finished == 24 && backend_released == 24 && all_freed,
          "PLG-08: delayed TaskGroup::Collect releases all 24 completed tasks and frees captures");

    // Fill up to kMaxRetainedTasks (64) without calling Collect().
    std::vector<std::weak_ptr<int>> cap_weaks;
    cap_weaks.reserve(kMaxRetainedTasks);
    bool cap_started = true;
    for (size_t i = 0; i < kMaxRetainedTasks; ++i) {
        auto token = std::make_shared<int>(static_cast<int>(i));
        cap_weaks.push_back(token);
        PulseTaskId id = 0;
        if (group.Start("Cap", "test", [token](const PulseTaskContext&) {
                return *token >= 0 ? PULSE_TASK_COMPLETED : PULSE_TASK_FAILED;
            }, {}, &id) != PULSE_OK) {
            cap_started = false;
        }
    }
    const auto deadline_64 = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline_64) {
        adapter.Poll();
        if (!adapter.CallbackActive()) {
            adapter.Poll();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(cap_started && adapter.RetainedCount() == kMaxRetainedTasks && group.Pending() == kMaxRetainedTasks,
          "PLG-08: 64 completed un-released tasks retained at capacity");

    auto extra = std::make_shared<int>(999);
    std::weak_ptr<int> extra_weak = extra;
    PulseTaskId overflow_id = 77;
    const PulseStatus overflow_status = group.Start(
        "Overflow", "test", [extra](const PulseTaskContext&) { return PULSE_TASK_COMPLETED; }, {}, &overflow_id);
    extra.reset();
    Check(overflow_status == PULSE_ERROR_UNAVAILABLE && overflow_id == 0 && extra_weak.expired() &&
              adapter.RetainedCount() == kMaxRetainedTasks,
          "PLG-08: 65th un-released task returns PULSE_ERROR_UNAVAILABLE without evicting terminal tasks");

    const size_t collected_64 = group.Collect();
    bool all_64_freed = true;
    for (const auto& weak : cap_weaks) {
        if (!weak.expired()) all_64_freed = false;
    }
    Check(collected_64 == kMaxRetainedTasks && group.Pending() == 0 &&
              adapter.RetainedCount() == 0 && all_64_freed,
          "PLG-08: Collect drains full 64-task table and frees all captures");

    PulseTaskId recovered_id = 0;
    Check(group.Start("Recovered", "test", [](const PulseTaskContext&) { return PULSE_TASK_COMPLETED; }, {}, &recovered_id) == PULSE_OK &&
              recovered_id != 0,
          "PLG-08: task creation recovers immediately after Collect releases retained slots");
    while (adapter.CallbackActive()) {
        adapter.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    adapter.Poll();
    Check(group.Collect() == 1 && group.Pending() == 0 && adapter.RetainedCount() == 0,
          "PLG-08: post-recovery task collects cleanly");
}

void TestMultiModuleCrashGuardAndCallScope() {
    g_manifests[0] = {};
    g_manifests[0].id = "pulse.7zip";
    g_manifests[0].name = "7-Zip Archive Support";
    g_manifests[0].native.emplace();
    g_manifests[0].native->library = "pulse_7zip.dll";
    g_manifests[0].native->sha256 = std::string(64, 'a');
    g_manifests[0].native->files.push_back({"7z.dll", std::string(64, 'b')});

    g_manifests[1] = {};
    g_manifests[1].id = "pulse.inspector";
    g_manifests[1].name = "Inspector";
    g_manifests[1].native.emplace();
    g_manifests[1].native->library = "pulse_inspector.dll";
    g_manifests[1].native->sha256 = std::string(64, 'c');

    constexpr uintptr_t kEntryBegin = 0x50000000u;
    constexpr uintptr_t kEntryEnd = 0x50020000u;
    constexpr uintptr_t kSidecarBegin = 0x60000000u;
    constexpr uintptr_t kSidecarEnd = 0x60100000u;
    constexpr uintptr_t kOtherBegin = 0x70000000u;
    constexpr uintptr_t kOtherEnd = 0x70018000u;

    Check(guard::TrackModuleRange(0, kEntryBegin, kEntryEnd, true),
          "PLG-10: track entry DLL range for session 1 of pulse.7zip");
    Check(guard::TrackModuleRange(0, kSidecarBegin, kSidecarEnd, false),
          "PLG-10: attach private sidecar DLL (7z.dll) range to pulse.7zip");
    Check(guard::TrackModuleRange(0, kEntryBegin, kEntryEnd, true),
          "PLG-10: second session increments refcount without duplicating range slot");
    Check(guard::TrackModuleRange(1, kOtherBegin, kOtherEnd, true),
          "PLG-10: track second plugin range concurrently");

    Check(EqualsStr(guard::FindAttributedPackage(reinterpret_cast<const void*>(kEntryBegin)), "pulse.7zip") &&
              EqualsStr(guard::FindAttributedPackage(reinterpret_cast<const void*>(kEntryEnd - 1)), "pulse.7zip") &&
              guard::FindAttributedPackage(reinterpret_cast<const void*>(kEntryBegin - 1)) == nullptr &&
              guard::FindAttributedPackage(reinterpret_cast<const void*>(kEntryEnd)) == nullptr,
          "PLG-10: lock-free entry image boundary attribution is exact");

    Check(EqualsStr(guard::FindAttributedPackage(reinterpret_cast<const void*>(kSidecarBegin)), "pulse.7zip") &&
              EqualsStr(guard::FindAttributedPackage(reinterpret_cast<const void*>(kSidecarBegin + 0x84321u)), "pulse.7zip") &&
              EqualsStr(guard::FindAttributedPackage(reinterpret_cast<const void*>(kSidecarEnd - 1)), "pulse.7zip") &&
              guard::FindAttributedPackage(reinterpret_cast<const void*>(kSidecarEnd)) == nullptr,
          "PLG-10: faults inside sidecar 7z.dll range attribute to pulse.7zip");

    Check(EqualsStr(guard::FindAttributedPackage(reinterpret_cast<const void*>(kOtherBegin + 0x100u)), "pulse.inspector"),
          "PLG-10: concurrent second package range attributes to pulse.inspector");

    guard::ForgetModule(0);
    Check(EqualsStr(guard::FindAttributedPackage(reinterpret_cast<const void*>(kEntryBegin + 0x10u)), "pulse.7zip") &&
              EqualsStr(guard::FindAttributedPackage(reinterpret_cast<const void*>(kSidecarBegin + 0x10u)), "pulse.7zip"),
          "PLG-10: closing one of two sessions preserves both entry and sidecar attribution");

    guard::ForgetModule(0);
    Check(guard::FindAttributedPackage(reinterpret_cast<const void*>(kEntryBegin + 0x10u)) == nullptr &&
              guard::FindAttributedPackage(reinterpret_cast<const void*>(kSidecarBegin + 0x10u)) == nullptr &&
              EqualsStr(guard::FindAttributedPackage(reinterpret_cast<const void*>(kOtherBegin + 0x100u)), "pulse.inspector"),
          "PLG-10: refcount reaching zero clears both entry and sidecar ranges without disturbing other packages");

    guard::ForgetModule(1);
    Check(guard::FindAttributedPackage(reinterpret_cast<const void*>(kOtherBegin + 0x100u)) == nullptr,
          "PLG-10: second package range clears when its refcount reaches zero");

    // Thread-local CallScope isolation and automatic nesting restoration.
    Check(guard::ActiveCallPackage() == nullptr,
          "PLG-10: ActiveCallPackage is initially null");
    {
        const guard::CallScope outer("pulse.7zip");
        Check(EqualsStr(guard::ActiveCallPackage(), "pulse.7zip"),
              "PLG-10: outer CallScope sets thread-local active package");

        bool worker_initially_null = false;
        bool worker_scoped_ok = false;
        bool worker_restored_null = false;
        std::thread other([&] {
            worker_initially_null = (guard::ActiveCallPackage() == nullptr);
            {
                const guard::CallScope worker_scope("pulse.inspector");
                worker_scoped_ok = EqualsStr(guard::ActiveCallPackage(), "pulse.inspector");
            }
            worker_restored_null = (guard::ActiveCallPackage() == nullptr);
        });
        other.join();

        Check(worker_initially_null && worker_scoped_ok && worker_restored_null &&
                  EqualsStr(guard::ActiveCallPackage(), "pulse.7zip"),
              "PLG-10: CallScope is strictly thread-local and isolated across threads");

        {
            const guard::CallScope inner("pulse.inspector");
            Check(EqualsStr(guard::ActiveCallPackage(), "pulse.inspector"),
                  "PLG-10: nested CallScope overrides active clue within inner scope");
        }
        Check(EqualsStr(guard::ActiveCallPackage(), "pulse.7zip"),
              "PLG-10: exiting nested CallScope restores outer package clue");
    }
    Check(guard::ActiveCallPackage() == nullptr,
          "PLG-10: exiting outer CallScope clears thread-local clue");

    // Verify crash_reporter writes ActiveExtensionCallClue without disabling package
    // when fault address is outside all tracked plugin module images.
    const fs::path temp_root = fs::absolute(
        fs::path("bench_data") /
        ("native-overhaul-clue-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    std::error_code ec;
    fs::create_directories(temp_root / "extensions", ec);
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", temp_root.wstring().c_str());
    (void)guard::ApplyAtStartup(true);

    if (pulse::crash::Initialize({pulse::crash::ProcessRole::Test, false, temp_root.wstring()})) {
        {
            const guard::CallScope clue_scope("pulse.7zip");
            EXCEPTION_RECORD rec{};
            rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
            rec.ExceptionAddress = reinterpret_cast<PVOID>(static_cast<uintptr_t>(0x00001234u));
            CONTEXT ctx{};
            EXCEPTION_POINTERS ptrs{&rec, &ctx};
            pulse::crash::ReportRecoverable(&ptrs, "unit_test_outside_plugin");
        }
        pulse::crash::Shutdown();

        bool found_clue_json = false;
        bool extension_left_empty = false;
        const fs::path crash_dir = temp_root / "Diagnostics" / "Crashes";
        if (fs::exists(crash_dir, ec)) {
            for (const auto& item : fs::directory_iterator(crash_dir, ec)) {
                if (item.path().extension() != L".json") continue;
                const std::string json_text = ReadAllBytes(item.path());
                if (json_text.find("ActiveExtensionCallClue: pulse.7zip (unconfirmed fault outside plugin image)") !=
                    std::string::npos) {
                    found_clue_json = true;
                }
                if (json_text.find("\"extension\":\"\"") != std::string::npos) {
                    extension_left_empty = true;
                }
            }
        }
        Check(found_clue_json && extension_left_empty &&
                  !fs::exists(temp_root / "extensions" / "native-crashed.txt", ec),
              "PLG-10: fault outside plugin image logs ActiveExtensionCallClue without writing native-crashed.txt");
    } else {
        Check(false, "PLG-10: initialize isolated crash reporter for clue verification");
    }
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr);
    fs::remove_all(temp_root, ec);
}

struct CommandFixture {
    bool live = true;
    int invoked = 0;
    std::string invoked_entry;
    std::string active_clue_during_invoke;
    pulse::extensions::native::CommandSelection current_selection;

    static PulseStatus PULSE_CALL Summary(void* user, PulseContextChangeFn callback, void* context) {
        (void)user;
        PulseContextSnapshot snapshot{};
        snapshot.size = sizeof(snapshot);
        snapshot.selection_revision = 1;
        callback(context, &snapshot);
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Capture(void* user, PulseContextSelectionFn callback, void* context) {
        auto& self = *static_cast<CommandFixture*>(user);
        if (!self.current_selection.valid) return PULSE_ERROR_STATE;
        std::vector<PulseContextSelectionEntry> entries;
        entries.reserve(self.current_selection.entries.size());
        for (const auto& item : self.current_selection.entries) {
            entries.push_back({
                sizeof(PulseContextSelectionEntry),
                item.directory ? static_cast<uint32_t>(PULSE_CONTEXT_ENTRY_DIRECTORY) : 0u,
                pulse::sdk::View(item.path),
                pulse::sdk::View(item.name),
            });
        }
        const PulseContextSelectionSnapshot snapshot{
            sizeof(PulseContextSelectionSnapshot),
            1,
            pulse::sdk::View(self.current_selection.directory),
            entries.data(),
            static_cast<uint32_t>(entries.size()),
        };
        callback(context, &snapshot);
        return PULSE_OK;
    }
    static void PULSE_CALL Invoke(void* user, PulseCommandId, const PulseContextSelectionSnapshot* snapshot) {
        auto& self = *static_cast<CommandFixture*>(user);
        ++self.invoked;
        if (snapshot && snapshot->entry_count > 0 && snapshot->entries[0].name.data) {
            self.invoked_entry.assign(snapshot->entries[0].name.data, snapshot->entries[0].name.length);
        }
        const char* clue = guard::ActiveCallPackage();
        self.active_clue_during_invoke = clue ? clue : "";
    }
};

void TestStaticCommandContributions() {
    using namespace pulse::extensions;
    using namespace pulse::extensions::native;

    ManifestCommandContribution extract;
    extract.id = "extract-here";
    extract.title = {"解压到当前文件夹", "Extract Here", "解壓縮至此"};
    extract.detail = {"释放自 {stem}", "From {stem}", ""};
    extract.group = {"7-Zip", "7-Zip", "7-Zip"};
    extract.submenu = true;
    extract.icon = PULSE_COMMAND_ICON_EXTRACT;
    extract.kinds = PULSE_COMMAND_FILES;
    extract.min_selection = 1;
    extract.max_selection = 1;
    extract.extensions = {".7z", ".zip", ".gz"};
    extract.virtual_key = 'E';
    extract.modifiers = PULSE_COMMAND_CTRL | PULSE_COMMAND_SHIFT;

    ManifestCommandContribution compress;
    compress.id = "quick-compress";
    compress.title = {"压缩", "Compress", ""};
    compress.detail = {"生成 \"{base}.7z\"", "Create \"{base}.7z\"", ""};
    compress.group = {"", "7-Zip", ""};
    compress.submenu = false;
    compress.icon = PULSE_COMMAND_ICON_ARCHIVE;
    compress.kinds = PULSE_COMMAND_FILES | PULSE_COMMAND_DIRECTORIES;
    compress.min_selection = 1;
    compress.max_selection = 4096;
    compress.extensions = {".7z"};
    compress.exclude_extensions = true;

    Check(CommandsAdapter::ResolveLocalizedText(extract.title, StaticCommandLanguage::ZhTW) == "解壓縮至此" &&
              CommandsAdapter::ResolveLocalizedText(extract.detail, StaticCommandLanguage::ZhTW) == "释放自 {stem}" &&
              CommandsAdapter::ResolveLocalizedText(compress.group, StaticCommandLanguage::ZhTW) == "7-Zip",
          "PLG-07: StaticCommandLanguage::ZhTW resolves tw -> zh -> en fallback chain");

    const HWND owner1 = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x201));
    const HWND owner2 = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x202));

    CommandSelection single_archive;
    single_archive.directory = "C:\\work\\Q4";
    single_archive.valid = true;
    single_archive.entries.push_back({"C:\\work\\Q4\\report.final.tar.GZ", "report.final.tar.GZ", false});

    const CommandInfo info_tw = CommandsAdapter::EvaluateStaticContribution(
        owner1, 0, "pulse.7zip", extract, StaticCommandLanguage::ZhTW, single_archive);
    const CommandInfo info_en = CommandsAdapter::EvaluateStaticContribution(
        owner1, 0, "pulse.7zip", extract, StaticCommandLanguage::EnUS, single_archive);

    Check(info_tw.enabled &&
              info_tw.token >= kFirstStaticCommandToken &&
              info_tw.token <= kLastStaticCommandToken &&
              info_tw.token == info_en.token &&
              info_tw.title == "解壓縮至此" &&
              info_en.title == "Extract Here" &&
              info_en.detail == "From report.final.tar" &&
              (info_en.flags & PULSE_COMMAND_GROUP_SUBMENU) != 0,
          "PLG-07: static contribution matches case-insensitive extension, resolves {stem}, and keeps stable token");

    const CommandInfo excluded_on_7z = CommandsAdapter::EvaluateStaticContribution(
        owner1, 0, "pulse.7zip", compress, StaticCommandLanguage::EnUS,
        CommandSelection{"C:\\work\\Q4", {{"C:\\work\\Q4\\bundle.7z", "bundle.7z", false}}, true});
    Check(!excluded_on_7z.enabled,
          "PLG-07: exclude_extensions rejects excluded .7z selection");

    CommandSelection single_folder;
    single_folder.directory = "C:\\work\\Q4";
    single_folder.valid = true;
    single_folder.entries.push_back({"C:\\work\\Q4\\Photos.2026", "Photos.2026", true});
    const CommandInfo folder_compress = CommandsAdapter::EvaluateStaticContribution(
        owner1, 0, "pulse.7zip", compress, StaticCommandLanguage::EnUS, single_folder);
    Check(folder_compress.enabled && folder_compress.detail == "Create \"Photos.2026.7z\"",
          "PLG-07: single directory keeps full name including dots for {base}/{stem}");

    CommandSelection multi_files;
    multi_files.directory = "C:\\work\\BundleDir\\";
    multi_files.valid = true;
    multi_files.entries.push_back({"C:\\work\\BundleDir\\a.txt", "a.txt", false});
    multi_files.entries.push_back({"C:\\work\\BundleDir\\b.txt", "b.txt", false});
    const CommandInfo multi_compress = CommandsAdapter::EvaluateStaticContribution(
        owner1, 0, "pulse.7zip", compress, StaticCommandLanguage::ZhCN, multi_files);

    ManifestCommandContribution stem_on_multi = compress;
    stem_on_multi.id = "stem-multi";
    stem_on_multi.detail = {"{stem}.7z", "{stem}.7z", ""};
    const CommandInfo multi_stem = CommandsAdapter::EvaluateStaticContribution(
        owner1, 0, "pulse.7zip", stem_on_multi, StaticCommandLanguage::EnUS, multi_files);

    Check(multi_compress.enabled && multi_compress.detail == "生成 \"BundleDir.7z\"" &&
              multi_stem.enabled && multi_stem.detail.empty(),
          "PLG-07: multi-selection resolves {base} to parent folder name and suppresses {stem}");

    Check(CommandsAdapter::Identity(owner1, info_en.token) == "native:pulse.7zip:extract-here" &&
              !CommandsAdapter::LookupStaticToken(owner2, info_en.token).has_value(),
          "PLG-07: static command token resolves identity for owning window only");

    // Simulate on-demand DLL activation followed by InvokeIdentity handoff.
    CommandFixture fixture;
    fixture.current_selection = single_archive;
    PulseContextV1 ctx{};
    ctx.size = sizeof(ctx);
    ctx.version = PULSE_CONTEXT_VERSION_1;
    ctx.context = &fixture;
    ctx.get = CommandFixture::Summary;
    ctx.get_selection = CommandFixture::Capture;

    CommandsAdapter live_adapter(owner1, "pulse.7zip", ctx, [&] { return fixture.live; });
    PulseCommandsV1 cmd_table{};
    Check(live_adapter.Query(PULSE_COMMANDS_VERSION_1, &cmd_table, sizeof(cmd_table)) == PULSE_OK,
          "PLG-07: live CommandsAdapter queries table on demand");

    const std::array<PulseStringView, 3> exts{
        pulse::sdk::View(".7z"),
        pulse::sdk::View(".zip"),
        pulse::sdk::View(".gz"),
    };
    PulseCommandDescriptor desc{};
    desc.size = sizeof(desc);
    desc.id = pulse::sdk::View("extract-here");
    desc.title = pulse::sdk::View("Extract Here");
    desc.icon = PULSE_COMMAND_ICON_EXTRACT;
    desc.kinds = PULSE_COMMAND_FILES;
    desc.min_selection = 1;
    desc.max_selection = 1;
    desc.extensions = exts.data();
    desc.extension_count = static_cast<uint32_t>(exts.size());
    desc.invoke = CommandFixture::Invoke;
    desc.user = &fixture;
    PulseCommandId live_id = 0;
    Check(cmd_table.register_command(cmd_table.context, &desc, &live_id) == PULSE_OK &&
              static_cast<int>(live_id) < kFirstStaticCommandToken,
          "PLG-07: live command token stays in dynamic range below static token space");

    const auto target = CommandsAdapter::LookupStaticToken(owner1, info_en.token);
    Check(target.has_value() && target->package == 0 &&
              CommandsAdapter::InvokeIdentity(owner1, target->identity) &&
              fixture.invoked == 1 &&
              fixture.invoked_entry == "report.final.tar.GZ" &&
              fixture.active_clue_during_invoke == "pulse.7zip",
          "PLG-07/PLG-10: static token dispatches live registered callback via InvokeIdentity with CallScope");

    live_adapter.Close();
    CommandsAdapter::ClearStaticOwner(owner1);
    Check(!CommandsAdapter::LookupStaticToken(owner1, info_en.token).has_value(),
          "PLG-07: ClearStaticOwner removes static token mappings on window close");
}

} // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    TestTasksOwnershipAndCapacity();
    TestMultiModuleCrashGuardAndCallScope();
    TestStaticCommandContributions();
    std::printf("Summary: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
