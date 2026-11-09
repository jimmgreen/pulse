#include <windows.h>
#include "files_adapter.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
struct ReplyStorage {
    json::Value response;
    PulseFilesReply reply{};
    std::vector<PulseFileEntry> files;
    std::vector<PulseRenameRow> rows;
    std::vector<PulseFileTaskItem> items;
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
        if (!data || !data->is_object() || !Decode(*data))
            Invalid();
    }
    void Invalid() {
        reply.status = PULSE_ERROR_STATE;
        constexpr char error[] = "invalid_host_response";
        static constexpr char stable[] = "invalid_host_response";
        reply.error = {stable, sizeof(error) - 1};
        reply.directory = {};
        reply.page = {};
        reply.plan = {};
        reply.task = {};
        reply.cancel_accepted = 0;
    }
    bool Decode(const json::Value &data) {
        if (reply.operation == PULSE_FILES_CONTEXT) {
            return Text(data, "directory", reply.directory.directory) &&
                   Text(data, "selectionName", reply.directory.selection_name) &&
                   Text(data, "viewMode", reply.directory.view_mode, true) &&
                   Integer(data, "selectionCount", reply.directory.selection_count) &&
                   Boolean(data, "loading", reply.directory.loading);
        }
        if (reply.operation == PULSE_FILES_LIST) {
            const auto *values = data.find("entries");
            if (!values || !values->is_array() || values->size() > 256 ||
                !Text(data, "nextCursor", reply.page.next_cursor) || !Integer(data, "total", reply.page.total))
                return false;
            for (const auto &value : values->items()) {
                PulseFileEntry item{};
                uint64_t attributes = 0;
                uint32_t directory = 0, reparse = 0;
                if (!Text(value, "entry", item.entry) || !Text(value, "name", item.name) ||
                    !Decimal(value, "sizeBytes", item.size_bytes) ||
                    !Decimal(value, "modifiedFileTime", item.modified_filetime) ||
                    !Integer(value, "attributes", attributes, UINT32_MAX) || !Boolean(value, "directory", directory) ||
                    !Boolean(value, "reparsePoint", reparse))
                    return false;
                item.attributes = static_cast<uint32_t>(attributes);
                item.flags = (directory ? PULSE_FILE_DIRECTORY : 0u) | (reparse ? PULSE_FILE_REPARSE_POINT : 0u);
                files.push_back(item);
            }
            reply.page.entries = files.data();
            reply.page.count = static_cast<uint32_t>(files.size());
            return true;
        }
        if (reply.operation == PULSE_FILES_PREPARE_RENAME) {
            const auto *values = data.find("rows");
            if (!values || !values->is_array() || !values->size() || values->size() > 64 ||
                !Text(data, "planId", reply.plan.plan_id) ||
                !Integer(data, "expiresInMs", reply.plan.expires_in_ms, 60000))
                return false;
            for (const auto &value : values->items()) {
                PulseRenameRow row{};
                if (!Text(value, "entry", row.entry) || !Text(value, "originalName", row.original_name) ||
                    !Text(value, "newLeafName", row.new_leaf_name))
                    return false;
                const auto status = value.get_string("status");
                if (status != "ready" && status != "unchanged")
                    return false;
                row.action = status == "ready" ? PULSE_RENAME_READY : PULSE_RENAME_UNCHANGED;
                rows.push_back(row);
            }
            reply.plan.rows = rows.data();
            reply.plan.count = static_cast<uint32_t>(rows.size());
            return true;
        }
        if (!Decimal(data, "taskId", reply.task.task_id) || !reply.task.task_id)
            return false;
        if (reply.operation == PULSE_FILES_COMMIT_RENAME)
            return data.get_string("status") == "submitted";
        if (reply.operation == PULSE_FILES_TASK_CANCEL)
            return Boolean(data, "accepted", reply.cancel_accepted);
        if (reply.operation != PULSE_FILES_TASK_GET || !Boolean(data, "cancelRequested", reply.task.cancel_requested) ||
            !Boolean(data, "terminal", reply.task.terminal) || !Boolean(data, "completed", reply.task.completed) ||
            !Decimal(data, "transferredBytes", reply.task.transferred_bytes) ||
            !Decimal(data, "totalBytes", reply.task.total_bytes))
            return false;
        const auto *values = data.find("items");
        if (!values || !values->is_array() || values->size() > 4096)
            return false;
        for (const auto &value : values->items()) {
            PulseFileTaskItem item{};
            uint64_t state = 0, error = 0, cleanup = 0;
            if (!Text(value, "source", item.source) || !Text(value, "destination", item.destination) ||
                !Integer(value, "state", state, 5) || !Integer(value, "error", error, UINT32_MAX) ||
                !Integer(value, "cleanupError", cleanup, UINT32_MAX))
                return false;
            item.state = static_cast<uint32_t>(state);
            item.system_error = static_cast<uint32_t>(error);
            item.cleanup_error = static_cast<uint32_t>(cleanup);
            items.push_back(item);
        }
        reply.task.items = items.data();
        reply.task.count = static_cast<uint32_t>(items.size());
        return true;
    }
};
} // namespace
struct FilesAdapter::Impl : std::enable_shared_from_this<Impl> {
    struct Request {
        uint64_t id;
        uint32_t operation;
        json::Value input;
        PulseFilesCompletionFn completion;
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
    std::unordered_set<uint64_t> owned_tasks;
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
        type.lpszClassName = L"PulseNativeFilesAdapter";
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
    PulseStatus Submit(uint32_t operation, json::Value input, PulseFilesCompletionFn callback, void *user,
                       uint64_t *output) {
        if (!output)
            return PULSE_ERROR_ARGUMENT;
        *output = 0;
        if (!callback)
            return PULSE_ERROR_ARGUMENT;
        const auto status = Ready();
        if (status != PULSE_OK)
            return status;
        if (requests.size() >= kPendingLimit || (operation == PULSE_FILES_COMMIT_RENAME && owned_tasks.size() >= 256))
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
        auto tasks = std::move(owned_tasks);
        owned_tasks.clear();
        for (uint64_t task : tasks) {
            try {
                if (!backend.live || !backend.dispatch || !backend.live(caller))
                    break;
                auto input = json::Value::Object();
                input.set("op", "operations.cancel");
                input.set("taskId", std::to_string(task));
                // No DLL callback survives close. Only this adapter's committed task
                // handles are cancelled; completed mutations are never rolled back.
                backend.dispatch(caller, input, [](json::Value) {});
            } catch (...) {
            }
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
            if (storage.reply.status == PULSE_OK && request.operation == PULSE_FILES_COMMIT_RENAME)
                owned_tasks.insert(storage.reply.task.task_id);
            if (request.operation == PULSE_FILES_TASK_GET) {
                uint64_t task = 0;
                if (Decimal(request.input, "taskId", task) &&
                    ((storage.reply.status == PULSE_OK && storage.reply.task.terminal) ||
                     (storage.reply.status != PULSE_OK &&
                      std::string_view(storage.reply.error.data, storage.reply.error.length) == "not_found")))
                    owned_tasks.erase(task);
            }
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
using Impl = FilesAdapter::Impl;
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
PulseStatus PULSE_CALL Context(void *context, PulseFilesCompletionFn done, void *user, uint64_t *output) {
    return Boundary(context, output, [&](Impl &self) {
        auto input = json::Value::Object();
        input.set("op", "context.get");
        return self.Submit(PULSE_FILES_CONTEXT, std::move(input), done, user, output);
    });
}
PulseStatus PULSE_CALL List(void *context, PulseStringView cursor, uint32_t limit, PulseFilesCompletionFn done,
                            void *user, uint64_t *output) {
    return Boundary(context, output, [&](Impl &self) {
        std::string copied;
        if (!limit || limit > 256 || !Copy(cursor, copied, 64, false))
            return static_cast<PulseStatus>(PULSE_ERROR_ARGUMENT);
        auto input = json::Value::Object();
        input.set("op", "files.list");
        input.set("limit", static_cast<int>(limit));
        if (!copied.empty())
            input.set("cursor", std::move(copied));
        return self.Submit(PULSE_FILES_LIST, std::move(input), done, user, output);
    });
}
PulseStatus PULSE_CALL Prepare(void *context, const PulseRenameInput *rows, uint32_t count, PulseFilesCompletionFn done,
                               void *user, uint64_t *output) {
    return Boundary(context, output, [&](Impl &self) {
        if (!rows || !count || count > 64)
            return static_cast<PulseStatus>(PULSE_ERROR_ARGUMENT);
        auto input = json::Value::Object(), items = json::Value::Array();
        input.set("op", "files.prepareRename");
        for (uint32_t i = 0; i < count; ++i) {
            std::string entry, name;
            if (!Copy(rows[i].entry, entry, 64) || !Copy(rows[i].new_leaf_name, name, 1024))
                return static_cast<PulseStatus>(PULSE_ERROR_ARGUMENT);
            auto item = json::Value::Object();
            item.set("entry", std::move(entry));
            item.set("newLeafName", std::move(name));
            items.push(std::move(item));
        }
        input.set("items", std::move(items));
        return self.Submit(PULSE_FILES_PREPARE_RENAME, std::move(input), done, user, output);
    });
}
PulseStatus PULSE_CALL Commit(void *context, PulseStringView plan, PulseFilesCompletionFn done, void *user,
                              uint64_t *output) {
    return Boundary(context, output, [&](Impl &self) {
        std::string copied;
        if (!Copy(plan, copied, 64))
            return static_cast<PulseStatus>(PULSE_ERROR_ARGUMENT);
        auto input = json::Value::Object();
        input.set("op", "files.commitRename");
        input.set("planId", std::move(copied));
        return self.Submit(PULSE_FILES_COMMIT_RENAME, std::move(input), done, user, output);
    });
}
PulseStatus Task(void *context, uint64_t task, bool cancel, PulseFilesCompletionFn done, void *user, uint64_t *output) {
    return Boundary(context, output, [&](Impl &self) {
        if (!task)
            return static_cast<PulseStatus>(PULSE_ERROR_ARGUMENT);
        auto input = json::Value::Object();
        input.set("op", cancel ? "operations.cancel" : "operations.get");
        input.set("taskId", std::to_string(task));
        return self.Submit(cancel ? PULSE_FILES_TASK_CANCEL : PULSE_FILES_TASK_GET, std::move(input), done, user,
                           output);
    });
}
PulseStatus PULSE_CALL GetTask(void *context, uint64_t task, PulseFilesCompletionFn done, void *user,
                               uint64_t *output) {
    return Task(context, task, false, done, user, output);
}
PulseStatus PULSE_CALL CancelTask(void *context, uint64_t task, PulseFilesCompletionFn done, void *user,
                                  uint64_t *output) {
    return Task(context, task, true, done, user, output);
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
FilesAdapter::FilesAdapter(CallerBinding caller, FilesBackend backend)
    : impl_(std::make_shared<Impl>(std::move(caller), std::move(backend))) {
    impl_->Open();
}
FilesAdapter::~FilesAdapter() { Close(); }
PulseFilesV1 FilesAdapter::Table() const noexcept {
    return {sizeof(PulseFilesV1), PULSE_FILES_VERSION_1, impl_.get(), Context, List, Prepare, Commit, GetTask,
            CancelTask,           CancelRequest};
}
PulseStatus FilesAdapter::Query(uint32_t version, void *output, uint32_t size) const noexcept {
    if (!output || size < sizeof(PulseFilesV1))
        return PULSE_ERROR_ARGUMENT;
    if (version != PULSE_FILES_VERSION_1)
        return PULSE_ERROR_ABI;
    if (const auto status = impl_->Ready(); status != PULSE_OK)
        return status;
    const auto table = Table();
    std::memcpy(output, &table, sizeof(table));
    return PULSE_OK;
}
PulseStatus FilesAdapter::Close() noexcept { return impl_ ? impl_->Close() : PULSE_OK; }
bool FilesAdapter::CallbackActive() const noexcept { return impl_ && impl_->callbacks != 0; }
} // namespace pulse::extensions::native
