// ai_chat_history.cpp - 55 P3 历史对话: the conversation as a saved file
// (ai_chat_store.h) and back, and the layer that lists them
// (plans/ai/55_P3_实现计划.md).
//
// What is saved is what the panel shows of the current versions: messages,
// answers, closed thinking, the rating; a card becomes a quiet note with its
// title; stopped / cut rows and error buttons are not kept. The model history
// goes with it, so a reopened conversation continues where it was.
#include "ai_assistant_chat.h"

#include "ai_assistant.h"
#include "ai_chat_store.h"
#include "ai_settings.h"
#include "app_commands.h"
#include "app_internal.h"
#include "app_navigation.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../ui/settings_page_ids.h"
#include "../ui/ui_renderer.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <functional>
#include <utility>

namespace pulse {

namespace ai = app::ai;
using I = l10n::StringId;
using ui::AiBlockKind;
using ui::AiBlockView;

// The layer's list: every saved conversation (newest first), the filtered
// rows in their groups, and the row being renamed or deleted (by id, so a
// refilter keeps them).
struct AiHistoryUi {
    std::vector<ai::ChatEntry> all;
    std::wstring query;
    std::vector<size_t> rows;           // into `all`, in the order shown
    std::vector<std::wstring> groups;   // per row: its heading ("" same group)
    std::wstring rename_id, rename_text;
    std::wstring delete_id;
};

namespace {

std::wstring Text(I id) { return l10n::Get(id).c_str(); }
std::string U(const std::wstring& w) { return json::Utf8(w); }
std::wstring W(const json::Value& o, std::string_view key) { return json::Wide(o.get_string(key)); }

AiHistoryUi& Ui(AppState& s) {
    if (!s.aiAssistant.hist) s.aiAssistant.hist = std::make_shared<AiHistoryUi>();
    return *s.aiAssistant.hist;
}

// ---- The conversation <-> json ------------------------------------------

json::Value BlockJson(const AiChat& c, const AiBlockView& b) {
    json::Value o = json::Value::Object();
    const auto note = [&](const std::wstring& text) {
        if (text.empty()) return json::Value();
        o.set("k", "note");
        o.set("text", U(text));
        return o;
    };
    switch (b.kind) {
    case AiBlockKind::User:
        o.set("k", "user");
        o.set("text", U(b.text));
        if (!b.context.empty()) o.set("ctx", U(b.context));
        if (b.edited) o.set("edited", true);
        if (const auto it = c.turn_hist.find(b.id); it != c.turn_hist.end())
            o.set("h", static_cast<int64_t>(it->second));
        return o;
    case AiBlockKind::Thinking:
        if (b.text.empty() && b.running) return {};
        o.set("k", "think");
        o.set("label", U(b.label));
        if (!b.meta.empty()) o.set("meta", U(b.meta));
        if (!b.text.empty()) o.set("text", U(b.text));
        return o;
    case AiBlockKind::Text:
        if (b.text.empty()) return {};
        o.set("k", "text");
        o.set("text", U(b.text));
        return o;
    case AiBlockKind::Acts:
        o.set("k", "acts");
        if (!b.meta.empty()) o.set("meta", U(b.meta));
        if (b.rating) o.set("rating", b.rating);
        return o;
    case AiBlockKind::Note:
        return note(b.text);
    case AiBlockKind::Error:
        return note(b.title.empty() ? b.sub : b.title);
    case AiBlockKind::Steps:
    case AiBlockKind::Plan:
    case AiBlockKind::Confirm:
    case AiBlockKind::Result:
    case AiBlockKind::Query:
    case AiBlockKind::Rename:
    case AiBlockKind::Task:
    case AiBlockKind::Disk:
    case AiBlockKind::Clean:
    case AiBlockKind::Funnel:
    case AiBlockKind::Dup:
        return note(b.title.empty() ? b.label : b.title);
    case AiBlockKind::Working:
    case AiBlockKind::Cut:
    case AiBlockKind::Follow:
    case AiBlockKind::Stopped:
        return {};
    }
    return {};
}

// The blocks and the first message (the title), or false with no message.
bool BlocksJson(const AiChat& c, json::Value& blocks, std::wstring& first) {
    blocks = json::Value::Array();
    first.clear();
    for (const AiBlockView& b : c.blocks) {
        json::Value o = BlockJson(c, b);
        if (o.is_null()) continue;
        if (b.kind == AiBlockKind::User && first.empty()) first = b.text;
        blocks.push(std::move(o));
    }
    return !first.empty();
}

void RestoreBlock(AiChat& c, const json::Value& o) {
    if (!o.is_object()) return;
    const std::string k = o.get_string("k");
    if (k == "user") {
        AiBlockView& b = c.Add(AiBlockKind::User, 0);
        b.text = W(o, "text");
        b.context = W(o, "ctx");
        b.edited = o.get_bool("edited");
        int64_t h = 0;
        if (const json::Value* v = o.find("h"); v && v->get_int(h, 0, int64_t{1} << 40))
            c.turn_hist[b.id] = static_cast<size_t>(h);
    } else if (k == "think") {
        AiBlockView& b = c.Add(AiBlockKind::Thinking, 0);
        b.label = W(o, "label");
        b.meta = W(o, "meta");
        b.text = W(o, "text");
        b.running = false;
        b.open = false;
    } else if (k == "text") {
        AiBlockView& b = c.Add(AiBlockKind::Text, 0);
        b.text = W(o, "text");
    } else if (k == "acts") {
        AiBlockView& b = c.Add(AiBlockKind::Acts, 0);
        b.meta = W(o, "meta");
        int64_t r = 0;
        if (const json::Value* v = o.find("rating"); v && v->get_int(r, -1, 1)) b.rating = static_cast<int>(r);
    } else if (k == "note") {
        AiBlockView& b = c.Add(AiBlockKind::Note, 0);
        b.text = W(o, "text");
    }
}

// ---- Times and groups ---------------------------------------------------

bool LocalTime(int64_t unix_ms, SYSTEMTIME& out) {
    if (unix_ms <= 0) return false;
    const uint64_t ticks = (static_cast<uint64_t>(unix_ms) + 11644473600000ull) * 10000ull;
    FILETIME ft{static_cast<DWORD>(ticks & 0xffffffffu), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utc{};
    return FileTimeToSystemTime(&ft, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &out);
}

// Days since 1601 of a local calendar date.
int64_t DayNumber(const SYSTEMTIME& t) {
    SYSTEMTIME d{};
    d.wYear = t.wYear;
    d.wMonth = t.wMonth;
    d.wDay = t.wDay;
    FILETIME ft{};
    if (!SystemTimeToFileTime(&d, &ft)) return 0;
    const uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<int64_t>(ticks / 864000000000ull);
}

// 0 today, 1 yesterday, 2 within 7 days, 3 earlier.
int AgeGroup(int64_t unix_ms, const SYSTEMTIME& now, int64_t today) {
    SYSTEMTIME t{};
    if (!LocalTime(unix_ms, t)) return 3;
    (void)now;
    const int64_t days = today - DayNumber(t);
    if (days <= 0) return 0;
    if (days == 1) return 1;
    if (days < 7) return 2;
    return 3;
}

// Today HH:mm; this week 周X; else M/D (yyyy/M/D in another year).
std::wstring WhenLabel(int64_t unix_ms, const SYSTEMTIME& now, int64_t today) {
    SYSTEMTIME t{};
    if (!LocalTime(unix_ms, t)) return {};
    const int64_t days = today - DayNumber(t);
    wchar_t buf[32]{};
    if (days <= 0) {
        swprintf_s(buf, L"%02u:%02u", t.wHour, t.wMinute);
        return buf;
    }
    if (days < 7) {
        static constexpr I kWeek[] = {I::AiWeekSun, I::AiWeekMon, I::AiWeekTue, I::AiWeekWed,
                                      I::AiWeekThu, I::AiWeekFri, I::AiWeekSat};
        return Text(kWeek[t.wDayOfWeek % 7]);
    }
    if (t.wYear != now.wYear) swprintf_s(buf, L"%u/%u/%u", t.wYear, t.wMonth, t.wDay);
    else swprintf_s(buf, L"%u/%u", t.wMonth, t.wDay);
    return buf;
}

void Refilter(AiHistoryUi& h) {
    h.rows.clear();
    h.groups.clear();
    const std::wstring key = ai::SearchKey(h.query);
    std::vector<size_t> pinned, rest;
    for (size_t i = 0; i < h.all.size(); ++i) {
        if (!key.empty() && h.all[i].haystack.find(key) == std::wstring::npos) continue;
        (h.all[i].pinned ? pinned : rest).push_back(i);
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const int64_t today = DayNumber(now);
    static constexpr I kGroups[] = {I::DateToday, I::DateYesterday, I::AiHistWeek, I::AiHistEarlier};
    for (size_t i = 0; i < pinned.size(); ++i) {
        h.rows.push_back(pinned[i]);
        h.groups.push_back(i == 0 ? Text(I::AiHistPinned) : std::wstring());
    }
    int last = -1;
    for (size_t i : rest) {
        const int g = AgeGroup(h.all[i].updated, now, today);
        h.rows.push_back(i);
        h.groups.push_back(g != last ? Text(kGroups[g]) : std::wstring());
        last = g;
    }
}

void Reload(AppState& s) {
    AiHistoryUi& h = Ui(s);
    const std::wstring dir = ai::ChatsDirectory();
    h.all = dir.empty() ? std::vector<ai::ChatEntry>{} : ai::ListChats(dir);
    Refilter(h);
}

const ai::ChatEntry* RowEntry(AppState& s, int row) {
    if (!s.aiAssistant.hist || row < 0) return nullptr;
    AiHistoryUi& h = *s.aiAssistant.hist;
    if (row >= static_cast<int>(h.rows.size())) return nullptr;
    return &h.all[h.rows[static_cast<size_t>(row)]];
}

ai::ChatEntry* FindEntry(AiHistoryUi& h, const std::wstring& id) {
    for (ai::ChatEntry& e : h.all)
        if (e.id == id) return &e;
    return nullptr;
}

int RowOf(const AiHistoryUi& h, const std::wstring& id) {
    if (id.empty()) return -1;
    for (size_t i = 0; i < h.rows.size(); ++i)
        if (h.all[h.rows[i]].id == id) return static_cast<int>(i);
    return -1;
}

void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

}  // namespace

// ---- Saving and opening -------------------------------------------------

void AiHistorySave(AppState& s) {
    if (!s.aiAssistant.chat) return;
    AiChat& c = *s.aiAssistant.chat;
    json::Value blocks;
    std::wstring first;
    if (!BlocksJson(c, blocks, first)) return;
    const ai::Settings settings = ai::LoadSettings();
    if (!settings.assistant.history) return;
    const std::wstring dir = ai::ChatsDirectory();
    if (dir.empty()) return;

    if (!c.renamed) {
        c.title = ai::PlainLine(first, 40);
        if (c.title.empty()) c.title = Text(I::AiHistUntitled);
    }
    json::Value doc = json::Value::Object();
    doc.set("v", 1);
    doc.set("title", U(c.title));
    if (c.renamed) doc.set("renamed", true);
    if (c.pinned) doc.set("pinned", true);
    doc.set("model", U(AiChatModelId(s)));
    doc.set("blocks", std::move(blocks));
    doc.set("history", AiAgentHistoryJson(s));
    // Unchanged since the last save (opened and closed again): keep its time
    // and place in the list.
    const size_t sig = std::hash<std::string>{}(json::Serialize(doc));
    if (!c.history_id.empty() && sig == c.saved_sig) return;

    const int64_t now = ai::NowUnixMs();
    if (c.history_id.empty()) {
        const uint32_t salt = static_cast<uint32_t>(GetTickCount64()) ^ (GetCurrentProcessId() << 16);
        c.history_id = ai::NewChatId(now, salt);
        c.created = now;
    }
    doc.set("id", U(c.history_id));
    doc.set("created", c.created);
    doc.set("updated", now);
    if (!ai::SaveChat(dir, c.history_id, doc)) return;
    c.saved_sig = sig;

    // The open list follows.
    if (s.aiAssistant.hist) {
        AiHistoryUi& h = *s.aiAssistant.hist;
        ai::ChatEntry e = ai::EntryOf(c.history_id, doc);
        if (ai::ChatEntry* old = FindEntry(h, c.history_id)) *old = std::move(e);
        else h.all.insert(h.all.begin(), std::move(e));
        std::stable_sort(h.all.begin(), h.all.end(),
                         [](const ai::ChatEntry& a, const ai::ChatEntry& b) { return a.updated > b.updated; });
        Refilter(h);
        Redraw(s);
    }
}

bool AiHistoryOpenChat(AppState& s, const std::wstring& id) {
    const std::wstring dir = ai::ChatsDirectory();
    json::Value doc;
    if (dir.empty() || !ai::LoadChat(dir, id, doc)) return false;
    if (s.aiAssistant.chat && s.aiAssistant.chat->history_id == id) return true;
    AiChatReset(s);   // stops and saves the one in the panel
    AiChat& c = Chat(s);
    c.history_id = id;
    c.title = W(doc, "title");
    c.renamed = doc.get_bool("renamed");
    c.pinned = doc.get_bool("pinned");
    int64_t created = 0;
    if (const json::Value* v = doc.find("created")) v->get_int(created);
    c.created = created;
    if (const json::Value* blocks = doc.find("blocks"); blocks && blocks->is_array())
        for (const json::Value& b : blocks->items()) RestoreBlock(c, b);
    if (const json::Value* history = doc.find("history"); history && history->is_array() && history->size() > 0)
        AiAgentSetHistory(s, *history);
    // As saved: the next save only happens once something changes.
    json::Value blocks;
    std::wstring first;
    BlocksJson(c, blocks, first);
    json::Value again = json::Value::Object();
    again.set("v", 1);
    again.set("title", U(c.title));
    if (c.renamed) again.set("renamed", true);
    if (c.pinned) again.set("pinned", true);
    again.set("model", U(AiChatModelId(s)));
    again.set("blocks", std::move(blocks));
    again.set("history", AiAgentHistoryJson(s));
    c.saved_sig = std::hash<std::string>{}(json::Serialize(again));
    s.renderer.AiPanel().ScrollToEnd();
    Redraw(s);
    return true;
}

// ---- The layer ----------------------------------------------------------

void AiHistoryShow(AppState& s, bool open) {
    AiAssistantState& a = s.aiAssistant;
    if (open == a.hist_open) return;
    a.hist_open = open;
    AiHistoryUi& h = Ui(s);
    h.rename_id.clear();
    h.delete_id.clear();
    if (open) {
        // Retention runs when the list is looked at (and at start-up).
        const ai::Settings settings = ai::LoadSettings();
        const std::wstring dir = ai::ChatsDirectory();
        if (!dir.empty() && settings.assistant.history_days > 0)
            ai::PurgeChats(dir, settings.assistant.history_days, ai::NowUnixMs());
        h.query.clear();
        Reload(s);
        s.renderer.AiPanel().HistoryScrollTop();
    }
    Redraw(s);
}

void AiHistoryFillVm(AppState& s, ui::AiHistoryView& v) {
    const AiAssistantState& a = s.aiAssistant;
    v.open = a.hist_open;
    if (!a.hist_open && s.renderer.AiPanel().HistoryAmount() <= 0.001f) return;
    AiHistoryUi& h = Ui(s);
    v.search_cue = Text(I::AiHistSearch);
    v.query = h.query;
    v.foot = Text(I::AiHistFoot);
    v.settings = Text(I::AiHistSettings);
    v.rename_hint = Text(I::AiHistRenameHint);
    v.delete_note = Text(I::AiHistDeleteNote);
    v.cancel = Text(I::Cancel);
    v.del = Text(I::Delete);
    if (h.rows.empty()) {
        if (!h.query.empty()) v.empty = Text(I::AiHistNoMatch);
        else if (!ai::LoadSettings().assistant.history) v.empty = Text(I::AiHistOff);
        else v.empty = Text(I::AiHistEmpty);
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const int64_t today = DayNumber(now);
    const std::wstring current = a.chat ? a.chat->history_id : std::wstring();
    v.rows.reserve(h.rows.size());
    for (size_t i = 0; i < h.rows.size(); ++i) {
        const ai::ChatEntry& e = h.all[h.rows[i]];
        ui::AiHistoryRowView r;
        r.group = h.groups[i];
        r.title = e.title.empty() ? Text(I::AiHistUntitled) : e.title;
        r.snippet = e.snippet;
        r.when = WhenLabel(e.updated, now, today);
        r.pinned = e.pinned;
        r.current = !current.empty() && e.id == current;
        v.rows.push_back(std::move(r));
    }
    v.renaming = RowOf(h, h.rename_id);
    v.rename_text = h.rename_text;
    v.deleting = RowOf(h, h.delete_id);
}

bool AiHistoryActivate(AppState& s, ui::AiPanelPart part, int row) {
    using P = ui::AiPanelPart;
    AiHistoryUi& h = Ui(s);
    const ai::ChatEntry* e = RowEntry(s, row);
    const std::wstring dir = ai::ChatsDirectory();
    switch (part) {
    case P::HistRow:
        if (!e) return false;
        if (!h.rename_id.empty() || !h.delete_id.empty()) {
            // A click elsewhere ends the rename / confirmation first.
            const bool same = e->id == h.rename_id || e->id == h.delete_id;
            h.rename_id.clear();
            h.delete_id.clear();
            Redraw(s);
            if (same) return false;
        }
        // A copy: opening saves the one in the panel, which reorders `all`.
        if (const std::wstring id = e->id; AiHistoryOpenChat(s, id)) AiHistoryShow(s, false);
        return false;
    case P::HistPin: {
        if (!e || dir.empty()) return false;
        const bool pinned = !e->pinned;
        const std::wstring id = e->id;
        if (!ai::UpdateChatMeta(dir, id, nullptr, &pinned)) return false;
        if (ai::ChatEntry* m = FindEntry(h, id)) m->pinned = pinned;
        if (s.aiAssistant.chat && s.aiAssistant.chat->history_id == id) s.aiAssistant.chat->pinned = pinned;
        Refilter(h);
        Redraw(s);
        return false;
    }
    case P::HistRename:
        if (!e) return false;
        h.delete_id.clear();
        h.rename_id = e->id;
        h.rename_text = e->title;
        s.renderer.AiPanel().HistoryRevealRow(row);
        Redraw(s);
        return true;
    case P::HistDelete:
        if (!e) return false;
        h.rename_id.clear();
        h.delete_id = e->id;
        Redraw(s);
        return false;
    case P::HistDelCancel:
        h.delete_id.clear();
        Redraw(s);
        return false;
    case P::HistDelOk: {
        if (!e || e->id != h.delete_id || dir.empty()) return false;
        const std::wstring id = e->id;
        h.delete_id.clear();
        if (!ai::DeleteChat(dir, id)) return false;
        h.all.erase(std::remove_if(h.all.begin(), h.all.end(), [&](const ai::ChatEntry& x) { return x.id == id; }),
                    h.all.end());
        // The panel's conversation stays; saved again it becomes a new file.
        if (s.aiAssistant.chat && s.aiAssistant.chat->history_id == id) {
            s.aiAssistant.chat->history_id.clear();
            s.aiAssistant.chat->saved_sig = 0;
            s.aiAssistant.chat->pinned = false;
        }
        Refilter(h);
        Redraw(s);
        return false;
    }
    case P::HistSettings:
        AiHistoryShow(s, false);
        OpenSettingsTab(s, ui::kSettingsPageAssistant);
        return false;
    default:
        // The layer's empty space ends a rename or confirmation.
        if (!h.rename_id.empty() || !h.delete_id.empty()) {
            h.rename_id.clear();
            h.delete_id.clear();
            Redraw(s);
        }
        return false;
    }
}

void AiHistorySetQuery(AppState& s, std::wstring query) {
    AiHistoryUi& h = Ui(s);
    if (query == h.query) return;
    h.query = std::move(query);
    h.delete_id.clear();
    Refilter(h);
    s.renderer.AiPanel().HistoryScrollTop();
    Redraw(s);
}

void AiHistoryRenameText(AppState& s, std::wstring text) { Ui(s).rename_text = std::move(text); }

const std::wstring& AiHistoryRenameTitle(AppState& s) { return Ui(s).rename_text; }

bool AiHistoryRenaming(const AppState& s) { return s.aiAssistant.hist && !s.aiAssistant.hist->rename_id.empty(); }

void AiHistoryRenameEnd(AppState& s, bool save) {
    AiHistoryUi& h = Ui(s);
    if (h.rename_id.empty()) return;
    const std::wstring id = std::exchange(h.rename_id, std::wstring());
    std::wstring title = ai::PlainLine(h.rename_text, 80);
    const std::wstring dir = ai::ChatsDirectory();
    if (save && !title.empty() && !dir.empty() && ai::UpdateChatMeta(dir, id, &title, nullptr)) {
        if (ai::ChatEntry* m = FindEntry(h, id)) {
            m->title = title;
            m->haystack = ai::SearchKey(title) + m->haystack.substr((std::min)(m->haystack.find(L'\n'), m->haystack.size()));
        }
        if (s.aiAssistant.chat && s.aiAssistant.chat->history_id == id) {
            s.aiAssistant.chat->title = title;
            s.aiAssistant.chat->renamed = true;
        }
        Refilter(h);
    }
    Redraw(s);
}

bool AiHistoryOpenFirst(AppState& s) {
    AiHistoryUi& h = Ui(s);
    if (h.query.empty() || h.rows.empty()) return false;
    const std::wstring id = h.all[h.rows.front()].id;   // a copy (see HistRow)
    if (AiHistoryOpenChat(s, id)) AiHistoryShow(s, false);
    return true;
}

bool AiHistoryEscape(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.hist_open) return false;
    AiHistoryUi& h = Ui(s);
    if (!h.rename_id.empty()) {
        AiHistoryRenameEnd(s, false);
        return true;
    }
    if (!h.delete_id.empty()) {
        h.delete_id.clear();
        Redraw(s);
        return true;
    }
    AiHistoryShow(s, false);
    return true;
}

bool AiHistoryRowPinned(AppState& s, int row) {
    const ai::ChatEntry* e = RowEntry(s, row);
    return e && e->pinned;
}

}  // namespace pulse
