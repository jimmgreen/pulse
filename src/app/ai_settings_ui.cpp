#include "ai_settings_ui.h"
#include "ai_assistant.h"
#include "about_info.h"
#include "ai_confirm.h"
#include "ai_pack.h"
#include "ai_pack_ui.h"
#include "ai_service.h"
#include "ai_settings.h"
#include "ai_status.h"
#include "app_internal.h"
#include "app_messages.h"
#include "app_navigation.h"
#include "actions/action_inventory.h"
#include "actions/action_policy.h"
#include "actions/action_registry.h"
#include "../common/localization.h"
#include "../ui/settings_page_ids.h"
#include "../ui/ui_renderer.h"
#include <array>
#include <memory>

namespace pulse {
namespace {

namespace ai = app::ai;
using I = l10n::StringId;

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

std::wstring ToolLabel(std::string tool) {
    if (tool.rfind("pulse.", 0) == 0) tool.erase(0, 6);
    if (tool == "get_context" || tool == "get_state") return Text(I::AiToolGetContext);
    if (tool == "list_folder") return Text(I::AiToolListFolder);
    if (tool == "search") return Text(I::AiToolSearch);
    if (tool == "get_properties") return Text(I::AiToolProperties);
    if (tool == "reveal") return Text(I::AiToolReveal);
    if (tool == "create_zip") return Text(I::AiToolCreateZip);
    if (tool == "undo_last") return Text(I::AiToolUndo);
    if (tool == "find_actions") return Text(I::AiToolFindActions);
    if (tool == "run_actions") return Text(I::AiToolRunActions);
    return json::Wide(tool);
}

std::wstring LocalTime(uint64_t unix_ms) {
    ULARGE_INTEGER t;
    t.QuadPart = unix_ms * 10000ULL + 116444736000000000ULL;
    FILETIME utc{t.LowPart, t.HighPart}, local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return {};
    wchar_t buffer[16];
    swprintf_s(buffer, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

void Outcome(const ai::AiCallEntry& e, ui::AiCallRowView& row) {
    using T = ui::AiCallTone;
    if (e.running) {
        row.outcome = Text(e.waiting_user ? I::AiChipWaiting : I::AiOutcomeRunning);
        row.tone = T::Running;
    } else if (e.ok) {
        row.outcome = Text(I::AiOutcomeDone);
        row.tone = T::Done;
    } else if (e.code == "user_denied") {
        row.outcome = Text(I::AiOutcomeDenied);
        row.tone = T::Declined;
    } else if (e.code == "user_timeout") {
        row.outcome = Text(I::AiOutcomeTimeout);
        row.tone = T::Declined;
    } else if (e.code == "not_allowed") {
        row.outcome = Text(I::AiOutcomeBlocked);
        row.tone = T::Declined;
    } else if (e.code == "locked") {
        row.outcome = Text(I::AiMcpGroupLocked);
        row.tone = T::Declined;
    } else if (e.code == "cancelled") {
        row.outcome = Text(I::AiOutcomeCancelled);
        row.tone = T::Declined;
    } else if (e.code == "invalid_args") {
        // Refused before anything ran; the line under it says why.
        row.outcome = Text(I::AiOutcomeNotRun);
        row.tone = T::Declined;
    } else if (e.code == "not_ai_operation") {
        row.outcome = Text(I::AiOutcomeNotUndone);
        row.tone = T::Declined;
    } else if (e.code == "unsupported") {
        row.outcome = Text(I::AiOutcomeCannot);
        row.tone = T::Declined;
    } else if (e.code == "busy") {
        row.outcome = Text(I::AiOutcomeBusy);
        row.tone = T::Failed;
    } else {
        row.outcome = Fill(Text(I::AiOutcomeFailed), L"{code}", json::Wide(e.code));
        row.tone = T::Failed;
    }
}

// ai.json as last read, and what the AI page showed (its rows map clicks back).
struct PageCache {
    bool staged = false;   // StageAiShot: never read ai.json
    uint64_t read_at = 0;
    ai::Settings settings;
    bool broken = false;
    uint64_t pack_checked_at = 0;
    bool can_uninstall = false;
    std::shared_ptr<const ui::AiPageView> view;
};
PageCache& Cache() {
    static PageCache cache;
    return cache;
}

void Reread() { Cache().read_at = 0; }

namespace A = app::actions;

// AI 应用可以做的事: how many actions each group has (the inventory by McpGroupOf).
const std::array<size_t, A::kMcpGroupCount>& GroupCounts() {
    static const std::array<size_t, A::kMcpGroupCount> counts = [] {
        std::array<size_t, A::kMcpGroupCount> c{};
        for (const A::ActionEntry& e : A::kActions) ++c[static_cast<size_t>(A::McpGroupOf(e.id))];
        return c;
    }();
    return counts;
}

void FillGroups(const ai::Settings& settings, ui::AiPageView& view) {
    using G = A::McpGroup;
    using Tag = ui::AiGroupTag;
    const struct {
        G group;
        I title, desc;
        Tag tag;
        const wchar_t* icon;
    } rows[] = {
        {G::View, I::AiMcpGroupView, I::AiMcpDescView, Tag::DefaultOn, L"\xE7B3"},
        {G::Organize, I::AiMcpGroupOrganize, I::AiMcpDescOrganize, Tag::None, L"\xE8EC"},
        {G::Create, I::AiMcpGroupCreate, I::AiMcpDescCreate, Tag::Asks, L"\xE8F4"},
        {G::Files, I::AiMcpGroupFiles, I::AiMcpDescFiles, Tag::Asks, L"\xE8DE"},
        {G::External, I::AiMcpGroupExternal, I::AiMcpDescExternal, Tag::AsksOutside, L"\xE945"},
        {G::Locked, I::AiMcpGroupLocked, I::AiMcpDescLocked, Tag::None, L"\xE72E"},
    };
    for (const auto& r : rows) {
        ui::AiGroupRowView row;
        row.group = static_cast<int>(r.group);
        row.icon = r.icon;
        row.title = Text(r.title);
        const std::wstring n = std::to_wstring(GroupCounts()[static_cast<size_t>(r.group)]);
        row.count = Fill(Text(r.group == G::View ? I::AiMcpCountFirst : I::AiMcpCount), L"{n}", n);
        row.desc = Text(r.desc);
        row.tag = r.tag;
        row.locked = r.group == G::Locked;
        row.on = !row.locked && ai::McpAllows(settings, A::McpGroupKey(r.group));
        view.groups.push_back(std::move(row));
    }
}

// The 新建 notification's target: its undo group and the archive.
struct SnackTarget {
    uint64_t undo_group = 0;
    std::wstring folder, name;
};
SnackTarget& Snack() {
    static SnackTarget target;
    return target;
}

void SplitPath(const std::wstring& path, std::wstring& folder, std::wstring& name) {
    const size_t slash = path.find_last_of(L"\\/");
    folder = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (folder.size() == 2 && folder[1] == L':') folder += L'\\';
}

} // namespace

std::wstring AiMcpConfigText() {
    std::wstring exe(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (n == 0) return {};
        if (n < exe.size()) {
            exe.resize(n);
            break;
        }
        exe.resize(exe.size() * 2);
    }
    std::wstring escaped;
    for (const wchar_t c : exe) {
        if (c == L'\\' || c == L'"') escaped += L'\\';
        escaped += c;
    }
    return L"{\n  \"mcpServers\": {\n    \"pulse\": {\n      \"command\": \"" + escaped +
           L"\",\n      \"args\": [\"--mcp-stdio\"]\n    }\n  }\n}\n";
}

void FillAiStatusVm(ui::WindowViewModel& vm) {
    const ai::AiChip chip = ai::GetAiChip(GetTickCount64());
    switch (chip.state) {
        case ai::AiChipState::Hidden: return;
        case ai::AiChipState::Idle:
            // Out of the way while nobody is connected (随用随调，不用就收起).
            if (!chip.clients) return;
            vm.status.ai_state = ui::StatusAiState::Idle;
            vm.status.ai_text = chip.client;
            return;
        case ai::AiChipState::Busy:
            vm.status.ai_state = ui::StatusAiState::Busy;
            vm.status.ai_text = chip.client + L" \u00B7 " + ToolLabel(chip.tool);
            return;
        case ai::AiChipState::Waiting:
            vm.status.ai_state = ui::StatusAiState::Waiting;
            vm.status.ai_text = Text(I::AiChipWaiting);
            return;
        case ai::AiChipState::Done:
            vm.status.ai_state = ui::StatusAiState::Done;
            vm.status.ai_text = Fill(Text(I::AiChipDone), L"{n}", std::to_wstring(chip.burst_calls));
            return;
    }
}

void FillAiSettingsVm(AppState& s, ui::WindowViewModel& vm) {
    PageCache& cache = Cache();
    const uint64_t now = GetTickCount64();
    if (!cache.staged && (!cache.read_at || now - cache.read_at >= 1000)) {
        cache.settings = ai::ReadAiSettings(&cache.broken);
        cache.read_at = now;
    }
    if (!cache.staged && (!cache.pack_checked_at || now - cache.pack_checked_at >= 2000)) {
        // Hashes Pulse.AI.exe, like the 增强包 page: at most every 2 s.
        cache.can_uninstall = app::QueryAiPackState() == app::AiPackState::Ready;
        cache.pack_checked_at = now;
    }
    const ai::AiStatusSnapshot status = ai::GetAiStatus();
    auto view = std::make_shared<ui::AiPageView>();
    view->enabled = cache.settings.mcp_enabled;
    view->listening = status.listening;
    view->broken = cache.broken;
    view->can_uninstall = cache.can_uninstall;
    if (!view->enabled) {
        view->status = Text(I::AiEnableOffDesc);
    } else if (!status.listening) {
        view->status = Text(I::AiStatusStarting);
    } else if (status.clients.empty()) {
        view->status = Text(I::AiStatusWaiting);
    } else {
        std::wstring names;
        for (const std::wstring& name : status.clients) names += (names.empty() ? L"" : L", ") + name;
        view->status = Fill(Text(I::AiStatusConnected), L"{clients}", names);
    }
    auto connected = [&](const std::wstring& name) {
        for (const std::wstring& c : status.clients)
            if (c == name) return true;
        return false;
    };
    for (const ai::ClientRecord& record : cache.settings.clients) {
        ui::AiClientRowView row;
        row.name = record.name;
        row.connected = connected(record.name);
        switch (record.grant) {
            case ai::ClientGrant::Allowed: row.grant = ui::AiGrantView::Allowed; break;
            case ai::ClientGrant::Denied: row.grant = ui::AiGrantView::Denied; break;
            default:
                row.grant = ai::IsAiSessionClient(record.name) ? ui::AiGrantView::Session : ui::AiGrantView::Ask;
                break;
        }
        view->clients.push_back(std::move(row));
    }
    // Allowed "this time" (not in ai.json) or connected without a record.
    auto listed = [&](const std::wstring& name) {
        for (const auto& row : view->clients)
            if (row.name == name) return true;
        return false;
    };
    for (const std::wstring& name : status.clients) {
        if (listed(name)) continue;
        ui::AiClientRowView row;
        row.name = name;
        row.connected = true;
        row.grant = ai::IsAiSessionClient(name) ? ui::AiGrantView::Session : ui::AiGrantView::Ask;
        view->clients.push_back(std::move(row));
    }
    FillGroups(cache.settings, *view);
    // A run_actions rule names its action (pulse.file.new_folder): show the
    // action's title, as the palette does. Listed only when such a rule exists.
    std::vector<app::actions::ActionView> actions;
    for (const ai::AllowRule& rule : cache.settings.allow) {
        std::wstring label = ToolLabel(rule.tool);
        if (rule.tool.rfind("pulse.", 0) == 0 && app::actions::FindAction(std::string_view(rule.tool).substr(6))) {
            if (actions.empty()) actions = app::actions::ListActions(s);
            for (const app::actions::ActionView& v : actions)
                if (v.key.empty() && v.id == std::string_view(rule.tool).substr(6)) {
                    label = v.title;
                    break;
                }
        }
        view->rules.push_back({rule.client, rule.tool, std::move(label), rule.folder});
    }
    for (const ai::AiCallEntry& e : status.recent) {
        if (view->calls.size() >= ui::kAiPageCalls) break;
        ui::AiCallRowView row;
        row.time = LocalTime(e.started_ms);
        row.client = e.client;
        row.tool_label = ToolLabel(e.tool);
        // "2 步\n选中 6 项 → …": the head follows the tool's name, the rest goes under it.
        const size_t cut = e.summary.find(L'\n');
        const std::wstring head = cut == std::wstring::npos ? std::wstring() : e.summary.substr(0, cut);
        row.summary = cut == std::wstring::npos ? e.summary : e.summary.substr(cut + 1);
        if (!head.empty()) row.tool_label += L" \u00B7 " + head;
        Outcome(e, row);
        view->calls.push_back(std::move(row));
    }
    view->more_calls = status.session_calls > view->calls.size() ? status.session_calls - view->calls.size() : 0;
    cache.view = view;
    vm.settings_ai = std::move(view);
}

void AiSettingsAction(AppState& s, int index) {
    const auto view = Cache().view;
    if (!view) return;
    auto client_at = [&](int base) -> const ui::AiClientRowView* {
        const int row = index - base;
        return row >= 0 && static_cast<size_t>(row) < view->clients.size() ? &view->clients[static_cast<size_t>(row)] : nullptr;
    };
    if (index >= ui::kAiGroupBase && index < ui::kAiGroupBase + ui::kAiRowLimit) {
        // AI 应用可以做的事: the next call reads it (run_actions, find_actions, reveal).
        const int row = index - ui::kAiGroupBase;
        if (!view->broken && row >= 0 && static_cast<size_t>(row) < view->groups.size()) {
            const ui::AiGroupRowView& g = view->groups[static_cast<size_t>(row)];
            if (!g.locked) {
                const char* key = A::McpGroupKey(static_cast<A::McpGroup>(g.group));
                const bool on = !g.on;
                ai::UpdateAiSettings([&](ai::Settings& settings) { ai::SetMcpAllow(settings, key, on); });
            }
        }
    } else if (index == ui::kAiToggle) {
        if (!view->broken) ai::SetAiMcpEnabled(!view->enabled);
    } else if (index == ui::kAiCopyConfig) {
        if (app::CopyTextToClipboard(s.hwnd, AiMcpConfigText()))
            ShowStatusFeedback(s, Text(I::AiConfigCopied));
    } else if (index == ui::kAiOpenLog) {
        const std::wstring folder = ai::ActivityDirectory();
        CreateDirectoryW(folder.c_str(), nullptr);
        NavigateTo(s, folder);
    } else if (index == ui::kAiUninstall) {
        if (ConfirmRemoveAiPack(s)) {
            ai::StopAiServiceAsync();
            OpenSettingsTab(s, 8);
        }
    } else if (index >= ui::kAiRuleRemoveBase) {
        const int row = index - ui::kAiRuleRemoveBase;
        if (row >= 0 && static_cast<size_t>(row) < view->rules.size()) {
            const ui::AiRuleRowView& r = view->rules[static_cast<size_t>(row)];
            ai::RemoveAiAllowRule({r.client, r.tool, r.folder});
        }
    } else if (const auto* c = client_at(ui::kAiClientForgetBase); c && index >= ui::kAiClientForgetBase) {
        ai::ForgetAiClient(c->name);
    } else if (const auto* d = client_at(ui::kAiClientDenyBase); d && index >= ui::kAiClientDenyBase) {
        ai::SetAiClientGrant(d->name, ai::ClientGrant::Denied);
    } else if (const auto* a = client_at(ui::kAiClientAllowBase); a && index >= ui::kAiClientAllowBase) {
        ai::SetAiClientGrant(a->name, ai::ClientGrant::Allowed);
    }
    Reread();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void StageAiShot(int variant) {
    PageCache& cache = Cache();
    cache.staged = true;
    cache.broken = false;
    cache.can_uninstall = true;
    cache.settings = {};
    cache.settings.mcp_enabled = true;
    cache.settings.clients = {{L"Claude", ai::ClientGrant::Allowed},
                              {L"Cursor", ai::ClientGrant::Ask},
                              {L"Old Helper", ai::ClientGrant::Denied}};
    cache.settings.allow = {{L"Claude", "pulse.create_zip", L"D:\\Projects\\Reports"}};
    ai::ResetAiStatusForTest();
    ai::SetAiListening(true);
    auto event = [](ai::BrokerEventKind kind, uint64_t connection, const wchar_t* client, const char* tool = "",
                    int64_t call = 0, bool ok = true, const char* code = "") {
        ai::BrokerEvent e;
        e.kind = kind;
        e.connection = connection;
        e.client = client;
        e.tool = tool;
        e.call = call;
        e.ok = ok;
        e.code = code;
        ai::OnAiBrokerEvent(e);
    };
    using K = ai::BrokerEventKind;
    event(K::Connected, 1, L"Claude");
    const struct { const char* tool; bool ok; const char* code; } done[] = {
        {"pulse.get_context", true, ""},  {"pulse.list_folder", true, ""},       {"pulse.search", true, ""},
        {"pulse.get_properties", false, "access_denied"}, {"pulse.create_zip", false, "user_denied"},
        {"pulse.create_zip", true, ""},   {"pulse.reveal", true, ""},
    };
    int64_t id = 1;
    for (const auto& d : done) {
        event(K::CallStarted, 1, L"Claude", d.tool, id);
        event(K::CallFinished, 1, L"Claude", d.tool, id, d.ok, d.code);
        ++id;
    }
    // P5: 执行 Pulse 动作 with its summary, and one the switches stopped.
    {
        event(K::CallStarted, 1, L"Claude", "pulse.find_actions", id);
        ai::BrokerEvent e;
        e.kind = K::CallFinished;
        e.connection = 1;
        e.client = L"Claude";
        e.tool = "pulse.find_actions";
        e.call = id++;
        e.ok = true;
        e.summary = L"\n\u201C\u79FB\u52A8\u201D \u2192 file.move_to\u3001file.copy_to\u3001tray.send_to";
        ai::OnAiBrokerEvent(e);
        event(K::CallStarted, 1, L"Claude", "pulse.run_actions", id);
        e.tool = "pulse.run_actions";
        e.call = id++;
        e.summary = L"2 \u6B65\n\u9009\u4E2D 6 \u9879 \u2192 6 \u9879\u5DF2\u79FB\u5230\u300C2026\u300D \u00B7 "
                    L"\u4F60\u70B9\u4E86\u300C\u79FB\u52A8\u300D";
        ai::OnAiBrokerEvent(e);
        event(K::CallStarted, 1, L"Claude", "pulse.run_actions", id);
        e.call = id++;
        e.ok = false;
        e.code = "not_allowed";
        e.summary = L"1 \u6B65\n\u52A0\u6807\u7B7E \u00B7 \u6574\u7406 Pulse \u672A\u6253\u5F00";
        ai::OnAiBrokerEvent(e);
    }
    if (variant == 0) {
        event(K::CallStarted, 1, L"Claude", "pulse.search", id);
    } else if (variant == 1) {
        event(K::CallStarted, 1, L"Claude", "pulse.create_zip", id);
        event(K::WaitingUser, 1, L"Claude", "pulse.create_zip", id);
    }
}

void AiChipClicked(AppState& s) {
    if (ai::GetAiChip(GetTickCount64()).state == ai::AiChipState::Waiting && ai::BringAiQuestionToFront()) return;
    AiAssistantToggle(s);
}

void NoteAiChange(AppState& s, const ai::BrokerEvent& event) {
    if (!s.hwnd) return;
    std::string tool = event.tool;
    if (tool.rfind("pulse.", 0) == 0) tool.erase(0, 6);
    if (tool == "create_zip") {
        const json::Value* path = event.data.find("path");
        if (!path || !path->is_string()) return;
        SnackTarget& target = Snack();
        SplitPath(json::Wide(path->as_string()), target.folder, target.name);
        target.undo_group = event.undo_group;
        std::wstring text = Fill(Fill(Text(I::AiSnackCreated), L"{client}", event.client), L"{name}", target.name);
        std::vector<ui::SnackAction> actions;
        if (event.undo_group && s.ops.CanUndo() && s.ops.TopUndoGroup() == event.undo_group)
            actions.push_back({l10n::Get(I::SnackUndo), WM_AI_SNACK_UNDO});
        actions.push_back({Text(I::AiSnackOpen), WM_AI_SNACK_OPEN});
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo, std::move(text), {}, std::move(actions));
    } else if (tool == "run_actions") {
        // The file step's line: 「Claude：6 项已移到「2026」」 with 撤销 and 打开.
        const json::Value* summary = event.data.find("summary");
        if (!summary || !summary->is_string()) return;
        SnackTarget& target = Snack();
        target.undo_group = event.undo_group;
        target.folder.clear();
        target.name.clear();
        if (const json::Value* open = event.data.find("open"); open && open->is_string())
            target.folder = json::Wide(open->as_string());
        std::wstring text = Fill(Fill(Text(I::AiSnackRunDone), L"{client}", event.client), L"{summary}",
                                 json::Wide(summary->as_string()));
        std::vector<ui::SnackAction> actions;
        if (event.undo_group && s.ops.CanUndo() && s.ops.TopUndoGroup() == event.undo_group)
            actions.push_back({l10n::Get(I::SnackUndo), WM_AI_SNACK_UNDO});
        if (!target.folder.empty()) actions.push_back({Text(I::AiSnackOpen), WM_AI_SNACK_OPEN});
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo, std::move(text), {}, std::move(actions));
    } else if (tool == "undo_last") {
        const json::Value* undone = event.data.find("undone");
        const std::wstring label = undone && undone->is_string() ? json::Wide(undone->as_string()) : std::wstring();
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo,
                                       Fill(Fill(Text(I::AiSnackUndone), L"{client}", event.client), L"{name}", label),
                                       {}, {});
    }
}

void HandleAiSnackMessage(AppState& s, UINT message) {
    SnackTarget& target = Snack();
    if (message == WM_AI_SNACK_UNDO) {
        // Only the change the notification named, and only while it is the newest.
        if (target.undo_group && s.ops.CanUndo() && s.ops.TopUndoGroup() == target.undo_group)
            (void)s.ops.UndoGroup(target.undo_group);
    } else if (message == WM_AI_SNACK_OPEN) {
        if (target.folder.empty()) return;
        NavigateTo(s, target.folder);
        if (app::Tab* tab = ActiveTab(s); tab && !target.name.empty()) SelectNameInTab(s, *tab, target.name);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse
