#include <windows.h>
#include "../app/extensions/native/files_adapter.h"
#include <cstdio>
#include <functional>
#include <thread>
#include <type_traits>
#include <vector>
using namespace pulse;
using namespace pulse::extensions;
using namespace pulse::extensions::native;
static_assert(std::is_standard_layout_v<PulseFilesV1> && std::is_trivially_copyable_v<PulseFilesReply>);
static_assert(sizeof(void *) != 8 || sizeof(PulseFilesV1) == 72);
static_assert(sizeof(void *) != 8 || offsetof(PulseFileEntry, size_bytes) == 32);
namespace {
int failures = 0;
void Check(bool ok, const char *label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
PulseStringView View(const std::string &text) { return {text.data(), static_cast<uint32_t>(text.size())}; }
std::string Copy(PulseStringView text) { return text.length ? std::string(text.data, text.length) : std::string(); }
json::Value Success(json::Value data) {
    auto result = json::Value::Object();
    result.set("ok", true);
    result.set("data", std::move(data));
    return result;
}
json::Value Error(const char *code) {
    auto result = json::Value::Object(), error = json::Value::Object();
    result.set("ok", false);
    error.set("code", code);
    result.set("error", std::move(error));
    return result;
}
bool Pump(const std::function<bool()> &done) {
    const auto end = GetTickCount64() + 5000;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (done())
            return true;
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < end);
    return done();
}
struct Capture {
    unsigned calls = 0;
    PulseStatus status = -1;
    uint32_t operation = 0, count = 0, flags = 0, terminal = 0, completed = 0, accepted = 0;
    uint64_t request = 0, number = 0;
    std::string error, name, token;
    std::function<void(const PulseFilesReply &)> hook;
    static void PULSE_CALL Done(void *context, const PulseFilesReply *reply) {
        auto &self = *static_cast<Capture *>(context);
        ++self.calls;
        self.status = reply->status;
        self.operation = reply->operation;
        self.request = reply->request_id;
        self.error = Copy(reply->error);
        self.accepted = reply->cancel_accepted;
        if (reply->operation == PULSE_FILES_CONTEXT)
            self.name = Copy(reply->directory.directory);
        if (reply->operation == PULSE_FILES_LIST) {
            self.count = reply->page.count;
            self.token = Copy(reply->page.next_cursor);
            if (reply->page.count) {
                self.name = Copy(reply->page.entries[0].name);
                self.number = reply->page.entries[0].size_bytes;
                self.flags = reply->page.entries[0].flags;
            }
        }
        if (reply->operation == PULSE_FILES_PREPARE_RENAME) {
            self.count = reply->plan.count;
            self.token = Copy(reply->plan.plan_id);
            if (reply->plan.count)
                self.name = Copy(reply->plan.rows[0].new_leaf_name);
        }
        if (reply->operation == PULSE_FILES_COMMIT_RENAME || reply->operation == PULSE_FILES_TASK_GET ||
            reply->operation == PULSE_FILES_TASK_CANCEL) {
            self.number = reply->task.task_id;
            self.terminal = reply->task.terminal;
            self.completed = reply->task.completed;
            self.count = reply->task.count;
        }
        if (self.hook)
            self.hook(*reply);
    }
};
struct Backend {
    authorization::Registry registry;
    bool hold = false, malformed = false, denied = false;
    unsigned dispatches = 0, cancels = 0;
    uint64_t commit_task = UINT64_MAX;
    json::Value last;
    std::vector<FilesBackend::Completion> completions;
    std::vector<uint64_t> cancelled_tasks;
    std::vector<authorization::OwnerHandle> observed_owners;
    json::Value Reply(const json::Value &input) {
        if (denied)
            return Error("permission_denied");
        auto data = json::Value::Object();
        const auto op = input.get_string("op");
        if (op == "context.get") {
            data.set("directory", "C:\\fixture");
            data.set("selectionName", "sample");
            data.set("viewMode", json::Value());
            data.set("selectionCount", 2);
            data.set("loading", false);
        } else if (op == "files.list") {
            auto rows = json::Value::Array(), row = json::Value::Object();
            row.set("entry", "entry-a");
            row.set("name", "Unicode \xCE\xB2.txt");
            row.set("sizeBytes", malformed ? "18446744073709551616" : "18446744073709551615");
            row.set("modifiedFileTime", "133000000000000000");
            row.set("attributes", 32);
            row.set("directory", false);
            row.set("reparsePoint", true);
            rows.push(std::move(row));
            data.set("entries", std::move(rows));
            data.set("total", 2);
            data.set("nextCursor", "cursor-next");
        } else if (op == "files.prepareRename") {
            auto rows = json::Value::Array();
            for (const auto &item : input.find("items")->items()) {
                auto row = json::Value::Object();
                row.set("entry", item.get_string("entry"));
                row.set("originalName", "original.txt");
                row.set("newLeafName", item.get_string("newLeafName"));
                row.set("status", "ready");
                rows.push(std::move(row));
            }
            data.set("planId", "opaque-plan");
            data.set("expiresInMs", 60000);
            data.set("rows", std::move(rows));
        } else if (op == "files.commitRename") {
            data.set("taskId", std::to_string(commit_task));
            data.set("status", "submitted");
        } else if (op == "operations.get") {
            data.set("taskId", input.get_string("taskId"));
            data.set("cancelRequested", false);
            data.set("terminal", true);
            data.set("completed", true);
            data.set("transferredBytes", "0");
            data.set("totalBytes", "0");
            auto rows = json::Value::Array(), row = json::Value::Object();
            row.set("source", "old.txt");
            row.set("destination", "new.txt");
            row.set("state", 2);
            row.set("error", 0);
            row.set("cleanupError", 0);
            rows.push(std::move(row));
            data.set("items", std::move(rows));
        } else if (op == "operations.cancel") {
            const auto text = input.get_string("taskId");
            cancelled_tasks.push_back(std::stoull(text));
            data.set("taskId", text);
            data.set("accepted", true);
        }
        return Success(std::move(data));
    }
    FilesBackend Functions() {
        FilesBackend functions;
        functions.live = [&](const CallerBinding &caller) { return registry.ValidateBinding(caller); };
        functions.dispatch = [&](const CallerBinding &caller, const json::Value &input, FilesBackend::Completion done) {
            ++dispatches;
            last = input;
            observed_owners.push_back(caller.Owner());
            if (hold)
                completions.push_back(std::move(done));
            else
                done(Reply(input));
            return FilesBackend::Cancel([&] { ++cancels; });
        };
        return functions;
    }
};
} // namespace
int main() {
    Backend backend;
    authorization::PackageIdentity identity{0, "sample.native", "revision", 1};
    backend.registry.SyncPackages({{identity, true}});
    auto first = backend.registry.RegisterOwner(100), second = backend.registry.RegisterOwner(200);
    if (!first || !second)
        return 2;
    auto caller = backend.registry.CaptureBinding(identity, *first);
    auto other_caller = backend.registry.CaptureBinding(identity, *second);
    if (!caller || !other_caller)
        return 2;
    FilesAdapter adapter(*caller, backend.Functions()), other(*other_caller, backend.Functions());
    PulseFilesV1 files{};
    Check(adapter.Query(1, &files, sizeof(files)) == PULSE_OK && files.size == sizeof(files) && files.version == 1,
          "versioned C table is copied into caller-owned storage");
    Check(adapter.Query(2, &files, sizeof(files)) == PULSE_ERROR_ABI &&
              adapter.Query(1, &files, sizeof(files) - 1) == PULSE_ERROR_ARGUMENT,
          "unsupported version and short table fail closed");
    Capture capture;
    uint64_t request = 0;
    Check(files.current_directory(files.context, Capture::Done, &capture, &request) == PULSE_OK && request &&
              !capture.calls,
          "accepted request does not invoke DLL callback inline");
    Check(Pump([&] { return capture.calls == 1; }) && capture.status == PULSE_OK && capture.name == "C:\\fixture",
          "typed current-directory result is delivered on a later UI message");
    capture = {};
    std::string cursor = "original-cursor";
    Check(files.list(files.context, View(cursor), 2, Capture::Done, &capture, &request) == PULSE_OK,
          "paged list accepts an opaque cursor");
    cursor.assign("changed-after-call");
    Check(Pump([&] { return capture.calls == 1; }) && backend.last.get_string("cursor") == "original-cursor" &&
              capture.number == UINT64_MAX && capture.token == "cursor-next" &&
              capture.flags == PULSE_FILE_REPARSE_POINT,
          "cursor is copied and uint64 metadata is not rounded through JavaScript numbers");
    const auto copied_name = capture.name;
    capture = {};
    std::string entry = "entry-a", name = "custom-\xCE\xB2.txt";
    PulseRenameInput input{View(entry), View(name)};
    Check(files.prepare_rename(files.context, &input, 1, Capture::Done, &capture, &request) == PULSE_OK,
          "DLL supplies its own mapping without a fixed naming rule");
    name.assign("changed");
    entry.assign("changed");
    Check(Pump([&] { return capture.calls == 1; }) && capture.name == "custom-\xCE\xB2.txt" &&
              capture.token == "opaque-plan" && copied_name == "Unicode \xCE\xB2.txt",
          "input and retained callback copies do not borrow destroyed storage");
    capture = {};
    const std::string plan = "opaque-plan";
    Check(files.commit_rename(files.context, View(plan), Capture::Done, &capture, &request) == PULSE_OK &&
              Pump([&] { return capture.calls == 1; }) && capture.number == UINT64_MAX,
          "commit delegates plan identity and returns exact submitted task ID");
    capture = {};
    Check(files.task_get(files.context, UINT64_MAX, Capture::Done, &capture, &request) == PULSE_OK &&
              Pump([&] { return capture.calls == 1; }) && capture.terminal && capture.completed && capture.count == 1,
          "typed task query exposes actual terminal and per-item outcome fields");
    capture = {};
    Check(files.task_cancel(files.context, 77, Capture::Done, &capture, &request) == PULSE_OK &&
              Pump([&] { return capture.calls == 1; }) && capture.accepted && backend.cancelled_tasks.back() == 77,
          "task cancellation delegates only the supplied scoped task ID");
    backend.denied = true;
    capture = {};
    files.current_directory(files.context, Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && capture.status != PULSE_OK &&
              capture.error == "permission_denied",
          "host permission failure remains a typed failure with stable machine code");
    backend.denied = false;
    backend.malformed = true;
    capture = {};
    files.list(files.context, {}, 1, Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && capture.error == "invalid_host_response" && !capture.count,
          "malformed overflowing host metadata is rejected without a partial payload");
    backend.malformed = false;
    request = 99;
    Check(files.list(files.context, {}, 257, Capture::Done, &capture, &request) == PULSE_ERROR_ARGUMENT && !request,
          "list bounds reject before dispatch and clear request ID");
    const char invalid_utf8[] = {'\xC0', '\xAF'};
    PulseRenameInput invalid{{"e", 1}, {invalid_utf8, 2}};
    Check(files.prepare_rename(files.context, &invalid, 1, Capture::Done, &capture, &request) == PULSE_ERROR_ARGUMENT &&
              !request,
          "malformed UTF-8 is rejected at the ABI boundary");
    Check(files.prepare_rename(files.context, nullptr, 65, Capture::Done, &capture, &request) == PULSE_ERROR_ARGUMENT &&
              files.task_get(files.context, 0, Capture::Done, &capture, &request) == PULSE_ERROR_ARGUMENT,
          "rename batch and task-ID bounds fail without callbacks");
    PulseStatus wrong_thread = PULSE_OK;
    std::thread worker([&] {
        uint64_t id = 0;
        wrong_thread = files.current_directory(files.context, Capture::Done, &capture, &id);
    });
    worker.join();
    Check(wrong_thread == PULSE_ERROR_STATE, "off-thread ABI calls cannot enter the UI service");
    backend.hold = true;
    capture = {};
    files.current_directory(files.context, Capture::Done, &capture, &request);
    Check(Pump([&] { return !backend.completions.empty(); }), "pending backend request remains asynchronous");
    const auto cancel_before = backend.cancels;
    Check(files.cancel_request(files.context, request) == PULSE_OK && backend.cancels == cancel_before + 1,
          "cancel_request invokes only its host cancellation closure");
    auto late = std::move(backend.completions.back());
    backend.completions.clear();
    late(backend.Reply(backend.last));
    Pump([&] { return true; });
    Check(!capture.calls, "late completion after cancellation cannot call DLL user data");
    backend.hold = false;
    Capture other_capture;
    const auto other_files = other.Table();
    uint64_t other_request = 0;
    other_files.current_directory(other_files.context, Capture::Done, &other_capture, &other_request);
    Check(files.cancel_request(files.context, other_request) == PULSE_ERROR_STATE &&
              Pump([&] { return other_capture.calls == 1; }) && backend.observed_owners.back() == *second,
          "one adapter cannot cancel another owner request or substitute its principal");
    const auto dispatched = backend.dispatches;
    other_capture = {};
    other_files.current_directory(other_files.context, Capture::Done, &other_capture, &other_request);
    backend.registry.UnregisterOwner(*second);
    Check(Pump([&] { return other_capture.calls == 1; }) && other_capture.error == "invalid_caller" &&
              backend.dispatches == dispatched,
          "revoked owner cannot dispatch a queued file resource request");
    std::vector<uint64_t> bounded;
    for (unsigned i = 0; i < 64; ++i) {
        uint64_t id = 0;
        if (files.current_directory(files.context, Capture::Done, &capture, &id) == PULSE_OK)
            bounded.push_back(id);
    }
    Check(bounded.size() == 64 &&
              files.current_directory(files.context, Capture::Done, &capture, &request) == PULSE_ERROR_STATE,
          "pending request admission is bounded at 64");
    for (auto id : bounded)
        files.cancel_request(files.context, id);
    backend.commit_task = 123;
    capture = {};
    files.commit_rename(files.context, View(plan), Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }), "adapter records its own submitted task for shutdown");
    const auto cancelled_before_close = backend.cancelled_tasks.size();
    capture = {};
    bool active_inside = false;
    PulseStatus close_inside = PULSE_OK;
    capture.hook = [&](const PulseFilesReply &) {
        active_inside = adapter.CallbackActive();
        close_inside = adapter.Close();
    };
    files.current_directory(files.context, Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && active_inside && close_inside == PULSE_ERROR_STATE &&
              !adapter.CallbackActive(),
          "close during callback suppresses future delivery but signals loader to defer DLL unload");
    Check(backend.cancelled_tasks.size() == cancelled_before_close + 1 && backend.cancelled_tasks.back() == 123,
          "close requests cancellation only for this adapter's unobserved submitted task");
    Check(files.current_directory(files.context, Capture::Done, &capture, &request) == PULSE_ERROR_STATE && !request,
          "cached interface cannot issue operations after close");
    other.Close();
    return failures ? 1 : 0;
}
