#include "rename_service.h"
#include "audit_log.h"
#include "../../common/rename_identity.h"
#include "../../ui/batch_rename_dialog.h"
#include "../app_navigation.h"
#include "../app_runtime.h"
#include "../batch_rename_plan.h"
#include "rename_plan.h"
#include "resource_contract.h"
#include "resource_service.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pulse::extensions {
namespace {
namespace rn = app::rename;
constexpr uint64_t kLifetime = 60000;
RenameReviewProbe review_probe;
struct Plan : RenameDiskPlan {
    explicit Plan(CallerBinding value) : principal(std::move(value)) {}
    CallerBinding principal;
    HWND owner = nullptr;
    app::Tab *tab = nullptr; // Identity only; never dereferenced after a modal call.
    fs::SnapshotPtr snapshot;
    std::string id;
    uint64_t expires = 0, view_generation = 0;
    std::vector<std::string> entries;
    std::atomic<bool> cancelled{false};
};
struct Job {
    std::shared_ptr<Plan> plan;
    RenameService::Completion complete;
    HWND window = nullptr;
    bool committing = false, presenting = false;
    std::mutex mutex;
    std::optional<std::string> result;
};
std::unordered_map<std::string, std::shared_ptr<Plan>> plans;
std::unordered_map<HWND, std::shared_ptr<Job>> jobs;
struct TaskRecord {
    std::shared_ptr<Plan> plan;
    std::mutex mutex;
    std::optional<ops::ControlledTransferResult> result;
};
std::unordered_map<uint64_t, std::shared_ptr<TaskRecord>> tasks;

bool SamePrincipal(const CallerBinding &a, const CallerBinding &b) {
    return a.Package() == b.Package() && a.Owner() == b.Owner();
}
std::optional<CallerBinding> Principal(AppState &owner, const CallerLease &caller) {
    if (!ValidateCaller(caller) || !owner.hwnd || caller.owner.key != reinterpret_cast<uintptr_t>(owner.hwnd))
        return {};
    const auto result = CaptureCallerBinding(caller.package.slot, caller.owner.key);
    if (!result || result->Package() != caller.package || result->Owner() != caller.owner)
        return {};
    return result;
}
AppState *Owner(const Plan &plan) {
    if (!IsWindow(plan.owner))
        return nullptr;
    return reinterpret_cast<AppState *>(GetWindowLongPtrW(plan.owner, GWLP_USERDATA));
}
bool Current(const Plan &plan) {
    if (plan.cancelled || GetTickCount64() >= plan.expires || !ValidateCallerBinding(plan.principal))
        return false;
    auto *owner = Owner(plan);
    auto *tab = owner ? ActiveTab(*owner) : nullptr;
    return tab && tab == plan.tab && !tab->loading && !tab->pending_generation && !tab->search_entries &&
           !tab->content_results && !tab->search_content_active && tab->snapshot == plan.snapshot &&
           tab->view_generation == plan.view_generation && tab->current_path == plan.directory &&
           tab->snapshot_path == plan.directory;
}
std::string Nonce() {
    std::array<uint8_t, 24> bytes{};
    if (!authorization::SecureRandomBytes(bytes))
        return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string value;
    for (auto byte : bytes) {
        value += hex[byte >> 4];
        value += hex[byte & 15];
    }
    return value;
}
std::wstring Wide(const std::string &text) {
    if (text.empty() || text.size() > 1024 || text.find('\0') != std::string::npos)
        return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
json::Value Prepared(const Plan &plan) {
    auto data = json::Value::Object(), rows = json::Value::Array();
    data.set("planId", plan.id);
    const auto now = GetTickCount64();
    data.set("expiresInMs", static_cast<uint64_t>(plan.expires > now ? plan.expires - now : 0));
    for (size_t i = 0; i < plan.sources.size(); ++i) {
        auto row = json::Value::Object();
        row.set("entry", plan.entries[i]);
        row.set("originalName", json::Utf8(plan.sources[i].name));
        row.set("newLeafName", json::Utf8(plan.names[i]));
        row.set("status", plan.preview.rows[i].Renames() ? "ready" : "unchanged");
        rows.push(std::move(row));
    }
    data.set("rows", std::move(rows));
    return ResourceSuccess(std::move(data));
}
void Finish(const std::shared_ptr<Job> &job, json::Value result) {
    const HWND window = std::exchange(job->window, nullptr);
    if (window) {
        jobs.erase(window);
        DestroyWindow(window);
    }
    auto callback = std::move(job->complete);
    if (callback)
        callback(std::move(result));
}
void Present(const std::shared_ptr<Job> &job) {
    auto plan = job->plan;
    job->presenting = true;
    ui::BatchRenameDialogOptions options;
    options.proposed_names = plan->names;
    options.proposed_sources = plan->sources;
    options.proposed_plan = plan->preview;
    options.review_valid = [plan] { return Current(*plan); };
    std::vector<std::wstring> paths;
    for (const auto &source : plan->sources)
        paths.push_back(source.path);
    auto *owner = Owner(*plan);
    if (!owner || !Current(*plan)) {
        Finish(job, ResourceFailure("stale_plan"));
        return;
    }
    if (owner->isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_EXTENSION_CAPTURE", nullptr, 0))
        options.on_review_ready = review_probe;
    const auto review = ui::ShowBatchRenameDialog(plan->owner, paths, owner->darkMode, owner->accentColor, options);
    if (!review.accepted || !Current(*plan)) {
        Finish(job, ResourceFailure("cancelled"));
        return;
    }
    owner = Owner(*plan);
    if (!owner) {
        Finish(job, ResourceFailure("invalid_caller"));
        return;
    }
    size_t owner_tasks = 0;
    for (const auto &[id, task] : tasks)
        if (task->plan->principal.Owner() == plan->principal.Owner())
            ++owner_tasks;
    if (owner_tasks >= 32 || tasks.size() >= 256) {
        for (auto at = tasks.begin(); at != tasks.end();) {
            const bool same_owner = at->second->plan->principal.Owner() == plan->principal.Owner();
            bool terminal = false;
            {
                std::lock_guard lock(at->second->mutex);
                terminal = at->second->result.has_value();
            }
            if (terminal && ((same_owner && owner_tasks >= 32) || tasks.size() >= 256)) {
                if (same_owner)
                    --owner_tasks;
                at = tasks.erase(at);
            } else
                ++at;
        }
        if (owner_tasks >= 32 || tasks.size() >= 256) {
            Finish(job, ResourceFailure("task_budget"));
            return;
        }
    }
    const auto record = std::make_shared<TaskRecord>();
    record->plan = plan;
    ops::OpRequest request;
    request.type = ops::OpType::BatchRename;
    request.rename_completed = [record](ops::ControlledTransferResult result) {
        std::lock_guard lock(record->mutex);
        record->result = std::move(result);
    };
    for (size_t index : plan->preview.exec_order) {
        request.sources.push_back(plan->sources[index].path);
        request.new_names.push_back(plan->names[index]);
        request.authorized_rename_identities.push_back(plan->identities[index]);
    }
    request.rename_authorized = [plan] {
        return !plan->cancelled && HostAuthorizationRegistry().ValidateBinding(plan->principal);
    };
    const uint64_t task = owner->ops.Submit(std::move(request));
    RecordExtensionCall(plan->principal, "files.rename.commit", task ? "submitted" : "submit_failed");
    if (!task) {
        Finish(job, ResourceFailure("submit_failed"));
        return;
    }
    tasks.emplace(task, record);
    if (auto *tab = ActiveTab(*owner); tab == plan->tab) {
        tab->pending_selected_names.clear();
        for (size_t index : plan->preview.exec_order)
            tab->pending_selected_names.push_back(plan->names[index]);
        if (!tab->pending_selected_names.empty())
            tab->pending_selected_name = tab->pending_selected_names.front();
    }
    auto data = json::Value::Object();
    data.set("taskId", std::to_string(task));
    data.set("status", "submitted");
    Finish(job, ResourceSuccess(std::move(data)));
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message != WM_TIMER)
        return DefWindowProcW(window, message, wparam, lparam);
    const auto found = jobs.find(window);
    if (found == jobs.end())
        return 0;
    const auto job = found->second;
    if (job->presenting)
        return 0;
    if (!Current(*job->plan)) {
        job->plan->cancelled = true;
        plans.erase(job->plan->id);
        Finish(job, ResourceFailure("stale_plan"));
        return 0;
    }
    std::optional<std::string> error;
    {
        std::lock_guard lock(job->mutex);
        error.swap(job->result);
    }
    if (!error)
        return 0;
    if (!error->empty()) {
        plans.erase(job->plan->id);
        Finish(job, ResourceFailure(*error));
        return 0;
    }
    if (job->committing)
        Present(job);
    else {
        plans[job->plan->id] = job->plan;
        Finish(job, Prepared(*job->plan));
    }
    return 0;
}
RenameService::Cancel Start(AppState &owner, const std::shared_ptr<Plan> &plan, bool commit,
                            RenameService::Completion completion) {
    const auto job = std::make_shared<Job>();
    job->plan = plan;
    job->complete = std::move(completion);
    job->committing = commit;
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpfnWndProc = Procedure;
    type.lpszClassName = L"PulseRenamePlanPending";
    if (!GetClassInfoW(type.hInstance, type.lpszClassName, &type))
        RegisterClassW(&type);
    job->window =
        CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, nullptr);
    if (!job->window) {
        Finish(job, ResourceFailure("unavailable"));
        return {};
    }
    jobs[job->window] = job;
    if (!SetTimer(job->window, 1, 25, nullptr)) {
        Finish(job, ResourceFailure("unavailable"));
        return {};
    }
    owner.worker.EnqueueIo([job] {
        std::string result;
        try {
            result = ValidateRenameDisk(*job->plan, job->committing, [plan = job->plan] {
                return !plan->cancelled && HostAuthorizationRegistry().ValidateBinding(plan->principal);
            });
        } catch (...) {
            result = "internal_error";
        }
        std::lock_guard lock(job->mutex);
        job->result = std::move(result);
    });
    return [weak = std::weak_ptr<Plan>(plan)] {
        if (auto live = weak.lock())
            live->cancelled = true;
    };
}
void Cleanup() {
    std::erase_if(plans, [](const auto &entry) {
        return entry.second->cancelled || GetTickCount64() >= entry.second->expires ||
               !ValidateCallerBinding(entry.second->principal);
    });
    std::erase_if(tasks, [](const auto &value) {
        if (ValidateCallerBinding(value.second->plan->principal))
            return false;
        value.second->plan->cancelled = true;
        return true;
    });
}
} // namespace
void SetRenameReviewProbe(RenameReviewProbe probe) { review_probe = std::move(probe); }
RenameService::Cancel RenameService::Prepare(AppState &owner, const CallerLease &caller, const json::Value &items,
                                             Completion complete) {
    if (!complete)
        return {};
    const auto principal = Principal(owner, caller);
    if (!principal) {
        complete(ResourceFailure("stale_owner"));
        return {};
    }
    Cleanup();
    size_t count = 0;
    for (const auto &[id, plan] : plans)
        if (SamePrincipal(plan->principal, *principal))
            ++count;
    for (const auto &[window, job] : jobs)
        if (SamePrincipal(job->plan->principal, *principal))
            ++count;
    if (count >= 4 || plans.size() + jobs.size() >= 128) {
        complete(ResourceFailure("plan_budget"));
        return {};
    }
    if (!items.is_array() || !items.size() || items.size() > 64) {
        complete(ResourceFailure("invalid_request"));
        return {};
    }
    const auto plan = std::make_shared<Plan>(*principal);
    plan->owner = owner.hwnd;
    plan->expires = GetTickCount64() + kLifetime;
    plan->id = Nonce();
    auto *tab = ActiveTab(owner);
    if (!tab || plan->id.empty()) {
        complete(ResourceFailure("unavailable"));
        return {};
    }
    plan->tab = tab;
    plan->snapshot = tab->snapshot;
    plan->directory = tab->current_path;
    plan->view_generation = tab->view_generation;
    std::unordered_set<size_t> used;
    for (const auto &item : items.items()) {
        const auto *entry = item.find("entry");
        const auto *name = item.find("newLeafName");
        if (!item.is_object() || item.size() != 2 || !entry || !name || !entry->is_string() || !name->is_string()) {
            complete(ResourceFailure("invalid_request"));
            return {};
        }
        const auto index = ResolveResourceEntry(*principal, entry->as_string(), owner);
        const auto leaf = Wide(name->as_string());
        if (!index || !used.insert(*index).second || !plan->snapshot || *index >= plan->snapshot->size()) {
            complete(ResourceFailure("stale_reference"));
            return {};
        }
        if (!rn::IsValidComponent(leaf)) {
            complete(ResourceFailure("invalid_name"));
            return {};
        }
        const auto &source = (*plan->snapshot)[*index];
        if (!rn::IsValidComponent(source.name)) {
            complete(ResourceFailure("unsupported_context"));
            return {};
        }
        plan->entries.push_back(entry->as_string());
        plan->names.push_back(leaf);
        rn::Source value;
        value.name = source.name;
        value.path = (std::filesystem::path(plan->directory) / source.name).wstring();
        value.folder = std::filesystem::path(plan->directory).filename().wstring();
        plan->sources.push_back(std::move(value));
    }
    if (!Current(*plan)) {
        complete(ResourceFailure("stale_reference"));
        return {};
    }
    return Start(owner, plan, false, std::move(complete));
}
RenameService::Cancel RenameService::Commit(AppState &owner, const CallerLease &caller, std::string plan_id,
                                            Completion complete) {
    if (!complete)
        return {};
    const auto principal = Principal(owner, caller);
    if (!principal) {
        complete(ResourceFailure("stale_owner"));
        return {};
    }
    Cleanup();
    const auto found = plans.find(plan_id);
    if (found == plans.end() || !SamePrincipal(found->second->principal, *principal)) {
        complete(ResourceFailure("stale_plan"));
        return {};
    }
    const auto plan = found->second;
    plans.erase(found); // Every commit attempt consumes its plan, including refusal or cancellation.
    if (!Current(*plan)) {
        complete(ResourceFailure("stale_plan"));
        return {};
    }
    return Start(owner, plan, true, std::move(complete));
}
std::optional<json::Value> RenameService::Task(AppState &owner, const CallerLease &caller, uint64_t task, bool cancel) {
    const auto principal = Principal(owner, caller);
    if (!principal)
        return {};
    const auto at = tasks.find(task);
    if (at == tasks.end() || !SamePrincipal(at->second->plan->principal, *principal))
        return {};
    const auto record = at->second;
    std::lock_guard lock(record->mutex);
    auto data = json::Value::Object();
    data.set("taskId", std::to_string(task));
    if (cancel) {
        const bool accepted = !record->result.has_value();
        if (accepted)
            record->plan->cancelled = true;
        data.set("accepted", accepted);
    } else {
        data.set("cancelRequested", record->plan->cancelled.load());
        data.set("terminal", record->result.has_value());
        data.set("completed", record->result && record->result->complete);
        data.set("transferredBytes", "0");
        data.set("totalBytes", "0");
        auto items = json::Value::Array();
        if (record->result)
            for (const auto &item : record->result->items) {
                auto value = json::Value::Object();
                value.set("source", json::Utf8(item.source));
                value.set("destination", json::Utf8(item.destination));
                value.set("state", static_cast<int>(item.state));
                value.set("error", static_cast<uint64_t>(item.error));
                value.set("cleanupError", static_cast<uint64_t>(item.cleanup_error));
                items.push(std::move(value));
            }
        data.set("items", std::move(items));
    }
    return ResourceSuccess(std::move(data));
}
void RenameService::ReleaseOwner(uintptr_t owner) {
    for (auto &[id, plan] : plans)
        if (plan->principal.Owner().key == owner)
            plan->cancelled = true;
    std::vector<std::shared_ptr<Job>> retired;
    for (auto &[window, job] : jobs)
        if (job->plan->principal.Owner().key == owner)
            retired.push_back(job);
    for (const auto &job : retired) {
        job->plan->cancelled = true;
        job->complete = {};
        const HWND window = std::exchange(job->window, nullptr);
        if (window) {
            jobs.erase(window);
            DestroyWindow(window);
        }
    }
    std::erase_if(tasks, [&](const auto &value) {
        if (value.second->plan->principal.Owner().key != owner)
            return false;
        value.second->plan->cancelled = true;
        return true;
    });
    Cleanup();
}
} // namespace pulse::extensions
