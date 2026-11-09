#include "../../app/extensions/native/tasks_adapter.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
#include <stdexcept>
using namespace pulse::extensions::native;
namespace {
int failures = 0, passes = 0;
void Check(bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); ok ? ++passes : ++failures; }
PulseStringView Text(const char* text) { return {text, static_cast<uint32_t>(std::strlen(text))}; }
struct Work {
    std::atomic<bool> hold{false}, saw_cancel{false};
    std::atomic<int> entered{0}, reports{0};
    int cancels = 0;
    bool cleanup_failure = false;
    uint32_t terminal = PULSE_TASK_COMPLETED;
    static uint32_t PULSE_CALL Run(void* user, const PulseTaskContext* context) {
        auto& work = *static_cast<Work*>(user); ++work.entered;
        char item[] = "item.txt"; char error[] = "copied error";
        PulseTaskProgress progress{sizeof(progress), 40, 1, 2, 10, 20, Text(error), Text(item)};
        if (context->report(context->context, &progress) == PULSE_OK) ++work.reports;
        item[0] = 'X'; error[0] = 'X';
        progress.percent = 101;
        if (context->report(context->context, &progress) == PULSE_ERROR_ARGUMENT) ++work.reports;
        while (work.hold) { if (context->cancelled(context->context)) work.saw_cancel = true; Sleep(1); }
        if (work.saw_cancel && work.cleanup_failure) {
            progress.percent = 40; progress.error = Text("cleanup failed"); progress.current_item = Text("staging");
            if (context->report(context->context, &progress) == PULSE_OK) ++work.reports;
            return PULSE_TASK_FAILED;
        }
        return work.saw_cancel ? PULSE_TASK_CANCELLED : work.terminal;
    }
    static void PULSE_CALL Cancel(void* user, uint64_t) { ++static_cast<Work*>(user)->cancels; }
};
struct Read {
    uint32_t state = 99;
    std::string error, item;
    static void PULSE_CALL Get(void* user, const PulseTaskSnapshot* snapshot) {
        auto& value = *static_cast<Read*>(user); value.state = snapshot->state;
        value.error.assign(snapshot->progress.error.data, snapshot->progress.error.length);
        value.item.assign(snapshot->progress.current_item.data, snapshot->progress.current_item.length);
    }
};
bool Wait(const std::function<bool()>& ready) { const auto until = GetTickCount64() + 5000; do { if (ready()) return true; Sleep(1); } while (GetTickCount64() < until); return false; }
}
int main() {
    std::map<uint64_t, TaskSnapshot> host;
    bool deny = false, cancel = false, wrong_thread = false;
    const auto ui = GetCurrentThreadId(); int finishes = 0, releases = 0, updates = 0;
    TasksBackend backend;
    backend.create = [&](const TaskSnapshot& value) { wrong_thread |= ui != GetCurrentThreadId(); if (deny) return false; host[value.id] = value; return true; };
    backend.update = [&](const TaskSnapshot& value) { wrong_thread |= ui != GetCurrentThreadId(); host[value.id] = value; ++updates; };
    backend.finish = [&](const TaskSnapshot& value) { wrong_thread |= ui != GetCurrentThreadId(); host[value.id] = value; ++finishes; };
    backend.cancelled = [&](uint64_t) { wrong_thread |= ui != GetCurrentThreadId(); return cancel; };
    backend.release = [&](uint64_t id) { wrong_thread |= ui != GetCurrentThreadId(); host.erase(id); ++releases; };
    TasksAdapter adapter(reinterpret_cast<HWND>(1), [] { return true; }, backend); PulseTasksV1 api{};
    Check(adapter.Query(1, &api, sizeof(api)) == PULSE_OK, "query task interface");
    Work work; PulseTaskDescriptor descriptor{sizeof(descriptor), Text("Task"), Text("Archive"), Work::Run, &work, Work::Cancel};
    uint64_t id = 0;
    deny = true; Check(api.create(api.context, &descriptor, &id) == PULSE_ERROR_UNAVAILABLE && !id && !work.entered, "backend rejection starts no worker"); deny = false;
    Check(api.create(api.context, &descriptor, &id) == PULSE_OK && id, "create admits host-owned worker");
    Read read;
    Check(Wait([&] { adapter.Poll(); api.get(api.context, id, Read::Get, &read); return read.state == PULSE_TASK_COMPLETED; }), "completion finalizes on UI poll");
    Check(read.item == "item.txt" && read.error == "copied error" && work.reports == 2, "progress copies borrowed text and validates percent");
    Check(api.cancel(api.context, id) == PULSE_ERROR_STATE && finishes == 1, "terminal immutable and finish once");
    const int terminal_updates = updates;
    for (int n = 0; n < 10; ++n) adapter.Poll();
    Check(updates == terminal_updates && finishes == 1, "retained terminal polls do not copy updates or repeat finish");
    Check(api.release(api.context, id) == PULSE_OK && api.get(api.context, id, Read::Get, &read) == PULSE_ERROR_STATE, "release retires terminal id");
    work.hold = true; work.saw_cancel = false; api.create(api.context, &descriptor, &id);
    Check(Wait([&] { return work.entered == 2; }), "cancellation worker entered");
    Check(Wait([&] { return work.reports == 4; }), "running worker reported progress");
    adapter.Poll(); const int unchanged_updates = updates;
    for (int n = 0; n < 10; ++n) adapter.Poll();
    Check(updates == unchanged_updates, "unchanged running progress does not repaint backend");
    cancel = true; adapter.Poll(); adapter.Poll();
    Check(Wait([&] { return work.saw_cancel.load(); }) && work.cancels == 1 && adapter.CallbackActive(), "host cancel signals once and holds unload gate");
    api.get(api.context, id, Read::Get, &read); Check(read.state == PULSE_TASK_CANCELLING, "cancelling visible before worker return");
    work.hold = false; cancel = false;
    Check(Wait([&] { adapter.Poll(); api.get(api.context, id, Read::Get, &read); return read.state == PULSE_TASK_CANCELLED; }), "cooperative cancellation reaches explicit terminal");
    uint64_t foreign = id + 10000; Check(api.cancel(api.context, foreign) == PULSE_ERROR_STATE, "unknown id cannot cancel another operation");
    PulseStatus wrong = PULSE_OK; std::thread thread([&] { wrong = api.cancel(api.context, id); }); thread.join();
    Check(wrong == PULSE_ERROR_STATE && !wrong_thread, "API and backend UI thread confinement");
    Work cleanup; cleanup.hold = true; cleanup.cleanup_failure = true; descriptor.user = &cleanup;
    uint64_t cleanup_id = 0; api.create(api.context, &descriptor, &cleanup_id);
    Check(Wait([&] { return cleanup.entered == 1; }), "cleanup worker starts");
    api.cancel(api.context, cleanup_id); adapter.Poll();
    Check(Wait([&] { return cleanup.saw_cancel.load(); }), "cleanup observes cancellation");
    cleanup.hold = false;
    Check(Wait([&] { adapter.Poll(); api.get(api.context, cleanup_id, Read::Get, &read); return read.state == PULSE_TASK_FAILED; }) &&
        read.error == "cleanup failed" && cleanup.reports == 3, "cancelled worker can report cleanup error and preserve Failed terminal");
    api.release(api.context, cleanup_id);
    Work failed; failed.terminal = PULSE_TASK_FAILED; descriptor.user = &failed;
    uint64_t failed_id = 0; api.create(api.context, &descriptor, &failed_id);
    Check(Wait([&] { adapter.Poll(); api.get(api.context, failed_id, Read::Get, &read); return read.state == PULSE_TASK_FAILED; }),
        "worker cleanup failure remains explicit Failed");
    api.release(api.context, failed_id);
    Work partial; partial.terminal = PULSE_TASK_PARTIAL; descriptor.user = &partial;
    uint64_t partial_id = 0; api.create(api.context, &descriptor, &partial_id);
    Check(Wait([&] { adapter.Poll(); api.get(api.context, partial_id, Read::Get, &read); return read.state == PULSE_TASK_PARTIAL; }),
        "worker partial completion remains explicit Partial");
    api.release(api.context, partial_id);
    Work queued; queued.hold = true; descriptor.user = &queued;
    uint64_t tasks[15]{};
    for (auto& task : tasks) Check(api.create(api.context, &descriptor, &task) == PULSE_OK, "bounded task admitted");
    Check(Wait([&] { return queued.entered == 2; }), "at most two workers start");
    uint64_t extra = 0; Check(api.create(api.context, &descriptor, &extra) == PULSE_OK, "oldest terminal auto-evicted at capacity");
    Check(api.create(api.context, &descriptor, &extra) == PULSE_ERROR_UNAVAILABLE, "active task capacity rejects overflow");
    adapter.Close(); Check(queued.cancels == 16 && adapter.CallbackActive(), "close delivers cancellation once and retains workers");
    queued.hold = false; Check(Wait([&] { adapter.Poll(); return !adapter.CallbackActive(); }) && host.empty(), "close drains worker lifetime and releases host mappings");
    Check(releases >= 18 && !wrong_thread, "backend release remains UI-thread only");
    Work exception_work; exception_work.hold = true;
    auto throwing_backend = backend;
    throwing_backend.update = [](const TaskSnapshot&) { throw std::runtime_error("backend update"); };
    throwing_backend.release = [](uint64_t) { throw std::runtime_error("backend release"); };
    TasksAdapter exception_adapter(reinterpret_cast<HWND>(2), [] { return true; }, throwing_backend);
    PulseTasksV1 exception_api{}; exception_adapter.Query(1, &exception_api, sizeof(exception_api));
    descriptor.user = &exception_work; uint64_t exception_id = 0;
    exception_api.create(exception_api.context, &descriptor, &exception_id);
    Check(Wait([&] { return exception_work.reports == 2; }), "exception cleanup worker reports");
    exception_adapter.Poll();
    Check(exception_work.cancels == 1 && exception_adapter.CallbackActive(), "backend exception closes and cancels without escaping noexcept poll");
    exception_work.hold = false;
    Check(Wait([&] { exception_adapter.Poll(); return !exception_adapter.CallbackActive(); }), "throwing backend release still permits worker drain");
    std::printf("%d PASS, %d FAIL\n", passes, failures); return failures ? 1 : 0;
}
