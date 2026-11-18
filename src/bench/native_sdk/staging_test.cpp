#include "../../app/extensions/native/staging_adapter.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <stdexcept>
using namespace pulse::extensions::native;
namespace {
int passes = 0, failures = 0;
void Check(bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); ok ? ++passes : ++failures; }
bool Wait(const std::function<bool()>& ready) { const auto end = GetTickCount64() + 8000; do { if (ready()) return true; Sleep(1); } while (GetTickCount64() < end); return false; }
std::string Utf8(const std::wstring& value) {
    int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr); return out;
}
PulseStringView Text(const std::string& s) { return {s.data(), static_cast<uint32_t>(s.size())}; }
struct Created {
    std::atomic<bool> done{false}; PulseStatus status = PULSE_ERROR_STATE; uint64_t id = 0; std::filesystem::path path; DWORD thread = 0;
    static void PULSE_CALL Callback(void* p, PulseStatus status, uint64_t id, PulseStringView path) {
        auto& value = *static_cast<Created*>(p); value.status = status; value.id = id; value.thread = GetCurrentThreadId();
        value.path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data ? path.data : ""), path.length)); value.done = true;
    }
};
struct Snapshot { uint32_t state = 99; uint64_t task = 0; std::string error;
    static void PULSE_CALL Read(void* p, const PulseStageSnapshot* s) { auto& value = *static_cast<Snapshot*>(p); value.state = s->state; value.task = s->task_id; value.error.assign(s->error.data ? s->error.data : "", s->error.length); }
};
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / (L"pulse-staging-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root / L"Unicode-測試");
    const auto destination = root / L"Unicode-測試"; const auto text = Utf8(destination.wstring());
    const auto ui = GetCurrentThreadId(); bool wrong_thread = false, complete = false, cancelled = false, throws = false;
    int submits = 0, queries = 0; HANDLE event = nullptr;
    StagingBackend backend;
    backend.choose_destination = [&](const std::wstring&) { wrong_thread |= ui != GetCurrentThreadId(); return std::pair<PulseStatus, std::wstring>{PULSE_OK, destination.wstring()}; };
    backend.submit = [&](const StageSubmission& request) {
        wrong_thread |= ui != GetCurrentThreadId(); ++submits; event = request.cancel_event;
        if (throws) throw std::runtime_error("submit failure");
        Check(request.sources.size() == 1, "commit passes top-level sources"); return StageSubmitResult{PULSE_OK, 123};
    };
    backend.query = [&](uint64_t) { ++queries; wrong_thread |= ui != GetCurrentThreadId(); return StageOperation{static_cast<PulseStageState>(cancelled ? PULSE_STAGE_CANCELLED : complete ? PULSE_STAGE_COMPLETED : PULSE_STAGE_RUNNING)}; };
    backend.cancel = [&](uint64_t) { wrong_thread |= ui != GetCurrentThreadId(); cancelled = true; };
    StagingAdapter adapter(reinterpret_cast<HWND>(1), [] { return true; }, backend); PulseStagingV1 api{};
    Check(adapter.Query(1, &api, sizeof(api)) == PULSE_OK, "query staging interface");
    bool chosen = false;
    Check(api.choose_destination(api.context, {}, [](void* p, PulseStatus status, PulseStringView path) { *static_cast<bool*>(p) = status == PULSE_OK && path.length > 0; }, &chosen) == PULSE_OK && chosen, "UI destination picker returns borrowed path");
    Created created; Check(api.create(api.context, Text(text), Created::Callback, &created) == PULSE_OK, "create asynchronous acceptance");
    Check(Wait([&] { return created.done.load(); }) && created.status == PULSE_OK && created.thread != ui, "create callback runs on worker");
    if (created.status != PULSE_OK) { api.get(api.context, created.id, [](void*, const PulseStageSnapshot* value) { std::printf("Create diagnostic: %.*s\n", static_cast<int>(value->error.length), value->error.data); }, nullptr); adapter.CleanupRemaining(); Wait([&] { adapter.Poll(); return !adapter.CallbackActive(); }); return 1; }
    HANDLE reader_guard = CreateFileW(created.path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Check(reader_guard != INVALID_HANDLE_VALUE, "published stage accepts archive reader guard sharing");
    if (reader_guard != INVALID_HANDLE_VALUE) CloseHandle(reader_guard);
    Check(std::filesystem::equivalent(created.path.parent_path(), destination), "staging shares destination volume and parent");
    Check((GetFileAttributesW(created.path.c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0, "staging directory hidden");
    Check(!MoveFileW(created.path.c_str(), (root / L"replacement").c_str()), "open staging identity prevents path replacement");
    Check(api.commit(api.context, created.id) == PULSE_OK, "empty commit accepted for worker validation");
    Snapshot snapshot;
    Check(Wait([&] { adapter.Poll(); api.get(api.context, created.id, Snapshot::Read, &snapshot); return snapshot.state == PULSE_STAGE_FAILED; }) && submits == 0, "empty tree rejected before submit");
    Check(api.release(api.context, created.id) == PULSE_OK, "failed stage can release");
    Check(Wait([&] { return !adapter.CallbackActive(); }) && !std::filesystem::exists(created.path), "release deletes asynchronously");
    Check(api.get(api.context, created.id, Snapshot::Read, &snapshot) == PULSE_ERROR_STATE, "released ID is invalid");
    Created work; api.create(api.context, Text(text), Created::Callback, &work); Check(Wait([&] { return work.done.load(); }) && work.status == PULSE_OK, "create second isolated stage");
    std::filesystem::create_directory(work.path / L"folder"); std::ofstream(work.path / L"folder" / L"file.txt") << "payload";
    Check(api.commit(api.context, work.id) == PULSE_OK && api.commit(api.context, work.id) == PULSE_ERROR_STATE, "double commit rejected");
    Check(Wait([&] { adapter.Poll(); api.get(api.context, work.id, Snapshot::Read, &snapshot); return snapshot.state == PULSE_STAGE_RUNNING; }) && snapshot.task == 123, "UI submit publishes operation ID");
    Check(adapter.CallbackActive(), "active operation holds lifecycle");
    Check(api.release(api.context, work.id) == PULSE_ERROR_STATE, "running operation cannot release");
    Check(api.cancel(api.context, work.id) == PULSE_OK && WaitForSingleObject(event, 0) == WAIT_OBJECT_0, "cancel signals confirmation event immediately");
    adapter.Poll(); api.get(api.context, work.id, Snapshot::Read, &snapshot);
    Check(snapshot.state == PULSE_STAGE_CANCELLED && cancelled, "cancel reaches operation backend");
    const int terminal_queries = queries;
    for (int n = 0; n < 10; ++n) adapter.Poll();
    Check(queries == terminal_queries, "terminal stage polls never query backend again");
    Check(api.release(api.context, work.id) == PULSE_OK && Wait([&] { return !adapter.CallbackActive(); }) && !std::filesystem::exists(work.path), "nested ordinary tree cleanup");
    Created invalid; const std::string relative = "relative/path"; api.create(api.context, Text(relative), Created::Callback, &invalid);
    Check(Wait([&] { return invalid.done.load(); }) && invalid.status != PULSE_OK, "relative destination rejected");
    api.release(api.context, invalid.id); Wait([&] { return !adapter.CallbackActive(); });
    throws = true;
    Created rejected; api.create(api.context, Text(text), Created::Callback, &rejected);
    Check(Wait([&] { return rejected.done.load(); }) && rejected.status == PULSE_OK, "create backend-exception stage");
    std::ofstream(rejected.path / L"file.txt") << "test";
    api.commit(api.context, rejected.id);
    Check(Wait([&] { adapter.Poll(); api.get(api.context, rejected.id, Snapshot::Read, &snapshot); return snapshot.state == PULSE_STAGE_FAILED; }), "backend exception becomes terminal failure");
    Check(api.release(api.context, rejected.id) == PULSE_OK && Wait([&] { return !adapter.CallbackActive(); }), "backend exception does not hold lifecycle forever");
    throws = false;
    auto long_destination = std::filesystem::path(L"\\\\?\\" + root.wstring());
    for (int n = 0; n < 7; ++n) long_destination /= L"long-destination-component-0123456789";
    std::filesystem::create_directories(long_destination);
    Created long_stage; const auto long_text = Utf8(long_destination.wstring());
    api.create(api.context, Text(long_text), Created::Callback, &long_stage);
    Check(Wait([&] { return long_stage.done.load(); }) && long_stage.status == PULSE_OK, "extended-length destination accepted");
    if (long_stage.status == PULSE_OK) api.release(api.context, long_stage.id);
    Wait([&] { return !adapter.CallbackActive(); });
    Created blocked; api.create(api.context, Text(text), Created::Callback, &blocked);
    Check(Wait([&] { return blocked.done.load(); }) && blocked.status == PULSE_OK, "create cleanup-failure fixture");
    const auto blocked_file = blocked.path / L"locked.txt";
    std::ofstream(blocked_file) << "preserve";
    HANDLE locked_file = CreateFileW(blocked_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(locked_file != INVALID_HANDLE_VALUE && api.release(api.context, blocked.id) == PULSE_OK, "cleanup accepts ID while file is held");
    Check(Wait([&] { return !adapter.CallbackActive(); }), "failed cleanup worker drains");
    Check(api.get(api.context, blocked.id, Snapshot::Read, &snapshot) == PULSE_OK && snapshot.state == PULSE_STAGE_FAILED &&
        !snapshot.error.empty() && std::filesystem::exists(blocked_file), "cleanup failure retains queryable diagnostic and files");
    if (locked_file != INVALID_HANDLE_VALUE) CloseHandle(locked_file);
    Check(api.release(api.context, blocked.id) == PULSE_OK && Wait([&] { return !adapter.CallbackActive(); }) &&
        !std::filesystem::exists(blocked.path), "explicit retry cleans after file lock released");
    Created ready; api.create(api.context, Text(text), Created::Callback, &ready); Check(Wait([&] { return ready.done.load(); }) && ready.status == PULSE_OK, "create writer-owned stage");
    std::ofstream(ready.path / L"unfinished.txt") << "writer";
    adapter.Close(); Check(std::filesystem::exists(ready.path / L"unfinished.txt"), "Close preserves writer files");
    Check(Wait([&] { return !adapter.CallbackActive(); }), "unsubmitted stage does not block plugin destroy");
    Check(api.commit(api.context, ready.id) == PULSE_ERROR_STATE && api.create(api.context, Text(text), Created::Callback, &created) == PULSE_ERROR_STATE, "Close rejects create and commit");
    Check(api.get(api.context, ready.id, Snapshot::Read, &snapshot) == PULSE_OK, "Close preserves get through destroy");
    adapter.CleanupRemaining(); Check(Wait([&] { adapter.Poll(); return !adapter.CallbackActive(); }) && !std::filesystem::exists(ready.path), "postdestroy cleanup removes remaining stage");
    Check(!wrong_thread, "all host backend calls run on owner UI");
    std::filesystem::remove_all(std::filesystem::path(L"\\\\?\\" + root.wstring()));
    std::printf("%d passed, %d failed\n", passes, failures); return failures ? 1 : 0;
}
