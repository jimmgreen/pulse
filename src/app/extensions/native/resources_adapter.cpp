#include "resources_adapter.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include <windows.h>

namespace pulse::extensions::native {
namespace {
constexpr UINT kWake = WM_APP + 0x71;
constexpr size_t kPendingLimit = 64;
std::atomic<uint64_t> next_request{1};
json::Value Failure(const char *code) {
    auto result = json::Value::Object(), error = json::Value::Object();
    result.set("ok", false);
    error.set("code", code);
    result.set("error", std::move(error));
    return result;
}
bool Copy(PulseStringView input, std::string &output, uint32_t maximum, bool required = true) {
    if (input.length > maximum || (input.length && !input.data) || (required && !input.length))
        return false;
    output.assign(input.length ? input.data : "", input.length);
    return output.find('\0') == std::string::npos &&
           (output.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, output.data(),
                                                  static_cast<int>(output.size()), nullptr, 0) > 0);
}
PulseStringView View(const std::string &value) { return {value.data(), static_cast<uint32_t>(value.size())}; }
bool Text(const json::Value &object, const char *name, PulseStringView &output, bool nullable = false) {
    const auto *value = object.find(name);
    if (!value || (nullable && value->is_null()))
        return nullable;
    if (!value->is_string() || value->as_string().size() > 65536)
        return false;
    output = View(value->as_string());
    return true;
}
bool Decimal(const json::Value &object, const char *name, uint64_t &output) {
    const auto *value = object.find(name);
    if (!value || !value->is_string())
        return false;
    const auto &text = value->as_string();
    if (text.empty() || (text.size() > 1 && text.front() == '0'))
        return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool Integer(const json::Value &object, const char *name, uint64_t &output, int64_t maximum = (int64_t{1} << 53) - 1) {
    const auto *value = object.find(name);
    int64_t number = 0;
    if (!value || !value->get_int(number, 0, maximum))
        return false;
    output = static_cast<uint64_t>(number);
    return true;
}
bool Boolean(const json::Value &object, const char *name, uint32_t &output) {
    const auto *value = object.find(name);
    if (!value || !value->is_bool())
        return false;
    output = value->as_bool() ? 1u : 0u;
    return true;
}
bool Identifier(std::string_view key) {
    return !key.empty() && key.size() <= 64 && std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-';
    });
}
struct ReplyStorage {
    json::Value response;
    PulseResourcesReply reply{};
    std::vector<PulseSetting> settings;
    std::vector<PulseAuditEntry> audit;
    ReplyStorage(json::Value value, uint32_t operation, uint64_t id) : response(std::move(value)) {
        reply.size = sizeof(reply);
        reply.operation = operation;
        reply.request_id = id;
        if (!response.get_bool("ok")) {
            reply.status = PULSE_ERROR_STATE;
            const auto *error = response.find("error");
            if (!error || !Text(*error, "code", reply.error))
                Invalid();
            return;
        }
        const auto *data = response.find("data");
        if (!data || !Decode(*data))
            Invalid();
    }
    void Invalid() {
        static constexpr char code[] = "invalid_host_response";
        reply.status = PULSE_ERROR_STATE;
        reply.error = {code, sizeof(code) - 1};
        reply.file_stat = {};
        reply.settings = {};
        reply.audit = {};
    }
    bool Decode(const json::Value &data) {
        if (reply.operation == PULSE_RESOURCES_STAT) {
            auto &file = reply.file_stat;
            uint64_t attributes = 0;
            uint32_t directory = 0, reparse = 0;
            if (!data.is_object() || !Text(data, "name", file.name) || !Decimal(data, "sizeBytes", file.size_bytes) ||
                !Decimal(data, "modifiedFileTime", file.modified_filetime) ||
                !Integer(data, "attributes", attributes, UINT32_MAX) || !Boolean(data, "directory", directory) ||
                !Boolean(data, "reparsePoint", reparse))
                return false;
            file.attributes = static_cast<uint32_t>(attributes);
            file.flags =
                (directory ? PULSE_RESOURCE_FILE_DIRECTORY : 0u) | (reparse ? PULSE_RESOURCE_FILE_REPARSE_POINT : 0u);
            return true;
        }
        if (reply.operation == PULSE_RESOURCES_SETTINGS_GET || reply.operation == PULSE_RESOURCES_SETTINGS_SET) {
            if (!data.is_object() || !Integer(data, "version", reply.settings.version))
                return false;
            const auto *values = data.find("values");
            if (!values || !values->is_object() || values->size() > 64)
                return false;
            for (const auto &key : values->keys()) {
                const auto *value = values->find(key);
                if (!Identifier(key) || !value || !value->is_string() || value->as_string().size() > 1024)
                    return false;
                settings.push_back({View(key), View(value->as_string())});
            }
            reply.settings.values = settings.data();
            reply.settings.count = static_cast<uint32_t>(settings.size());
            return true;
        }
        if (reply.operation != PULSE_RESOURCES_AUDIT_LIST || !data.is_array() || data.size() > 128)
            return false;
        for (const auto &value : data.items()) {
            PulseAuditEntry item{};
            if (!value.is_object() || !Decimal(value, "sequence", item.sequence) ||
                !Decimal(value, "monotonicMs", item.monotonic_ms) || !Text(value, "operation", item.operation) ||
                !Text(value, "outcome", item.outcome) || item.operation.length > 128 || item.outcome.length > 128)
                return false;
            audit.push_back(item);
        }
        reply.audit.entries = audit.data();
        reply.audit.count = static_cast<uint32_t>(audit.size());
        return true;
    }
};
} // namespace
struct ResourcesAdapter::Impl : std::enable_shared_from_this<Impl> {
    struct Request {
        uint64_t id;
        uint32_t operation;
        json::Value input;
        PulseResourcesCompletionFn completion;
        void *user;
        bool dispatched = false;
        FilesBackend::Cancel cancel;
    };
    explicit Impl(CallerBinding value, FilesBackend functions)
        : caller(std::move(value)), backend(std::move(functions)) {}
    CallerBinding caller;
    FilesBackend backend;
    const DWORD thread = GetCurrentThreadId();
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> closed{false};
    uint32_t callbacks = 0;
    std::unordered_map<uint64_t, Request> requests;
    std::mutex inbox_mutex;
    std::vector<std::pair<uint64_t, json::Value>> inbox;
    ~Impl() { Close(); }
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && (message == kWake || message == WM_TIMER)) {
            const auto keep_alive = self->shared_from_this();
            try {
                keep_alive->Pump();
            } catch (...) {
                keep_alive->Close();
            }
            return 0;
        }
        if (message == WM_NCDESTROY)
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
    }
    bool Open() {
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpfnWndProc = Procedure;
        type.lpszClassName = L"PulseNativeResourcesAdapter";
        if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        const HWND value =
            CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, this);
        window = value;
        if (!value)
            return false;
        if (!SetTimer(value, 1, 25, nullptr)) {
            DestroyWindow(value);
            window = nullptr;
            return false;
        }
        return true;
    }
    PulseStatus Ready() const {
        if (GetCurrentThreadId() != thread || closed)
            return PULSE_ERROR_STATE;
        return window && backend.dispatch && backend.live ? PULSE_OK : PULSE_ERROR_UNAVAILABLE;
    }
    PulseStatus Submit(uint32_t operation, json::Value input, PulseResourcesCompletionFn callback, void *user,
                       uint64_t *output) {
        if (!output)
            return PULSE_ERROR_ARGUMENT;
        *output = 0;
        if (!callback)
            return PULSE_ERROR_ARGUMENT;
        const auto status = Ready();
        if (status != PULSE_OK)
            return status;
        if (requests.size() >= kPendingLimit)
            return PULSE_ERROR_STATE;
        const uint64_t id = next_request.fetch_add(1);
        if (!id)
            return PULSE_ERROR_STATE;
        requests.emplace(id, Request{id, operation, std::move(input), callback, user, false, {}});
        *output = id;
        PostMessageW(window.load(), kWake, 0, 0);
        return PULSE_OK;
    }
    void Complete(uint64_t id, json::Value response) {
        if (closed)
            return;
        {
            std::lock_guard lock(inbox_mutex);
            if (closed)
                return;
            inbox.emplace_back(id, std::move(response));
        }
        if (const HWND value = window.load())
            PostMessageW(value, kWake, 0, 0);
    }
    PulseStatus Cancel(uint64_t id) {
        const auto status = Ready();
        if (status != PULSE_OK)
            return status;
        const auto found = requests.find(id);
        if (found == requests.end())
            return PULSE_ERROR_STATE;
        auto cancel = std::move(found->second.cancel);
        requests.erase(found);
        if (cancel)
            cancel();
        return PULSE_OK;
    }
    PulseStatus Close() noexcept {
        if (GetCurrentThreadId() != thread)
            return PULSE_ERROR_STATE;
        closed = true;
        auto retired = std::move(requests);
        requests.clear();
        for (auto &[id, request] : retired) {
            (void)id;
            try {
                if (request.cancel)
                    request.cancel();
            } catch (...) {
            }
        }
        {
            std::lock_guard lock(inbox_mutex);
            inbox.clear();
        }
        const HWND value = window.exchange(nullptr);
        if (value)
            DestroyWindow(value);
        return callbacks ? PULSE_ERROR_STATE : PULSE_OK;
    }
    void Pump() {
        if (closed)
            return;
        std::vector<uint64_t> pending;
        for (const auto &[id, request] : requests)
            if (!request.dispatched)
                pending.push_back(id);
        for (uint64_t id : pending) {
            auto found = requests.find(id);
            if (found == requests.end() || closed)
                continue;
            found->second.dispatched = true;
            if (!backend.live(caller)) {
                Complete(id, Failure("invalid_caller"));
                continue;
            }
            const auto weak = weak_from_this();
            FilesBackend::Cancel cancel;
            try {
                // Backend may complete synchronously or pump a native modal review.
                // Keep a copy so cancellation/reentrancy cannot invalidate its input.
                const auto input = found->second.input;
                cancel = backend.dispatch(caller, input, [weak, id](json::Value response) {
                    if (auto self = weak.lock())
                        self->Complete(id, std::move(response));
                });
            } catch (...) {
                Complete(id, Failure("internal_error"));
            }
            found = requests.find(id);
            if (found != requests.end() && !closed)
                found->second.cancel = std::move(cancel);
            else if (cancel)
                cancel();
        }
        std::vector<std::pair<uint64_t, json::Value>> ready;
        {
            std::lock_guard lock(inbox_mutex);
            ready.swap(inbox);
        }
        for (auto &[id, response] : ready) {
            if (closed)
                break;
            const auto found = requests.find(id);
            if (found == requests.end())
                continue;
            auto request = std::move(found->second);
            requests.erase(found);
            if (!backend.live(caller))
                response = Failure("invalid_caller");
            ReplyStorage storage(std::move(response), request.operation, id);
            ++callbacks;
            try {
                request.completion(request.user, &storage.reply);
            } catch (...) {
                Close();
            }
            --callbacks;
        }
    }
};
namespace {
using Impl = ResourcesAdapter::Impl;
template <class Work> PulseStatus Boundary(void *context, uint64_t *output, Work work) noexcept {
    if (output)
        *output = 0;
    if (!context || !output)
        return PULSE_ERROR_ARGUMENT;
    auto *self = static_cast<Impl *>(context);
    if (const auto status = self->Ready(); status != PULSE_OK)
        return status;
    try {
        return work(*self);
    } catch (...) {
        return PULSE_ERROR_UNAVAILABLE;
    }
}
PulseStatus PULSE_CALL Stat(void *context, PulseStringView entry, PulseResourcesCompletionFn done, void *user,
                            uint64_t *output) {
    return Boundary(context, output, [&](Impl &self) {
        std::string copied;
        if (!Copy(entry, copied, 64))
            return static_cast<PulseStatus>(PULSE_ERROR_ARGUMENT);
        auto input = json::Value::Object();
        input.set("op", "files.stat");
        input.set("entry", std::move(copied));
        return self.Submit(PULSE_RESOURCES_STAT, std::move(input), done, user, output);
    });
}
PulseStatus PULSE_CALL SettingsGet(void *context, PulseResourcesCompletionFn done, void *user, uint64_t *output) {
    return Boundary(context, output, [&](Impl &self) {
        auto input = json::Value::Object();
        input.set("op", "settings.get");
        return self.Submit(PULSE_RESOURCES_SETTINGS_GET, std::move(input), done, user, output);
    });
}
PulseStatus PULSE_CALL SettingsSet(void *context, PulseStringView key, PulseStringView value, uint32_t remove,
                                   uint64_t expected_version, PulseResourcesCompletionFn done, void *user,
                                   uint64_t *output) {
    return Boundary(context, output, [&](Impl &self) {
        std::string copied_key, copied_value;
        if (remove > 1 || expected_version > (uint64_t{1} << 53) - 1 || !Copy(key, copied_key, 64) ||
            !Identifier(copied_key) || !Copy(value, copied_value, 1024, false) || (remove && !copied_value.empty()))
            return static_cast<PulseStatus>(PULSE_ERROR_ARGUMENT);
        auto input = json::Value::Object();
        input.set("op", "settings.set");
        input.set("key", std::move(copied_key));
        input.set("value", remove ? json::Value() : json::Value(std::move(copied_value)));
        input.set("expectedVersion", expected_version);
        return self.Submit(PULSE_RESOURCES_SETTINGS_SET, std::move(input), done, user, output);
    });
}
PulseStatus PULSE_CALL AuditList(void *context, uint32_t limit, PulseResourcesCompletionFn done, void *user,
                                 uint64_t *output) {
    return Boundary(context, output, [&](Impl &self) {
        if (!limit || limit > 128)
            return static_cast<PulseStatus>(PULSE_ERROR_ARGUMENT);
        auto input = json::Value::Object();
        input.set("op", "audit.list");
        input.set("limit", static_cast<int>(limit));
        return self.Submit(PULSE_RESOURCES_AUDIT_LIST, std::move(input), done, user, output);
    });
}
PulseStatus PULSE_CALL CancelRequest(void *context, uint64_t id) {
    if (!context || !id)
        return PULSE_ERROR_ARGUMENT;
    try {
        return static_cast<Impl *>(context)->Cancel(id);
    } catch (...) {
        return PULSE_ERROR_UNAVAILABLE;
    }
}
} // namespace
ResourcesAdapter::ResourcesAdapter(CallerBinding caller, FilesBackend backend)
    : impl_(std::make_shared<Impl>(std::move(caller), std::move(backend))) {
    impl_->Open();
}
ResourcesAdapter::~ResourcesAdapter() { Close(); }
PulseResourcesV1 ResourcesAdapter::Table() const noexcept {
    return {sizeof(PulseResourcesV1),
            PULSE_RESOURCES_VERSION_1,
            impl_.get(),
            Stat,
            SettingsGet,
            SettingsSet,
            AuditList,
            CancelRequest};
}
PulseStatus ResourcesAdapter::Query(uint32_t version, void *output, uint32_t size) const noexcept {
    if (!output || size < sizeof(PulseResourcesV1))
        return PULSE_ERROR_ARGUMENT;
    if (version != PULSE_RESOURCES_VERSION_1)
        return PULSE_ERROR_ABI;
    if (const auto status = impl_->Ready(); status != PULSE_OK)
        return status;
    const auto table = Table();
    std::memcpy(output, &table, sizeof(table));
    return PULSE_OK;
}
PulseStatus ResourcesAdapter::Close() noexcept { return impl_ ? impl_->Close() : PULSE_OK; }
bool ResourcesAdapter::CallbackActive() const noexcept { return impl_ && impl_->callbacks != 0; }
} // namespace pulse::extensions::native
