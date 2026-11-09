#include "../app/extensions/native/resources_adapter.h"
#include <cstdio>
#include <functional>
#include <thread>
#include <type_traits>
#include <vector>
#include <windows.h>
using namespace pulse;
using namespace pulse::extensions;
using namespace pulse::extensions::native;
static_assert(std::is_standard_layout_v<PulseResourcesV1> && std::is_trivially_copyable_v<PulseResourcesReply>);
static_assert(sizeof(void *) != 8 || sizeof(PulseResourcesV1) == 56);
static_assert(sizeof(void *) != 8 || offsetof(PulseResourceFileStat, size_bytes) == 16);
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
    uint64_t request = 0, number = 0;
    uint32_t count = 0, flags = 0;
    std::string error, name, value;
    std::function<void()> hook;
    static void PULSE_CALL Done(void *context, const PulseResourcesReply *reply) {
        auto &self = *static_cast<Capture *>(context);
        ++self.calls;
        self.status = reply->status;
        self.request = reply->request_id;
        self.error = Copy(reply->error);
        if (reply->operation == PULSE_RESOURCES_STAT) {
            self.number = reply->file_stat.size_bytes;
            self.name = Copy(reply->file_stat.name);
            self.flags = reply->file_stat.flags;
        } else if (reply->operation == PULSE_RESOURCES_AUDIT_LIST) {
            self.count = reply->audit.count;
            if (self.count) {
                self.number = reply->audit.entries[0].sequence;
                self.name = Copy(reply->audit.entries[0].operation);
                self.value = Copy(reply->audit.entries[0].outcome);
            }
        } else {
            self.count = reply->settings.count;
            self.number = reply->settings.version;
            if (self.count) {
                self.name = Copy(reply->settings.values[0].key);
                self.value = Copy(reply->settings.values[0].value);
            }
        }
        if (self.hook)
            self.hook();
    }
};
struct Backend {
    authorization::Registry registry;
    bool hold = false, denied = false, malformed = false;
    unsigned dispatches = 0, cancels = 0;
    uint64_t version = 0;
    json::Value values = json::Value::Object(), last;
    std::vector<FilesBackend::Completion> completions;
    std::vector<authorization::OwnerHandle> owners;
    json::Value Reply(const json::Value &input) {
        if (denied)
            return Error("permission_denied");
        const auto op = input.get_string("op");
        auto data = json::Value::Object();
        if (op == "files.stat") {
            if (input.get_string("entry") != "opaque-entry")
                return Error("stale_reference");
            data.set("name", "alpha \xCE\xB2.txt");
            data.set("sizeBytes", malformed ? "18446744073709551616" : "18446744073709551615");
            data.set("modifiedFileTime", "133000000000000000");
            data.set("attributes", 32);
            data.set("directory", false);
            data.set("reparsePoint", true);
        } else if (op == "audit.list") {
            data = json::Value::Array();
            auto row = json::Value::Object();
            row.set("sequence", malformed ? "01" : "18446744073709551615");
            row.set("monotonicMs", "100");
            row.set("operation", "settings.set");
            row.set("outcome", "ok");
            // No such private fields are projected into the C ABI.
            row.set("privatePath", "C:\\private\\secret");
            data.push(std::move(row));
        } else {
            if (op == "settings.set") {
                int64_t expected = -1;
                input.find("expectedVersion")->get_int(expected, 0, (int64_t{1} << 53) - 1);
                if (static_cast<uint64_t>(expected) != version)
                    return Error("version_conflict");
                auto changed = json::Value::Object();
                const auto key = input.get_string("key");
                for (const auto &old : values.keys())
                    if (old != key)
                        changed.set(old, *values.find(old));
                if (!input.find("value")->is_null())
                    changed.set(key, *input.find("value"));
                values = std::move(changed);
                ++version;
            }
            data.set("version", version);
            data.set("values", values);
            if (malformed)
                data.set("version", -1);
        }
        return Success(std::move(data));
    }
    FilesBackend Functions() {
        FilesBackend result;
        result.live = [&](const CallerBinding &caller) { return registry.ValidateBinding(caller); };
        result.dispatch = [&](const CallerBinding &caller, const json::Value &input, FilesBackend::Completion done) {
            ++dispatches;
            last = input;
            owners.push_back(caller.Owner());
            if (hold)
                completions.push_back(std::move(done));
            else
                done(Reply(input));
            return FilesBackend::Cancel([&] { ++cancels; });
        };
        return result;
    }
};
} // namespace
int main() {
    Backend backend;
    authorization::PackageIdentity package{0, "sample.native", "revision", 1};
    backend.registry.SyncPackages({{package, true}});
    const auto owner = backend.registry.RegisterOwner(100), second = backend.registry.RegisterOwner(200);
    if (!owner || !second)
        return 2;
    const auto binding = backend.registry.CaptureBinding(package, *owner),
               other_binding = backend.registry.CaptureBinding(package, *second);
    if (!binding || !other_binding)
        return 2;
    ResourcesAdapter adapter(*binding, backend.Functions()), other(*other_binding, backend.Functions());
    PulseResourcesV1 api{};
    Capture capture;
    uint64_t request = 0;
    Check(adapter.Query(1, &api, sizeof(api)) == PULSE_OK && api.version == 1,
          "typed resources table is available by version");
    Check(adapter.Query(2, &api, sizeof(api)) == PULSE_ERROR_ABI &&
              adapter.Query(1, &api, sizeof(api) - 1) == PULSE_ERROR_ARGUMENT,
          "unsupported version and short storage rejected");
    std::string entry = "opaque-entry";
    Check(api.stat(api.context, View(entry), Capture::Done, &capture, &request) == PULSE_OK && !capture.calls,
          "stat callback is deferred after initiating ABI call");
    entry.assign("changed");
    Check(Pump([&] { return capture.calls == 1; }) && capture.status == PULSE_OK && capture.number == UINT64_MAX &&
              capture.flags == PULSE_RESOURCE_FILE_REPARSE_POINT && backend.last.get_string("entry") == "opaque-entry",
          "stat copies opaque input and preserves exact uint64 metadata");
    const auto copied_name = capture.name;
    capture = {};
    entry = "stale";
    api.stat(api.context, View(entry), Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && capture.error == "stale_reference" && capture.name.empty(),
          "host stale entry rejection has no partial metadata");
    Check(copied_name == "alpha \xCE\xB2.txt", "DLL-owned copy outlives borrowed callback data");
    capture = {};
    api.settings_get(api.context, Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && capture.status == PULSE_OK && capture.number == 0 &&
              !capture.count,
          "empty version-zero settings are represented without JSON");
    capture = {};
    std::string key = "prefix", value = "\xE6\x96\xB0-";
    api.settings_set(api.context, View(key), View(value), 0, 0, Capture::Done, &capture, &request);
    key.assign("changed");
    value.assign("changed");
    Check(Pump([&] { return capture.calls == 1; }) && capture.status == PULSE_OK && capture.number == 1 &&
              capture.name == "prefix" && capture.value == "\xE6\x96\xB0-",
          "settings copies UTF8 values and returns new version");
    capture = {};
    key = "prefix";
    value = "overwrite";
    api.settings_set(api.context, View(key), View(value), 0, 0, Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && capture.error == "version_conflict" && backend.version == 1,
          "optimistic version conflict cannot silently overwrite settings");
    capture = {};
    value.clear();
    api.settings_set(api.context, View(key), View(value), 0, 1, Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && capture.number == 2 && capture.count == 1 &&
              capture.value.empty(),
          "empty string remains a stored value");
    capture = {};
    api.settings_set(api.context, View(key), View(value), 1, 2, Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && capture.number == 3 && !capture.count &&
              backend.last.find("value")->is_null(),
          "explicit deletion maps to null and returns committed version");
    capture = {};
    api.audit_list(api.context, 128, Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && capture.number == UINT64_MAX && capture.count == 1 &&
              capture.name == "settings.set" && capture.value == "ok",
          "audit projects only typed operation outcome and exact counters");
    backend.denied = true;
    capture = {};
    api.audit_list(api.context, 1, Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && capture.error == "permission_denied" && !capture.count,
          "manifest permission rejection remains authoritative");
    backend.denied = false;
    backend.malformed = true;
    capture = {};
    entry = "opaque-entry";
    api.stat(api.context, View(entry), Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && capture.error == "invalid_host_response" && capture.name.empty(),
          "overflowing host metadata fails closed");
    capture = {};
    api.audit_list(api.context, 1, Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && capture.error == "invalid_host_response" && !capture.count,
          "noncanonical audit sequence is rejected");
    capture = {};
    api.settings_get(api.context, Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && capture.error == "invalid_host_response" && !capture.count,
          "invalid settings version cannot escape typed boundary");
    backend.malformed = false;
    Check(api.audit_list(api.context, 129, Capture::Done, &capture, &request) == PULSE_ERROR_ARGUMENT && !request,
          "audit limit enforced before dispatch");
    Check(api.settings_set(api.context, View(key), View(value), 0, UINT64_MAX, Capture::Done, &capture, &request) ==
              PULSE_ERROR_ARGUMENT,
          "settings version preserves service exact integer range");
    value = "not-empty";
    Check(api.settings_set(api.context, View(key), View(value), 1, 3, Capture::Done, &capture, &request) ==
              PULSE_ERROR_ARGUMENT,
          "ambiguous delete with nonempty value is rejected");
    key = "bad/key";
    Check(api.settings_set(api.context, View(key), View(value), 0, 3, Capture::Done, &capture, &request) ==
              PULSE_ERROR_ARGUMENT,
          "settings key cannot name a path");
    key = "prefix";
    value = std::string(1, static_cast<char>(0xff));
    Check(api.settings_set(api.context, View(key), View(value), 0, 3, Capture::Done, &capture, &request) ==
              PULSE_ERROR_ARGUMENT,
          "invalid UTF8 never reaches host settings service");
    PulseStatus wrong_thread = PULSE_OK;
    std::thread worker([&] {
        uint64_t id = 0;
        wrong_thread = api.settings_get(api.context, Capture::Done, &capture, &id);
    });
    worker.join();
    Check(wrong_thread == PULSE_ERROR_STATE, "ABI rejects calls from non-owner UI thread");
    backend.hold = true;
    capture = {};
    const auto before = backend.completions.size();
    api.settings_get(api.context, Capture::Done, &capture, &request);
    Check(Pump([&] { return backend.completions.size() == before + 1; }),
          "host async request begins through real message window");
    const auto cancel_before = backend.cancels;
    Check(api.cancel_request(api.context, request) == PULSE_OK && backend.cancels == cancel_before + 1,
          "cancel request invokes pending host cancellation");
    backend.completions.back()(backend.Reply(backend.last));
    Pump([] { return true; });
    Check(!capture.calls, "late cancelled completion cannot enter DLL");
    capture = {};
    const auto next = backend.completions.size();
    api.settings_get(api.context, Capture::Done, &capture, &request);
    Check(Pump([&] { return backend.completions.size() == next + 1; }), "second async completion is outstanding");
    auto background_done = backend.completions.back();
    auto response = backend.Reply(backend.last);
    std::thread completion_thread(
        [done = std::move(background_done), result = std::move(response)]() mutable { done(std::move(result)); });
    completion_thread.join();
    Check(!capture.calls, "background host result does not call DLL off-thread");
    Check(Pump([&] { return capture.calls == 1; }) && capture.number == 3,
          "background result is delivered on UI message pump");
    capture = {};
    auto other_api = other.Table();
    const auto count = backend.completions.size();
    other_api.settings_get(other_api.context, Capture::Done, &capture, &request);
    Check(Pump([&] { return backend.completions.size() == count + 1; }) && backend.owners.back() == *second &&
              api.cancel_request(api.context, request) == PULSE_ERROR_STATE,
          "another attachment keeps binding and cannot be cancelled by this adapter");
    other_api.cancel_request(other_api.context, request);
    backend.hold = false;
    capture = {};
    const auto dispatched = backend.dispatches;
    other_api.settings_get(other_api.context, Capture::Done, &capture, &request);
    backend.registry.UnregisterOwner(*second);
    Check(Pump([&] { return capture.calls == 1; }) && capture.error == "invalid_caller" &&
              backend.dispatches == dispatched,
          "revoked owner is rejected again at actual dispatch");
    // An attachment closing while a worker is outstanding must suppress even a
    // copied completion that arrives after its message-only window is destroyed.
    {
        ResourcesAdapter closing(*binding, backend.Functions());
        auto closing_api = closing.Table();
        backend.hold = true;
        Capture late;
        const auto held = backend.completions.size();
        closing_api.settings_get(closing_api.context, Capture::Done, &late, &request);
        Check(Pump([&] { return backend.completions.size() == held + 1; }), "close test holds actual dispatched work");
        const auto cancellations = backend.cancels;
        Check(closing.Close() == PULSE_OK && backend.cancels == cancellations + 1,
              "Close cancels an outstanding host request");
        backend.completions.back()(backend.Reply(backend.last));
        Pump([] { return true; });
        Check(!late.calls, "late result after Close cannot access DLL callback");
        backend.hold = false;
    }
    std::vector<uint64_t> pending;
    for (unsigned index = 0; index < 64; ++index) {
        uint64_t id = 0;
        if (api.settings_get(api.context, Capture::Done, &capture, &id) == PULSE_OK)
            pending.push_back(id);
    }
    Check(pending.size() == 64 &&
              api.settings_get(api.context, Capture::Done, &capture, &request) == PULSE_ERROR_STATE && !request,
          "bounded pending queue rejects request 65 before dispatch");
    for (auto id : pending)
        api.cancel_request(api.context, id);
    capture = {};
    bool active = false;
    PulseStatus close = PULSE_OK;
    capture.hook = [&] {
        active = adapter.CallbackActive();
        close = adapter.Close();
    };
    api.settings_get(api.context, Capture::Done, &capture, &request);
    Check(Pump([&] { return capture.calls == 1; }) && active && close == PULSE_ERROR_STATE && !adapter.CallbackActive(),
          "reentrant Close suppresses future delivery and requires delayed DLL unloading");
    Check(api.settings_get(api.context, Capture::Done, &capture, &request) == PULSE_ERROR_STATE && !request,
          "closed attachment cannot issue requests");
    other.Close();
    return failures ? 1 : 0;
}
