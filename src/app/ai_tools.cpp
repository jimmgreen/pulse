#include "ai_tools.h"
#include "ai_tools_fs.h"
#include "ai_tools_actions.h"
#include "ai_tools_ui.h"
#include "extensions/developer_tools.h"
#include "app_state.h"
#include "../common/localization.h"
#include "../index/index_client.h"
#include "../ops/ops_manager.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>

namespace pulse::app::ai {
namespace {

constexpr uint32_t kAiSearchBit = 0x80000000u;          // UI search ids count up from 1
constexpr uint64_t kAiSessionTag = 0xA1ull << 56;
constexpr uint64_t kAiUndoGroupBit = 1ull << 63;        // UI groups are FILETIMEs, far below
constexpr DWORD kSearchWaitMs = 15000;

struct IndexWaits {
    std::mutex mu;
    std::condition_variable cv;
    std::set<uint32_t> waiting;
    std::set<uint32_t> ready;
};

IndexWaits& Waits() {
    static auto* waits = new IndexWaits;   // process lifetime; the UI thread may still claim late
    return *waits;
}

std::atomic<uint32_t> g_next_search{1};
std::atomic<uint64_t> g_next_session{1};

ToolOutcome Cancelled() { return ToolOutcome::Fail("cancelled", "The call was cancelled."); }

ToolOutcome Search(const AiToolsHost& host, const ToolCall& call) {
    SearchPlan plan;
    ToolOutcome error;
    if (!PlanSearch(call.args, plan, error)) return error;
    if (!host.index || !host.index->Connected())
        return ToolOutcome::Fail("unsupported",
                                 "Pulse's file index is not available: search may be turned off in Pulse, or the "
                                 "index is still starting.", true);
    index::Query query;
    query.session_id = kAiSessionTag | (g_next_session.fetch_add(1) & 0x00FFFFFFFFFFFFFFull);
    query.subscribe = false;
    query.needle = plan.needle;
    query.path_prefix = plan.folder;
    query.folders_only = plan.folders_only;
    query.limit = plan.limit;
    query.rank = true;
    const uint32_t id = kAiSearchBit | (g_next_search.fetch_add(1) & ~kAiSearchBit);
    IndexWaits& waits = Waits();
    {
        std::lock_guard lock(waits.mu);
        waits.waiting.insert(id);
    }
    host.index->SearchAsync(query, id);
    bool answered = false;
    {
        std::unique_lock lock(waits.mu);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSearchWaitMs);
        while (!(answered = waits.ready.count(id) != 0) && !call.cancelled &&
               std::chrono::steady_clock::now() < deadline)
            waits.cv.wait_for(lock, std::chrono::milliseconds(50));
        waits.waiting.erase(id);
        waits.ready.erase(id);
    }
    index::SearchResult result;
    const bool have = answered && host.index->TakeResult(id, result);
    host.index->CancelSession(query.session_id);   // drops anything late and the session's bookkeeping
    if (call.cancelled) return Cancelled();
    if (!have) return ToolOutcome::Fail("timeout", "Pulse's index did not answer in time.", true);
    if (result.error)
        return ToolOutcome::Fail("internal", "The index search failed (error " + std::to_string(result.error) + ").",
                                 true);
    std::vector<FoundItem> items;
    for (const index::Hit& hit : result.hits) {
        if (items.size() >= plan.limit) break;
        items.push_back({hit.path, hit.name, hit.is_dir, hit.size, hit.mtime});
    }
    ToolOutcome out;
    out.data = SearchResultJson(items, result.total);
    return out;
}

uint64_t NewAiUndoGroup() {
    static std::atomic<uint64_t> last{0};
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    uint64_t group = ((static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime) | kAiUndoGroupBit;
    // Two zips within one clock tick still get their own groups.
    uint64_t previous = last.load();
    while (true) {
        const uint64_t next = group > previous ? group : previous + 1;
        if (last.compare_exchange_weak(previous, next)) return next;
    }
}

// What AI clients changed, newest last, for undo_last: a zip (its ops undo
// group) or a run_actions call (what it undoes on the UI thread, and its file
// step's group, which must still be the newest in Pulse's undo list).
struct AiUndoRecord {
    uint64_t ops_group = 0;
    std::shared_ptr<McpActionsRun> run;
    HWND window = nullptr;   // run: the window it changed
};

struct AiUndoList {
    std::mutex mu;
    std::vector<AiUndoRecord> records;
};

AiUndoList& UndoList() {
    static auto* list = new AiUndoList;   // process lifetime, like Waits()
    return *list;
}

void KeepUndo(AiUndoRecord record) {
    AiUndoList& list = UndoList();
    std::lock_guard lock(list.mu);
    list.records.push_back(std::move(record));
    if (list.records.size() > 32) list.records.erase(list.records.begin());
}

std::shared_ptr<const Settings> SettingsNow(const AiToolsHost& host) {
    if (host.settings)
        if (auto settings = host.settings()) return settings;
    return std::make_shared<const Settings>();
}

ToolOutcome CreateZip(const AiToolsHost& host, const ToolCall& call) {
    ZipPlan plan;
    ToolOutcome error;
    if (!PlanZip(call.args, plan, error)) return error;
    ToolOutcome out = RunZip(plan, call.cancelled);
    if (!out.ok) return out;
    // The AI page's line under 打包: the archive it made.
    const size_t slash = plan.zip_path.find_last_of(L"\\/");
    out.summary = L"\n" + (slash == std::wstring::npos ? plan.zip_path : plan.zip_path.substr(slash + 1));
    if (host.ops) {
        const uint64_t group = NewAiUndoGroup();
        std::wstring label = l10n::Get(l10n::StringId::AiUndoCreateZip).c_str();
        if (const size_t at = label.find(L"{client}"); at != std::wstring::npos) label.replace(at, 8, call.client);
        host.ops->RecordExternalCreate(plan.zip_path, group, std::move(label));
        out.undo_group = group;
        KeepUndo({group, nullptr, nullptr});
        out.data.set("undo", "undo_last moves the archive to the Recycle Bin");
    }
    return out;
}

// undo_last's line on the AI page (本次运行): what it undid, or why not.
std::wstring UndoLine(l10n::StringId id) { return L"\n" + std::wstring(l10n::Get(id)); }

ToolOutcome UndoFail(const char* code, const char* message, l10n::StringId why) {
    ToolOutcome out = ToolOutcome::Fail(code, message);
    out.summary = UndoLine(why);
    return out;
}

// What the ops undo stack calls the change, without its 撤销 / Undo: the
// notification and the AI page say 撤销了「新建文件夹 …」 themselves.
std::wstring UndoneName(std::wstring label) {
    const std::wstring cn = l10n::Cn(L"\u64A4\u9500");
    if (label.rfind(cn, 0) == 0) return label.substr(cn.size());
    if (label.rfind(L"Undo ", 0) == 0) return label.substr(5);
    return label;
}

// The newest run_actions call, when it is the newest AI change: its steps
// are undone backwards on the UI thread in the window they changed.
bool UndoRun(const AiToolsHost& host, ToolBroker& broker, const ToolCall& call, ToolOutcome& out) {
    AiUndoRecord record;
    {
        AiUndoList& list = UndoList();
        std::lock_guard lock(list.mu);
        if (list.records.empty() || !list.records.back().run) return false;
        record = list.records.back();
    }
    if (record.ops_group && host.ops &&
        (!host.ops->CanUndo() || host.ops->TopUndoGroup() != record.ops_group)) {
        out = UndoFail("not_ai_operation",
                       "The newest change in Pulse was not made by an AI client; only the user can undo it.",
                       l10n::StringId::AiUndoNotAi);
        return true;
    }
    bool window = false;
    const std::shared_ptr<McpActionsRun> run = record.run;
    const HWND changed = record.window;
    const bool ran = RunOnAiUiThread(host.ui_window, [&](AppState* s) {
        if (!s) return;
        AppState* target = changed ? s->core.windows.Lookup(changed) : s;
        if (!target) return;
        window = true;
        std::vector<std::function<void(AppState&)>> undo;
        {
            std::lock_guard lock(run->mu);
            undo.swap(run->undo);
        }
        for (auto it = undo.rbegin(); it != undo.rend(); ++it) (*it)(*target);
    }, call.cancelled);
    if (call.cancelled) {
        out = Cancelled();
        return true;
    }
    if (!ran) {
        out = ToolOutcome::Fail("busy", "Pulse did not answer in time; try again shortly.", true);
        return true;
    }
    {
        AiUndoList& list = UndoList();
        std::lock_guard lock(list.mu);
        std::erase_if(list.records, [&](const AiUndoRecord& r) { return r.run == run; });
    }
    if (record.ops_group) broker.ForgetAiUndoGroup(record.ops_group);
    if (!window) {
        out = UndoFail("not_ai_operation", "The window that call changed is closed; nothing was undone.",
                       l10n::StringId::AiUndoWindowClosed);
        return true;
    }
    std::wstring done;   // what that call did: 选中 6 项 → 6 项已移到「2026」
    {
        std::lock_guard lock(run->mu);
        const size_t cut = run->outcome.summary.find(L'\n');
        if (cut != std::wstring::npos) done = run->outcome.summary.substr(cut + 1);
    }
    // Only what was done: 「· 你点了「新建」」 / 「· 按「始终允许」」 was about
    // the call itself, not about this undo.
    if (const size_t dot = done.rfind(L" \u00B7 "); dot != std::wstring::npos) {
        const std::wstring tail = done.substr(dot + 3);
        const std::wstring clicked = l10n::Get(l10n::StringId::AiMcpSumClicked).c_str();
        const std::wstring clicked_head = clicked.substr(0, clicked.find(L"{button}"));
        if ((!clicked_head.empty() && tail.rfind(clicked_head, 0) == 0) ||
            tail == l10n::Get(l10n::StringId::AiMcpSumByRule).c_str())
            done.resize(dot);
    }
    out = ToolOutcome{};
    out.summary = done.empty() ? UndoLine(l10n::StringId::AiUndoRunLabel) : L"\n" + done;
    out.changed_files = record.ops_group != 0;
    out.data = json::Value::Object();
    // The notification shows it: Claude 撤销了「刚才的一组操作」.
    out.data.set("undone", json::Utf8(l10n::Get(l10n::StringId::AiUndoRunLabel).c_str()));
    out.data.set("what", "the last run_actions call");
    out.data.set("note", "Each step was put back as it was where that is still possible; a selection stays as it is.");
    return true;
}

ToolOutcome UndoLast(const AiToolsHost& host, ToolBroker& broker, const ToolCall& call) {
    if (ToolOutcome run_out; UndoRun(host, broker, call, run_out)) return run_out;
    if (!host.ops) return ToolOutcome::Fail("unsupported", "Undo is not available.");
    const uint64_t top = host.ops->TopUndoGroup();
    if (!host.ops->CanUndo())
        return UndoFail("not_ai_operation", "There is nothing to undo.", l10n::StringId::AiUndoNothing);
    if (!broker.IsAiUndoGroup(top))
        return UndoFail("not_ai_operation",
                        "The newest change in Pulse was not made by an AI client; only the user can undo it.",
                        l10n::StringId::AiUndoNotAi);
    const std::wstring label = UndoneName(host.ops->UndoLabel());
    switch (host.ops->UndoGroup(top)) {
        case ops::OpsManager::GroupUndo::Started: break;
        case ops::OpsManager::GroupUndo::Pending:
            return ToolOutcome::Fail("busy", "That change is still being carried out; try again shortly.", true);
        case ops::OpsManager::GroupUndo::NotOnTop:
            return UndoFail("not_ai_operation", "Another change became the newest one; nothing was undone.",
                            l10n::StringId::AiUndoNotNewest);
        case ops::OpsManager::GroupUndo::Unavailable:
            return UndoFail("not_ai_operation", "That change can no longer be undone.", l10n::StringId::AiUndoGone);
    }
    broker.ForgetAiUndoGroup(top);
    {
        AiUndoList& list = UndoList();
        std::lock_guard lock(list.mu);
        std::erase_if(list.records, [&](const AiUndoRecord& r) { return r.ops_group == top; });
    }
    ToolOutcome out;
    out.changed_files = true;
    if (!label.empty()) out.summary = L"\n" + label;
    out.data = json::Value::Object();
    out.data.set("undone", json::Utf8(label));
    out.data.set("note", "Pulse carries the undo out in the background, like the user's own undo.");
    return out;
}

} // namespace

bool ClaimAiIndexResult(uint32_t request_id) {
    if (!(request_id & kAiSearchBit)) return false;
    IndexWaits& waits = Waits();
    {
        std::lock_guard lock(waits.mu);
        if (waits.waiting.count(request_id)) waits.ready.insert(request_id);
    }
    waits.cv.notify_all();
    return true;   // a late AI reply too: never the UI's
}

void RegisterAiTools(ToolBroker& broker, const AiToolsHost& host) {
    broker.Register({"pulse.describe_extension_api", PermissionClass::Read,
                     extensions::DescribeExtensionApiTool, 10000, {}});
    broker.Register({"pulse.validate_extension_manifest", PermissionClass::Read,
                     extensions::ValidateExtensionManifestTool, 10000, {}});
    broker.Register({"pulse.get_extension_template", PermissionClass::Read,
                     extensions::GetExtensionTemplateTool, 10000, {}});
    const HWND window = host.ui_window;
    broker.Register({"pulse.get_context", PermissionClass::Read,
                     [window](const ToolCall& call) { return GetContextTool(window, call); }, 10000, {}});
    broker.Register({"pulse.list_folder", PermissionClass::Read,
                     [](const ToolCall& call) { return ListFolder(call.args, call.cancelled); }, 60000, {}});
    broker.Register({"pulse.search", PermissionClass::Read,
                     [host](const ToolCall& call) { return Search(host, call); }, 30000, {}});
    broker.Register({"pulse.get_properties", PermissionClass::Read,
                     [](const ToolCall& call) { return GetProperties(call.args, call.cancelled); }, 60000, {}});
    broker.Register({"pulse.reveal", PermissionClass::View,
                     [window, host](const ToolCall& call) {
                         if (!McpAllows(*SettingsNow(host), "view"))
                             return McpNotAllowed(actions::McpGroup::View, "reveal");
                         return RevealTool(window, call);
                     }, 15000, {}});
    broker.Register({"pulse.create_zip", PermissionClass::Create,
                     [host](const ToolCall& call) { return CreateZip(host, call); }, 10 * 60 * 1000,
                     [](const json::Value& args) { return ZipTargetFolder(args); },
                     [host](const json::Value& args, ToolOutcome& error) {
                         // Before anyone is asked: 新建 must be on (P5).
                         if (!McpAllows(*SettingsNow(host), "create")) {
                             error = McpNotAllowed(actions::McpGroup::Create, "create_zip");
                             return false;
                         }
                         ZipPlan plan;
                         return PlanZip(args, plan, error);
                     }});
    broker.Register({"pulse.undo_last", PermissionClass::Undo,
                     [host, &broker](const ToolCall& call) { return UndoLast(host, broker, call); }, 10000, {}});
    broker.Register({"pulse.find_actions", PermissionClass::Read,
                     [host](const ToolCall& call) { return FindActionsTool(host.ui_window, call, *SettingsNow(host)); },
                     10000, {}});
    // View: run_actions checks and asks for each step itself (ai_mcp_run.h);
    // long enough for a few questions of 2 minutes and a file step.
    broker.Register({"pulse.run_actions", PermissionClass::View,
                     [host](const ToolCall& call) {
                         std::shared_ptr<McpActionsRun> run;
                         ToolOutcome out = RunActionsTool(host.ui_window, call, *SettingsNow(host), host.add_rule, run);
                         if (run) {
                             std::lock_guard lock(run->mu);
                             if (!run->undo.empty()) {
                                 KeepUndo({run->file_group, run, run->window});
                                 out.undo_group = run->file_group;
                             }
                         }
                         return out;
                     }, 20 * 60 * 1000, {}});
}

} // namespace pulse::app::ai
