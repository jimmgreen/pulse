// ai_assistant_chat.cpp - Pulse 助手's conversation and the A2 demo driver.
//
// The demo follows the design's script (scenes 2-6) with the open folder's real entries: think, look at the window,
// read the folder, propose a plan, ask in the conversation, "sort", report.
// It never touches a file; a note at the top of the conversation says so.
// A3 swaps this driver for the DeepSeek agent loop, keeping the blocks.
#include "ai_assistant_chat.h"

#include "ai_assistant.h"
#include "app_internal.h"
#include "../common/localization.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"

#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <iterator>

namespace pulse {
namespace {

using I = l10n::StringId;
using ui::AiBlockKind;
using ui::AiBlockView;

constexpr UINT_PTR kRunTimer = 0x4152;   // 'AR'
constexpr UINT_PTR kPeekTimer = 0x4156;  // the peek bubble's in and out
constexpr uint64_t kPeekMs = 4200;

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

std::wstring Num(size_t n) { return std::to_wstring(n); }

void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

uint64_t Now() { return ui::motion::NowMs(); }

std::wstring FormatSize(uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    wchar_t buf[32];
    if (u == 0 || v >= 100.0) swprintf_s(buf, L"%.0f %s", v, units[u]);
    else swprintf_s(buf, L"%.1f %s", v, units[u]);
    return buf;
}

// The design's five groups by extension (plus everything else).
enum class Cat : uint8_t { Image, Document, App, Video, Archive, Other, Count };

Cat CatOf(const std::wstring& name) {
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos) return Cat::Other;
    std::wstring ext = name.substr(dot + 1);
    for (wchar_t& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    static const struct { const wchar_t* ext; Cat cat; } kMap[] = {
        {L"jpg", Cat::Image}, {L"jpeg", Cat::Image}, {L"png", Cat::Image}, {L"gif", Cat::Image},
        {L"webp", Cat::Image}, {L"bmp", Cat::Image}, {L"heic", Cat::Image}, {L"heif", Cat::Image},
        {L"tif", Cat::Image}, {L"tiff", Cat::Image}, {L"svg", Cat::Image}, {L"avif", Cat::Image},
        {L"jxl", Cat::Image}, {L"raw", Cat::Image}, {L"cr2", Cat::Image}, {L"nef", Cat::Image},
        {L"arw", Cat::Image}, {L"dng", Cat::Image}, {L"ico", Cat::Image}, {L"psd", Cat::Image},
        {L"pdf", Cat::Document}, {L"doc", Cat::Document}, {L"docx", Cat::Document}, {L"xls", Cat::Document},
        {L"xlsx", Cat::Document}, {L"ppt", Cat::Document}, {L"pptx", Cat::Document}, {L"txt", Cat::Document},
        {L"md", Cat::Document}, {L"csv", Cat::Document}, {L"rtf", Cat::Document}, {L"odt", Cat::Document},
        {L"ods", Cat::Document}, {L"odp", Cat::Document}, {L"epub", Cat::Document}, {L"wps", Cat::Document},
        {L"exe", Cat::App}, {L"msi", Cat::App}, {L"msix", Cat::App}, {L"msixbundle", Cat::App},
        {L"appx", Cat::App}, {L"appxbundle", Cat::App},
        {L"mp4", Cat::Video}, {L"mkv", Cat::Video}, {L"mov", Cat::Video}, {L"avi", Cat::Video},
        {L"wmv", Cat::Video}, {L"webm", Cat::Video}, {L"flv", Cat::Video}, {L"m4v", Cat::Video},
        {L"ts", Cat::Video}, {L"3gp", Cat::Video},
        {L"zip", Cat::Archive}, {L"rar", Cat::Archive}, {L"7z", Cat::Archive}, {L"tar", Cat::Archive},
        {L"gz", Cat::Archive}, {L"bz2", Cat::Archive}, {L"xz", Cat::Archive}, {L"zst", Cat::Archive},
        {L"iso", Cat::Archive}, {L"cab", Cat::Archive},
    };
    for (const auto& m : kMap)
        if (ext == m.ext) return m.cat;
    return Cat::Other;
}

struct CatInfo { I name; ui::AiIcon icon; ui::AiTint tint; };
// v2.js CATICON: image, doc, down, video, dup.
constexpr CatInfo kCats[] = {
    {I::AiAsstCatImage, ui::AiIcon::Image, ui::AiTint::Image},
    {I::AiAsstCatDocument, ui::AiIcon::Document, ui::AiTint::Document},
    {I::AiAsstCatApp, ui::AiIcon::Download, ui::AiTint::App},
    {I::AiAsstCatVideo, ui::AiIcon::Video, ui::AiTint::Video},
    {I::AiAsstCatArchive, ui::AiIcon::Duplicate, ui::AiTint::Archive},
    {I::AiAsstCatOther, ui::AiIcon::File, ui::AiTint::Neutral},
};

// What the demo read from the folder.
struct FolderScan {
    bool ok = false;
    std::wstring path, name;
    size_t items = 0, dirs = 0;
    uint64_t bytes = 0;
    struct File { std::wstring name; uint64_t size; Cat cat; };
    std::vector<File> files;
    struct Group { Cat cat; std::vector<size_t> files; bool exists; };
    std::vector<Group> groups;
};

FolderScan Scan(AppState& s) {
    FolderScan f;
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || tab->current_path.rfind(L"pulse:", 0) == 0) return f;
    f.path = tab->current_path;
    f.name = app::TabTitle(f.path);
    if (f.name.empty()) return f;
    const size_t n = (std::min)(tab->EntryCount(), static_cast<size_t>(2000));
    f.items = tab->EntryCount();
    for (size_t i = 0; i < n; ++i) {
        const fs::DirEntry e = tab->EntryAt(i);
        if (e.is_dir) { ++f.dirs; continue; }
        f.files.push_back({e.name, e.size, CatOf(e.name)});
        f.bytes += e.size;
    }
    for (int c = 0; c < static_cast<int>(Cat::Count); ++c) {
        FolderScan::Group g{static_cast<Cat>(c), {}, false};
        for (size_t i = 0; i < f.files.size(); ++i)
            if (f.files[i].cat == g.cat) g.files.push_back(i);
        if (g.files.empty()) continue;
        std::wstring dir = f.path;
        if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
        dir += Text(kCats[c].name);
        const DWORD attrs = GetFileAttributesW(dir.c_str());
        g.exists = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
        f.groups.push_back(std::move(g));
    }
    f.ok = true;
    return f;
}

// Plain text of an answer (no ** markers) for the clipboard.
std::wstring Plain(const std::wstring& text) {
    std::wstring out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'*' && i + 1 < text.size() && text[i + 1] == L'*') { ++i; continue; }
        out.push_back(text[i]);
    }
    return out;
}

void CopyText(HWND hwnd, const std::wstring& raw) {
    if (raw.empty() || !OpenClipboard(hwnd)) return;
    EmptyClipboard();
    // CF_UNICODETEXT lines end in CRLF (older apps paste a bare LF as one line).
    std::wstring text;
    text.reserve(raw.size() + raw.size() / 16);
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\n' && (i == 0 || raw[i - 1] != L'\r')) text += L'\r';
        text += raw[i];
    }
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

void Schedule(AppState& s);

void RunDue(AppState* s) {
    AiChat& c = *s->aiAssistant.chat;
    bool changed = false;
    // Run every step that is due (a slow frame may owe several).
    for (int guard = 0; guard < 16 && !c.queue.empty() && Now() >= c.next_at; ++guard) {
        AiChat::Step& step = c.queue.front();
        const int again = step.run ? step.run(*s) : -1;
        changed = true;
        if (again >= 0) {
            c.next_at = Now() + static_cast<uint64_t>(again);
            continue;
        }
        if (!c.queue.empty()) c.queue.pop_front();
        if (!c.queue.empty()) c.next_at = Now() + c.queue.front().delay;
    }
    if (changed) Redraw(*s);
    Schedule(*s);
}

void CALLBACK RunTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!s || !s->aiAssistant.chat) return;
    RunDue(s);
}

void Schedule(AppState& s) {
    AiChat& c = Chat(s);
    if (!s.hwnd || c.queue.empty()) return;
    const uint64_t now = Now();
    const UINT wait = c.next_at > now ? static_cast<UINT>((std::min)(c.next_at - now, static_cast<uint64_t>(60000))) : 1;
    SetTimer(s.hwnd, kRunTimer, (std::max)(wait, 1u), RunTimer);
}

void Push(AiChat& c, uint32_t delay, std::function<int(AppState&)> run) {
    const bool start = c.queue.empty();
    c.queue.push_back({delay, std::move(run)});
    if (start) c.next_at = Now() + delay;
}

void Push(AiChat& c, uint32_t delay, std::function<void(AppState&)> run) {
    Push(c, delay, std::function<int(AppState&)>([run = std::move(run)](AppState& s) {
             run(s);
             return -1;
         }));
}

void SetChip(AiChat& c, ui::StatusAiState state, std::wstring text, float progress = -1.0f) {
    c.chip = state;
    c.chip_text = std::move(text);
    c.chip_progress = progress;
}

std::wstring EffortName(const AppState& s) { return AiAssistantThinkName(s); }

std::wstring ModelName(const AppState& s) { return AiAssistantModelName(s); }

// 55 P2: each user block's versions on its turn's acts row (‹ 2 / 3 ›), a
// row of its own for a version that has none (it failed), 已编辑 on the
// bubble; 继续生成 only on the last turn.
void MarkVersions(AiChat& c) {
    for (size_t i = 0; i < c.blocks.size(); ++i) {
        if (c.blocks[i].kind != AiBlockKind::User) continue;
        const uint32_t uid = c.blocks[i].id;
        size_t end = i + 1;
        while (end < c.blocks.size() && c.blocks[end].kind != AiBlockKind::User) ++end;
        const bool last_turn = end == c.blocks.size();
        int count = 1, cur = 1;
        bool edited = false;
        if (auto it = c.branches.find(uid); it != c.branches.end() && !it->second.versions.empty()) {
            count = static_cast<int>(it->second.versions.size());
            cur = static_cast<int>(it->second.current) + 1;
            edited = c.blocks[i].text != it->second.versions.front().text;
        }
        c.blocks[i].edited = edited;
        size_t acts = c.blocks.size();
        for (size_t k = i + 1; k < end; ++k) {
            if (c.blocks[k].kind == AiBlockKind::Acts) acts = k;
            if (c.blocks[k].kind == AiBlockKind::Stopped && !last_turn) c.blocks[k].accept.clear();
        }
        if (count > 1 && acts == c.blocks.size() && !(c.busy && last_turn)) {
            AiBlockView row;
            row.kind = AiBlockKind::Acts;
            row.id = c.next_id++;
            row.ver_only = true;
            c.blocks.insert(c.blocks.begin() + static_cast<std::ptrdiff_t>(end), std::move(row));
            acts = end++;
        }
        if (acts < c.blocks.size()) {
            c.blocks[acts].ver = count > 1 ? cur : 0;
            c.blocks[acts].ver_count = count > 1 ? count : 0;
        }
        i = end - 1;
    }
}

// Ends the turn: the send button and the chip settle.
void Finish(AppState& s, bool done_chip) {
    AiChat& c = Chat(s);
    c.busy = false;
    c.think_id = 0;
    MarkVersions(c);
    if (done_chip) {
        SetChip(c, ui::StatusAiState::Done, Text(I::AiAsstChipDone));
        if (!s.aiAssistant.open) {
            c.unread = true;
            AiChatPeek(s, 650);
        }
    } else {
        SetChip(c, ui::StatusAiState::Idle, L"");
    }
    AiHistorySave(s);   // 55 P3: every finished (or stopped) turn
}

// Thinking: the header streams the reason, then folds into one line.
void PushThinking(AiChat& c, uint32_t delay, std::wstring reason) {
    auto id = std::make_shared<uint32_t>(0);
    Push(c, delay, std::function<void(AppState&)>([id](AppState& s) {
             AiChat& ch = Chat(s);
             AiBlockView& b = ch.Add(AiBlockKind::Thinking, Now());
             b.running = true;
             b.label = Text(I::AiAsstThinking);
             b.meta = L"\u00B7 " + EffortName(s);
             *id = b.id;
             ch.think_id = b.id;
             ch.think_start = Now();
         }));
    auto pos = std::make_shared<size_t>(0);
    Push(c, 0, std::function<int(AppState&)>([id, pos, reason = std::move(reason)](AppState& s) {
             AiBlockView* b = Chat(s).Find(*id);
             if (!b) return -1;
             *pos = (std::min)(reason.size(), *pos + 2);
             b->text = reason.substr(0, *pos);
             return *pos < reason.size() ? 66 : -1;   // 30 cps
         }));
    Push(c, 500, std::function<void(AppState&)>([id](AppState& s) {
             AiChat& ch = Chat(s);
             AiBlockView* b = ch.Find(*id);
             if (!b) return;
             const uint64_t secs = std::max<uint64_t>(1, (Now() - ch.think_start + 500) / 1000);
             b->running = false;
             b->label = Fill(Text(I::AiAsstThought), L"{n}", std::to_wstring(secs)) + L" \u00B7 " + EffortName(s);
             b->meta.clear();
             b->seconds.clear();
             ch.think_id = 0;
         }));
}

void StartOrganize(AppState& s, AiChat& c, const std::wstring& prompt);

void StartPlain(AppState& s, AiChat& c, const std::wstring& reason, const std::wstring& answer, bool think = true) {
    if (think && AiAssistantThinks(s)) PushThinking(c, 350, reason);
    auto tid = std::make_shared<uint32_t>(0);
    Push(c, 300, std::function<void(AppState&)>([tid](AppState& st) {
             AiBlockView& t = Chat(st).Add(AiBlockKind::Text, Now());
             t.streaming = true;
             *tid = t.id;
         }));
    Push(c, 0, std::function<int(AppState&)>([tid, answer, pos = std::make_shared<size_t>(0)](AppState& st) {
             AiBlockView* b = Chat(st).Find(*tid);
             if (!b) return -1;
             *pos = (std::min)(answer.size(), *pos + 2);
             b->text = answer.substr(0, *pos);
             b->streaming = *pos < answer.size();
             return *pos < answer.size() ? 44 : -1;
         }));
    Push(c, 200, std::function<void(AppState&)>([](AppState& st) { Finish(st, true); }));
}

void StartOrganize(AppState& s, AiChat& c, const std::wstring& prompt) {
    (void)prompt;
    auto scan = std::make_shared<FolderScan>(Scan(s));
    const FolderScan& f = *scan;
    if (!f.ok || f.files.empty()) {
        StartPlain(s, c, L"\u7528\u6237\u60F3\u6574\u7406\u6587\u4EF6\uFF0C\u4F46\u73B0\u5728\u6CA1\u6709\u6253\u5F00\u7684\u6587\u4EF6\u5939\u3002",
                   Text(I::AiAsstNoFolder));
        return;
    }
    const size_t nfiles = f.files.size();
    const size_t ngroups = f.groups.size();
    std::wstring cats;
    for (const auto& g : f.groups) {
        if (!cats.empty()) cats += L"\u3001";
        cats += Text(kCats[static_cast<int>(g.cat)].name);
    }
    // The model's words (A3: DeepSeek writes these).
    const std::wstring reason =
        L"\u7528\u6237\u60F3\u6309\u7C7B\u578B\u6574\u7406\u300C" + f.name +
        L"\u300D\u3002\u5148\u770B\u5F53\u524D\u7A97\u53E3\u548C\u8FD9\u4E2A\u6587\u4EF6\u5939\u91CC\u6709\u4EC0\u4E48\uFF0C\u518D\u6309\u6269\u5C55\u540D\u5206\u7EC4\uFF1A" +
        cats +
        L"\u3002\u76EE\u6807\u6587\u4EF6\u5939\u90FD\u8FD8\u4E0D\u5B58\u5728\uFF0C\u9700\u8981\u65B0\u5EFA\uFF0C\u8FD9\u8981\u5728\u65B9\u6848\u91CC\u5199\u6E05\u695A\uFF1B\u4E0D\u8986\u76D6\u5DF2\u6709\u6587\u4EF6\uFF0C\u505A\u5B8C\u8981\u80FD\u64A4\u9500\u3002";
    if (AiAssistantThinks(s)) PushThinking(c, 350, reason);

    // Scene 3: look at the window, read the folder.
    auto steps1 = std::make_shared<uint32_t>(0);
    Push(c, 300, std::function<void(AppState&)>([steps1, scan](AppState& st) {
             AiChat& ch = Chat(st);
             SetChip(ch, ui::StatusAiState::Busy,
                     Fill(Fill(Text(I::AiAsstChipReading), L"{k}", L"0"), L"{n}", Num(scan->files.size())), 0.1f);
             AiBlockView& b = ch.Add(AiBlockKind::Steps, Now());
             b.label = Fill(Fill(Text(I::AiAsstStepsRead), L"{folder}", scan->name), L"{n}", Num(scan->files.size()));
             ui::AiStepView a;
             a.icon = ui::AiIcon::Window;
             a.title = Text(I::AiAsstStepWindow);
             a.sub = Text(I::AiAsstStepWindowRun);
             a.born_ms = Now();
             b.steps.push_back(a);
             *steps1 = b.id;
         }));
    Push(c, 700, std::function<void(AppState&)>([steps1, scan](AppState& st) {
             AiBlockView* b = Chat(st).Find(*steps1);
             if (!b || b->steps.empty()) return;
             ui::AiStepView& a = b->steps[0];
             a.state = ui::AiStepState::Done;
             a.done_ms = Now();
             a.sub = Fill(Fill(Text(I::AiAsstStepWindowDone), L"{folder}", scan->name), L"{n}", Num(scan->items));
             ui::AiStepView r;
             r.icon = ui::AiIcon::Rows;
             r.title = Text(I::AiAsstStepRead);
             r.arg = scan->name;
             r.sub = Fill(Text(I::AiAsstStepReadRun), L"{n}", Num(scan->files.size()));
             r.born_ms = Now();
             b->steps.push_back(r);
         }));
    auto k = std::make_shared<size_t>(0);
    Push(c, 300, std::function<int(AppState&)>([steps1, scan, k](AppState& st) {
             AiChat& ch = Chat(st);
             AiBlockView* b = ch.Find(*steps1);
             if (!b || b->steps.size() < 2) return -1;
             const size_t n = scan->files.size();
             // Large folders read in bigger strides so the walk stays ~2 s.
             const size_t stride = std::max<size_t>(1, n / 12);
             *k = (std::min)(n, *k + stride);
             const std::wstring kn = Num(*k), nn = Num(n);
             b->steps[1].sub = Fill(Fill(Text(I::AiAsstStepReading), L"{k}", kn), L"{n}", nn);
             SetChip(ch, ui::StatusAiState::Busy, Fill(Fill(Text(I::AiAsstChipReading), L"{k}", kn), L"{n}", nn),
                     static_cast<float>(*k) / static_cast<float>(n));
             return *k < n ? 185 : -1;
         }));
    Push(c, 800, std::function<void(AppState&)>([steps1, scan](AppState& st) {
             AiBlockView* b = Chat(st).Find(*steps1);
             if (!b || b->steps.size() < 2) return;
             ui::AiStepView& r = b->steps[1];
             r.state = ui::AiStepState::Done;
             r.done_ms = Now();
             r.sub = Fill(Fill(Text(I::AiAsstStepReadDone), L"{n}", Num(scan->files.size())), L"{size}",
                          FormatSize(scan->bytes));
         }));
    // Intro line, then the plan card with its groups sliding in.
    std::wstring intro = L"\u300C" + f.name + L"\u300D\u91CC\u6709 " + Num(nfiles) + L" \u4E2A\u6587\u4EF6";
    if (f.dirs == 0) intro += L"\uFF0C\u6CA1\u6709\u5B50\u6587\u4EF6\u5939";
    intro += L"\u3002\u6309\u7C7B\u578B\u53EF\u4EE5\u5206\u6210 **" + Num(ngroups) +
             L" \u7EC4**\uFF0C\u76EE\u6807\u6587\u4EF6\u5939\u90FD\u9700\u8981\u65B0\u5EFA\uFF1A";
    auto intro_id = std::make_shared<uint32_t>(0);
    Push(c, 350, std::function<void(AppState&)>([intro_id](AppState& st) {
             AiBlockView& t = Chat(st).Add(AiBlockKind::Text, Now());
             t.streaming = true;
             *intro_id = t.id;
         }));
    Push(c, 0, std::function<int(AppState&)>([intro_id, intro, pos = std::make_shared<size_t>(0)](AppState& st) {
             AiBlockView* b = Chat(st).Find(*intro_id);
             if (!b) return -1;
             *pos = (std::min)(intro.size(), *pos + 2);
             if (*pos < intro.size() && intro[*pos - 1] == L'*' && intro[*pos] == L'*') ++*pos;
             b->text = intro.substr(0, *pos);
             b->streaming = *pos < intro.size();
             return *pos < intro.size() ? 50 : -1;   // 40 cps
         }));
    auto plan = std::make_shared<uint32_t>(0);
    Push(c, 200, std::function<void(AppState&)>([plan, scan](AppState& st) {
             AiBlockView& b = Chat(st).Add(AiBlockKind::Plan, Now());
             b.label = Text(I::AiAsstPlanTitle);
             b.meta = Fill(Fill(Text(I::AiAsstPlanMeta), L"{g}", Num(scan->groups.size())), L"{n}",
                           Num(scan->files.size()));
             for (const auto& g : scan->groups) {
                 const CatInfo& info = kCats[static_cast<int>(g.cat)];
                 ui::AiGroupView v;
                 v.icon = info.icon;
                 v.tint = info.tint;
                 v.name = Text(info.name);
                 v.count = Fill(Text(I::AiAsstCount), L"{n}", Num(g.files.size()));
                 for (size_t i = 0; i < g.files.size() && i < 40; ++i) {
                     if (!v.list.empty()) v.list += L"\u3001";
                     v.list += scan->files[g.files[i]].name;
                 }
                 if (!g.exists) v.badge = Text(I::AiAsstNewBadge);
                 b.groups.push_back(std::move(v));
             }
             *plan = b.id;
         }));
    auto gi = std::make_shared<size_t>(0);
    Push(c, 0, std::function<int(AppState&)>([plan, gi](AppState& st) {
             AiBlockView* b = Chat(st).Find(*plan);
             if (!b || *gi >= b->groups.size()) return -1;
             b->groups[(*gi)++].in_ms = Now();
             return *gi < b->groups.size() ? 170 : -1;
         }));

    // Scene 4: ask in the conversation; the run waits for the answer.
    Push(c, 400, std::function<void(AppState&)>([scan, plan](AppState& st) {
             AiChat& ch = Chat(st);
             SetChip(ch, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));
             AiBlockView& b = ch.Add(AiBlockKind::Confirm, Now());
             const std::wstring cn = Num(scan->files.size()), cg = Num(scan->groups.size());
             b.title = Fill(Fill(Text(I::AiAsstConfirmTitle), L"{n}", cn), L"{g}", cg);
             b.sub = Fill(Text(I::AiAsstConfirmSub), L"{folder}", scan->name);
             b.deny = Text(I::AiAsstDeny);
             b.accept = Text(I::AiAsstOrganize);
             ch.confirm_id = b.id;
             const uint32_t plan_id = *plan;
             ch.on_confirm = [scan, plan_id](AppState& s2, bool accept) {
                 AiChat& c2 = Chat(s2);
                 AiBlockView* cf = c2.Find(c2.confirm_id);
                 const std::wstring n2 = Num(scan->files.size()), g2 = Num(scan->groups.size());
                 if (cf) {
                     // Time spent waiting for the user is not model time.
                     const uint64_t now2 = Now();
                     if (now2 > cf->born_ms) c2.turn_start += now2 - cf->born_ms;
                     cf->done = true;
                     cf->title = Fill(Text(accept ? I::AiAsstConfirmed : I::AiAsstDenied), L"{n}", n2);
                 }
                 if (AiBlockView* p = c2.Find(plan_id)) p->collapsed = true;
                 c2.confirm_id = 0;
                 c2.on_confirm = nullptr;
                 if (!accept) {
                     SetChip(c2, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                     StartPlain(s2, c2, std::wstring(),
                                L"\u597D\u7684\uFF0C\u6CA1\u6709\u6539\u52A8\u4EFB\u4F55\u6587\u4EF6\u3002\u9700\u8981\u7684\u8BDD\u53EF\u4EE5\u6362\u4E2A\u5206\u6CD5\u518D\u8BD5\u3002",
                                false);
                     Schedule(s2);
                     return;
                 }
                 // Scene 5: "sort" with a progress step (demo: nothing moves).
                 auto steps2 = std::make_shared<uint32_t>(0);
                 Push(c2, 250, std::function<void(AppState&)>([steps2, scan](AppState& s3) {
                          AiChat& c3 = Chat(s3);
                          AiBlockView& b2 = c3.Add(AiBlockKind::Steps, Now());
                          const std::wstring n3 = Num(scan->files.size()), g3 = Num(scan->groups.size());
                          b2.label = Fill(Fill(Text(I::AiAsstStepsMove), L"{n}", n3), L"{g}", g3);
                          ui::AiStepView m;
                          m.icon = ui::AiIcon::Move;
                          m.title = Text(I::AiAsstStepMove);
                          m.arg = scan->name;
                          m.sub = Fill(Text(I::AiAsstStepMoveRun), L"{g}", g3);
                          m.born_ms = Now();
                          m.progress = 0.0f;
                          b2.steps.push_back(m);
                          *steps2 = b2.id;
                          SetChip(c3, ui::StatusAiState::Busy,
                                  Fill(Fill(Text(I::AiAsstChipOrganizing), L"{k}", L"0"), L"{n}", n3), 0.0f);
                      }));
                 auto mk = std::make_shared<size_t>(0);
                 // Folders appear (5 × 120 + 420 in v2.js), then 190 ms a file.
                 Push(c2, 300 + 120 * static_cast<uint32_t>(scan->groups.size()) + 420,
                      std::function<int(AppState&)>([steps2, scan, mk](AppState& s3) {
                          AiChat& c3 = Chat(s3);
                          AiBlockView* b2 = c3.Find(*steps2);
                          if (!b2 || b2->steps.empty()) return -1;
                          const size_t n = scan->files.size();
                          const size_t stride = std::max<size_t>(1, n / 12);
                          *mk = (std::min)(n, *mk + stride);
                          const std::wstring kn = Num(*mk), nn = Num(n);
                          const float p = static_cast<float>(*mk) / static_cast<float>(n);
                          b2->steps[0].progress = p;
                          b2->steps[0].sub = Fill(Fill(Text(I::AiAsstStepMoving), L"{k}", kn), L"{n}", nn);
                          SetChip(c3, ui::StatusAiState::Busy,
                                  Fill(Fill(Text(I::AiAsstChipOrganizing), L"{k}", kn), L"{n}", nn), p);
                          return *mk < n ? 190 : -1;
                      }));
                 Push(c2, 650, std::function<void(AppState&)>([steps2, scan](AppState& s3) {
                          AiBlockView* b2 = Chat(s3).Find(*steps2);
                          if (!b2 || b2->steps.empty()) return;
                          ui::AiStepView& m = b2->steps[0];
                          m.state = ui::AiStepState::Done;
                          m.done_ms = Now();
                          m.progress = -1.0f;
                          m.sub = Fill(Fill(Text(I::AiAsstStepMoveDone), L"{n}", Num(scan->files.size())), L"{g}",
                                       Num(scan->groups.size()));
                      }));
                 // Scene 6: fold the steps, result card, streamed answer, acts.
                 Push(c2, 400, std::function<void(AppState&)>([scan](AppState& s3) {
                          AiChat& c3 = Chat(s3);
                          for (AiBlockView& b3 : c3.blocks)
                              if (b3.kind == AiBlockKind::Steps && !b3.collapsed) { b3.collapsed = true; b3.open = false; }
                          AiBlockView& r = c3.Add(AiBlockKind::Result, Now());
                          const std::wstring n3 = Num(scan->files.size()), g3 = Num(scan->groups.size());
                          r.title = Fill(Text(I::AiAsstResultTitle), L"{n}", n3);
                          r.sub = Fill(Fill(Text(I::AiAsstResultSub), L"{folder}", scan->name), L"{g}", g3);
                          for (const auto& g : scan->groups) {
                              const CatInfo& info = kCats[static_cast<int>(g.cat)];
                              r.tags.push_back({info.tint, Text(info.name) + L" " + Num(g.files.size())});
                          }
                          r.deny = Text(I::AiAsstUndo);
                          r.accept = Text(I::AiAsstShowInPulse);
                          c3.result_id = r.id;
                          c3.on_undo = [scan](AppState& s4) {
                              AiChat& c4 = Chat(s4);
                              SetChip(c4, ui::StatusAiState::Busy, Text(I::AiAsstChipUndoing), 0.5f);
                              Push(c4, 1200, std::function<void(AppState&)>([scan](AppState& s5) {
                                       AiChat& c5 = Chat(s5);
                                       if (AiBlockView* rr = c5.Find(c5.result_id)) {
                                           rr->done = true;
                                           rr->title = Text(I::AiAsstUndone);
                                           rr->sub = Fill(Fill(Fill(Text(I::AiAsstUndoneSub), L"{n}",
                                                                    Num(scan->files.size())),
                                                               L"{folder}", scan->name),
                                                          L"{g}", Num(scan->groups.size()));
                                       }
                                       AiBlockView& note = c5.Add(AiBlockKind::Note, Now());
                                       note.text = Text(I::AiAsstUndoNote);
                                       SetChip(c5, ui::StatusAiState::Done, Text(I::AiAsstUndone));
                                       c5.on_undo = nullptr;
                                   }));
                              Schedule(s4);
                          };
                      }));
                 // The answer (A3: the model's words).
                 std::wstring counts;
                 size_t biggest = 0;
                 for (size_t i = 0; i < scan->files.size(); ++i)
                     if (scan->files[i].size > scan->files[biggest].size) biggest = i;
                 for (const auto& g : scan->groups) {
                     if (!counts.empty()) counts += L" \u00B7 ";
                     counts += Text(kCats[static_cast<int>(g.cat)].name) + L" " + Num(g.files.size());
                 }
                 std::wstring answer = L"\u6574\u7406\u597D\u4E86\uFF0C**" + n2 + L" \u4E2A\u6587\u4EF6**\u5DF2\u5206\u5230 " +
                                       g2 + L" \u4E2A\u65B0\u6587\u4EF6\u5939\uFF1A\n- " + counts + L"\n";
                 if (!scan->files.empty())
                     answer += L"- \u6700\u5360\u7A7A\u95F4\u7684\u662F **" + scan->files[biggest].name + L"**\uFF08" +
                               FormatSize(scan->files[biggest].size) +
                               L"\uFF09\uFF0C\u4E0D\u9700\u8981\u7684\u8BDD\u53EF\u4EE5\u79FB\u8D70\u3002\n";
                 answer += L"\u4E0D\u6EE1\u610F\u5C31\u70B9\u4E0A\u9762\u7684\u300C\u64A4\u9500\u300D\uFF0C\u6240\u6709\u6587\u4EF6\u4F1A\u56DE\u5230\u539F\u5904\u3002";
                 auto ans = std::make_shared<uint32_t>(0);
                 Push(c2, 300, std::function<void(AppState&)>([ans](AppState& s3) {
                          AiBlockView& t = Chat(s3).Add(AiBlockKind::Text, Now());
                          t.streaming = true;
                          *ans = t.id;
                      }));
                 Push(c2, 0, std::function<int(AppState&)>([ans, answer, pos = std::make_shared<size_t>(0)](AppState& s3) {
                          AiBlockView* b3 = Chat(s3).Find(*ans);
                          if (!b3) return -1;
                          *pos = (std::min)(answer.size(), *pos + 2);
                          if (*pos < answer.size() && answer[*pos - 1] == L'*' && answer[*pos] == L'*') ++*pos;
                          b3->text = answer.substr(0, *pos);
                          b3->streaming = *pos < answer.size();
                          return *pos < answer.size() ? 43 : -1;   // 46 cps
                      }));
                 Push(c2, 0, std::function<void(AppState&)>([](AppState& s3) {
                          AiChat& c3 = Chat(s3);
                          AiBlockView& a = c3.Add(AiBlockKind::Acts, Now());
                          const double secs = static_cast<double>(Now() - c3.turn_start) / 1000.0;
                          wchar_t buf[16];
                          swprintf_s(buf, L"%.1f", secs);
                          const bool off = !AiAssistantThinks(s3);
                          std::wstring meta = Text(off ? I::AiAsstActsMetaOff : I::AiAsstActsMeta);
                          meta = Fill(meta, L"{model}", ModelName(s3));
                          if (!off) meta = Fill(meta, L"{effort}", EffortName(s3));
                          meta = Fill(Fill(meta, L"{sec}", buf), L"{tools}", L"3");
                          a.meta = meta;
                          Finish(s3, true);
                      }));
                 Schedule(s2);
             };
         }));
}

} // namespace

void AiChatPush(AppState& s, uint32_t delay, std::function<int(AppState&)> run) {
    Push(Chat(s), delay, std::move(run));
    Schedule(s);
}

void AiChatRunDue(AppState& s) {
    if (!s.aiAssistant.chat) return;
    const AiChat& c = *s.aiAssistant.chat;
    if (c.queue.empty() || Now() < c.next_at) return;
    RunDue(&s);
}

void AiChatSetChip(AiChat& c, ui::StatusAiState state, std::wstring text, float progress) {
    SetChip(c, state, std::move(text), progress);
}

void AiChatFinish(AppState& s, bool done_chip) { Finish(s, done_chip); }
std::wstring AiChatEffortName(const AppState& s) { return EffortName(s); }
std::wstring AiChatModelId(const AppState& s) { return ModelName(s); }
std::wstring AiChatFormatSize(uint64_t bytes) { return FormatSize(bytes); }
void AiChatCopyText(HWND hwnd, const std::wstring& text) { CopyText(hwnd, text); }

void AiChatAddActs(AppState& s, int tools, bool failed) {
    AiChat& c = Chat(s);
    AiBlockView& a = c.Add(AiBlockKind::Acts, Now());
    if (failed) {
        // An error: effort, 0.0 秒 and 0 次 say nothing; Acts.done drops 👍.
        a.meta = ModelName(s);
        a.done = true;
        return;
    }
    const double secs = static_cast<double>(Now() - c.turn_start) / 1000.0;
    wchar_t buf[16];
    swprintf_s(buf, L"%.1f", secs);
    const bool off = !AiAssistantThinks(s);
    std::wstring meta = Text(off ? I::AiAsstActsMetaOff : I::AiAsstActsMeta);
    meta = Fill(meta, L"{model}", ModelName(s));
    if (!off) meta = Fill(meta, L"{effort}", EffortName(s));
    a.meta = Fill(Fill(meta, L"{sec}", buf), L"{tools}", std::to_wstring(tools));
}

namespace {
void CALLBACK PeekTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!s || !s->aiAssistant.chat) return;
    AiChat& c = *s->aiAssistant.chat;
    Redraw(*s);
    const uint64_t now = Now();
    const uint64_t next = c.peek_at > now ? c.peek_at : c.peek_until > now ? c.peek_until : 0;
    if (next) SetTimer(hwnd, kPeekTimer, static_cast<UINT>(next - now + 1), PeekTimer);
}
}  // namespace

void AiChatPeek(AppState& s, uint32_t delay) {
    AiChat& c = Chat(s);
    if (s.aiAssistant.open || c.busy || !c.turn_start) return;
    if (c.chip != ui::StatusAiState::Done && !c.peek_failed) return;
    if (c.peek_turn == c.turn_start) return;   // once per turn
    if (c.peek_text.empty()) {
        // No summary of its own: the start of the answer.
        for (auto it = c.blocks.rbegin(); it != c.blocks.rend(); ++it) {
            if (it->kind == AiBlockKind::User) break;
            if (it->kind != AiBlockKind::Text) continue;
            std::wstring t = Plain(it->text);
            for (wchar_t& ch : t)
                if (ch == L'\n' || ch == L'\r') ch = L' ';
            if (t.size() > 80) t = t.substr(0, 78) + L"\u2026";
            c.peek_text = t;
            break;
        }
    }
    if (c.peek_text.empty()) return;
    c.peek_turn = c.turn_start;
    c.peek_at = Now() + delay;
    c.peek_until = c.peek_at + kPeekMs;
    if (s.hwnd) SetTimer(s.hwnd, kPeekTimer, delay + 1, PeekTimer);
}

void AiChatHidePeek(AppState& s) {
    AiChat& c = Chat(s);
    if (!c.peek_at) return;
    c.peek_at = 0;
    c.peek_until = 0;
    Redraw(s);
}

void AiChatFillPeek(AppState& s, bool panel_open, ui::AiPeekView& peek) {
    AiChat& c = Chat(s);
    if (c.peek_text.empty() || !c.peek_turn) return;
    const uint64_t now = Now();
    peek.on = !panel_open && c.peek_at && now >= c.peek_at && now < c.peek_until;
    peek.text = c.peek_text;
    if (c.peek_failed) {
        peek.failed = true;
        peek.tone = c.peek_tone;
        peek.title = c.peek_title;
        peek.undo = Text(c.peek_tone == ui::AiErrTone::Accent ? I::AiErrBtnConnect : I::AiAsstRetry);
        peek.open = Text(I::AiPeekOpen);
        return;
    }
    peek.title = Text(I::AiPeekDone);
    bool can_undo = c.peek_undo && c.on_undo;
    if (const AiBlockView* r = c.Find(c.result_id); !r || r->done) can_undo = false;
    peek.undo = can_undo ? Text(I::AiAsstUndo) : std::wstring();
    peek.open = Text(I::AiPeekOpen);
}

void AiChatUndoLast(AppState& s) {
    AiChat& c = Chat(s);
    for (size_t i = 0; i < c.blocks.size(); ++i)
        if (c.blocks[i].id == c.result_id) {
            AiChatActivate(s, ui::AiPanelPart::ResultUndo, static_cast<int>(i));
            return;
        }
}

void AiChatFail(AppState& s, AiChatError err) {
    AiChat& c = Chat(s);
    const uint64_t now = Now();
    // A partial answer after the user's bubble stays; an amber strip says so.
    bool partial = false;
    for (auto it = c.blocks.rbegin(); it != c.blocks.rend(); ++it) {
        if (it->kind == AiBlockKind::User) break;
        if (it->kind == AiBlockKind::Text && !Plain(it->text).empty()) { partial = true; break; }
    }
    if (err.cut && partial) {
        AiBlockView& b = c.Add(AiBlockKind::Cut, now);
        b.title = Text(I::AiErrCutT);
        b.sub = Text(I::AiErrCutS);
        b.accept = Text(I::AiAsstRetry);
    } else {
        AiBlockView& b = c.Add(AiBlockKind::Error, now);
        b.tone = err.tone;
        b.icon = err.icon;
        b.title = err.title;
        b.sub = err.sub;
        b.detail = err.detail;
        b.hint = err.hint;
        b.actions = err.buttons;
        b.meta = ModelName(s);
    }
    c.busy = false;
    c.think_id = 0;
    // The peek: the card's title; nothing moved, or what did can be undone.
    c.peek_failed = true;
    c.peek_tone = err.tone;
    c.peek_title = !err.peek_title.empty() ? err.peek_title : err.title;
    std::wstring what = c.last_prompt;
    for (wchar_t& ch : what)
        if (ch == L'\n' || ch == L'\r') ch = L' ';
    if (what.size() > 16) what = what.substr(0, 15) + L"\u2026";
    const AiBlockView* r = c.Find(c.result_id);
    c.peek_text = Fill(Text(r && !r->done ? I::AiPeekFailUndo : I::AiPeekFail), L"{p}", what);
    c.peek_undo = false;
    // The chip: red needs you, amber will pass; not connected is no error.
    // With the panel open the card says it all.
    if (s.aiAssistant.open || err.tone == ui::AiErrTone::Accent) {
        SetChip(c, ui::StatusAiState::Idle, L"");
    } else {
        const bool red = err.tone == ui::AiErrTone::Err;
        SetChip(c, red ? ui::StatusAiState::Error : ui::StatusAiState::Warn,
                Text(red ? I::AiChipError : I::AiChipUnfinished));
        c.unread = true;
    }
    if (!s.aiAssistant.open) AiChatPeek(s, 650);
    MarkVersions(c);
    Redraw(s);
}

bool AiChatErrAction(AppState& s, int index, ui::AiErrAct& act) {
    AiChat& c = Chat(s);
    if (index < 0) return false;
    const size_t block = static_cast<size_t>(index / 4), button = static_cast<size_t>(index % 4);
    if (block >= c.blocks.size()) return false;
    const AiBlockView& b = c.blocks[block];
    if (b.kind != AiBlockKind::Error || b.gone_ms || button >= b.actions.size()) return false;
    act = b.actions[button].act;
    return true;
}

void AiChatErrRun(AppState& s, int index) {
    ui::AiErrAct act{};
    if (!AiChatErrAction(s, index, act)) return;
    AiChat& c = Chat(s);
    const AiBlockView& b = c.blocks[static_cast<size_t>(index / 4)];
    switch (act) {
    case ui::AiErrAct::Retry:
        AiChatRetryTurn(s);
        break;
    case ui::AiErrAct::TopUp:
        // Only offered for the official service.
        ShellExecuteW(s.hwnd, L"open", L"https://platform.deepseek.com/top_up", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case ui::AiErrAct::CopyDetail:
        CopyText(s.hwnd, b.title + (b.detail.empty() ? L"" : L"\r\n" + b.detail));
        break;
    case ui::AiErrAct::TaskRetry:
    case ui::AiErrAct::TaskSkip:
    case ui::AiErrAct::TaskUndo:
        // A paused task's card (flow C); the handler clears itself.
        if (b.id == c.pause_id && c.on_task_act) {
            auto fn = c.on_task_act;
            fn(s, act);
        }
        break;
    default:
        break;
    }
    Redraw(s);
}

void AiChatRetryTurn(AppState& s) {
    AiChat& c = Chat(s);
    if (c.busy || c.last_prompt.empty()) return;
    size_t user = c.blocks.size();
    for (size_t i = c.blocks.size(); i-- > 0;)
        if (c.blocks[i].kind == AiBlockKind::User) { user = i; break; }
    if (user == c.blocks.size()) return;
    // An organize that already ran keeps its card (and its undo): then the
    // retry is a new turn as before.
    for (size_t i = user + 1; i < c.blocks.size(); ++i)
        if (c.blocks[i].kind == AiBlockKind::Result) {
            const std::wstring context = c.blocks[user].context;
            AiChatSubmit(s, c.last_prompt, context);
            return;
        }
    const uint64_t now = Now();
    const bool motion_on = ui::motion::SystemAnimationsEnabled();
    // The card fades out (.25 s), then the failed turn goes and the prompt
    // is sent again under the same bubble.
    for (size_t i = user + 1; i < c.blocks.size(); ++i)
        if (c.blocks[i].kind == AiBlockKind::Error || c.blocks[i].kind == AiBlockKind::Cut) c.blocks[i].gone_ms = now;
    c.busy = true;
    c.unread = false;
    c.turn_start = now;
    c.queue.clear();
    c.result_id = 0;
    c.peek_text.clear();
    c.peek_undo = false;
    c.peek_failed = false;
    c.peek_at = c.peek_until = 0;
    SetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
    const uint32_t user_id = c.blocks[user].id;
    Push(c, motion_on ? 260u : 0u, std::function<void(AppState&)>([user_id](AppState& st) {
             AiChat& ch = Chat(st);
             size_t at = ch.blocks.size();
             for (size_t i = 0; i < ch.blocks.size(); ++i)
                 if (ch.blocks[i].id == user_id) { at = i; break; }
             if (at == ch.blocks.size()) return;
             ch.blocks.erase(ch.blocks.begin() + static_cast<std::ptrdiff_t>(at + 1), ch.blocks.end());
             st.renderer.AiPanel().ScrollToEnd();
             AiAgentStart(st, ch.last_prompt, !ch.blocks[at].context.empty());
         }));
    Schedule(s);
    Redraw(s);
}

void AiChatGroupLook(const std::vector<std::wstring>& names, ui::AiIcon& icon, ui::AiTint& tint) {
    // The most common kind, when it is at least half the group; else other.
    int counts[static_cast<int>(Cat::Count)] = {};
    for (const std::wstring& n : names) ++counts[static_cast<int>(CatOf(n))];
    int best = 0;
    for (int k = 1; k < static_cast<int>(Cat::Other); ++k)
        if (counts[k] > counts[best]) best = k;
    if (counts[best] == 0 || counts[best] * 2 < static_cast<int>(names.size())) best = static_cast<int>(Cat::Other);
    icon = best == static_cast<int>(Cat::Other) ? ui::AiIcon::Folder : kCats[best].icon;
    tint = kCats[best].tint;
}

AiBlockView* AiChat::Find(uint32_t id) {
    if (!id) return nullptr;
    for (AiBlockView& b : blocks)
        if (b.id == id) return &b;
    return nullptr;
}

AiBlockView& AiChat::Add(AiBlockKind kind, uint64_t now_ms) {
    AiBlockView b;
    b.kind = kind;
    b.id = next_id++;
    b.born_ms = now_ms;
    blocks.push_back(std::move(b));
    return blocks.back();
}

AiChat& Chat(AppState& s) {
    if (!s.aiAssistant.chat) s.aiAssistant.chat = std::make_shared<AiChat>();
    return *s.aiAssistant.chat;
}

namespace {
// PULSE_AI_DEMO=1 plays the local demo (screenshots, no key); otherwise the
// turn goes to the model.
bool DemoMode() {
    static const bool demo = [] {
        wchar_t v[8]{};
        return GetEnvironmentVariableW(L"PULSE_AI_DEMO", v, 8) > 0 && v[0] == L'1';
    }();
    return demo;
}

// PULSE_AI_DEMO_ANSWER=<UTF-8 file>: the demo answers with that Markdown
// (screenshots and checks of rich answers) instead of organizing. Answers
// split by a line "=====" are the versions 重新生成 brings, in turn.
std::vector<std::wstring> DemoAnswers() {
    wchar_t path[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"PULSE_AI_DEMO_ANSWER", path, MAX_PATH) == 0) return {};
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string bytes(64 * 1024, '\0');
    DWORD got = 0;
    const BOOL ok = ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
    CloseHandle(f);
    if (!ok || got == 0) return {};
    bytes.resize(got);
    if (bytes.rfind("\xEF\xBB\xBF", 0) == 0) bytes.erase(0, 3);
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), n);
    text.erase(std::remove(text.begin(), text.end(), L'\r'), text.end());
    std::vector<std::wstring> out;
    size_t from = 0;
    for (;;) {
        const size_t at = text.find(L"\n=====\n", from);
        out.push_back(text.substr(from, at == std::wstring::npos ? std::wstring::npos : at - from));
        if (at == std::wstring::npos) break;
        from = at + 7;
    }
    return out;
}

// The demo answer streams into the text block from `from` (12 characters
// every 30 ms), then the acts row.
void StreamDemo(AiChat& c, uint32_t text_id, std::wstring answer, size_t from) {
    Push(c, 0, std::function<int(AppState&)>([text_id, answer = std::move(answer), pos = std::make_shared<size_t>(from)](AppState& st) {
             AiBlockView* b = Chat(st).Find(text_id);
             if (!b) return -1;
             *pos = (std::min)(answer.size(), *pos + 12);
             b->text = answer.substr(0, *pos);
             b->streaming = *pos < answer.size();
             return b->streaming ? 30 : -1;
         }));
    Push(c, 120, std::function<void(AppState&)>([](AppState& st) {
             AiChatAddActs(st, 0);
             Finish(st, true);
         }));
}

// A turn starts: the send button turns into stop, the chip works.
void BeginTurn(AppState& s, AiChat& c) {
    c.busy = true;
    c.unread = false;
    c.turn_start = Now();
    c.queue.clear();
    c.result_id = 0;
    c.peek_text.clear();
    c.peek_undo = false;
    c.peek_failed = false;
    c.peek_at = c.peek_until = 0;
    SetChip(c, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
    s.renderer.AiPanel().ClearSelection();
    s.renderer.AiPanel().ScrollToEnd();
}

// Asks the user block `user` (its words, its folder) as the turn.
void RunTurn(AppState& s, AiChat& c, size_t user, const std::wstring& note) {
    const uint32_t uid = c.blocks[user].id;
    const std::wstring prompt = c.blocks[user].text;
    const bool with_context = !c.blocks[user].context.empty();
    c.last_prompt = prompt;
    c.turn_hist[uid] = AiAgentHistorySize(s);
    if (!DemoMode()) {
        AiAgentStart(s, prompt, with_context, note);
        Schedule(s);
        return;
    }
    const std::vector<std::wstring> answers = DemoAnswers();
    if (answers.empty()) {
        StartOrganize(s, c, prompt);
    } else {
        size_t ver = 0;
        if (auto it = c.branches.find(uid); it != c.branches.end()) ver = it->second.current;
        AiBlockView& t = c.Add(AiBlockKind::Text, Now());
        t.streaming = true;
        StreamDemo(c, t.id, answers[ver % answers.size()], 0);
    }
    Schedule(s);
}

size_t HistAt(AppState& s, const AiChat& c, uint32_t uid) {
    const auto it = c.turn_hist.find(uid);
    return it != c.turn_hist.end() ? it->second : AiAgentHistorySize(s);
}

// The user block's branch; its first version is what is there now.
AiTurnBranch& BranchOf(AiChat& c, size_t user) {
    AiTurnBranch& br = c.branches[c.blocks[user].id];
    if (br.versions.empty()) {
        AiTurnVersion v;
        v.text = c.blocks[user].text;
        v.context = c.blocks[user].context;
        br.versions.push_back(std::move(v));
        br.current = 0;
    }
    return br;
}

// Everything after the user block, and the model history from its turn on,
// go into `v` (the panel's selection may point into them: it goes).
void Stash(AppState& s, AiChat& c, size_t user, AiTurnVersion& v) {
    const uint32_t uid = c.blocks[user].id;
    v.text = c.blocks[user].text;
    v.context = c.blocks[user].context;
    const auto from = c.blocks.begin() + static_cast<std::ptrdiff_t>(user + 1);
    v.tail.assign(std::make_move_iterator(from), std::make_move_iterator(c.blocks.end()));
    c.blocks.erase(c.blocks.begin() + static_cast<std::ptrdiff_t>(user + 1), c.blocks.end());
    v.history = AiAgentCutHistory(s, HistAt(s, c, uid));
    s.renderer.AiPanel().ClearSelection();
}

void SyncLastPrompt(AiChat& c) {
    for (auto it = c.blocks.rbegin(); it != c.blocks.rend(); ++it)
        if (it->kind == AiBlockKind::User) {
            c.last_prompt = it->text;
            return;
        }
}

// A new version of the user block with these words, asked at once.
void NewVersion(AppState& s, AiChat& c, size_t user, const std::wstring& text, const std::wstring& note) {
    AiTurnBranch& br = BranchOf(c, user);
    Stash(s, c, user, br.versions[br.current]);
    AiTurnVersion v;
    v.text = text;
    v.context = c.blocks[user].context;
    br.versions.push_back(std::move(v));
    br.current = br.versions.size() - 1;
    c.blocks[user].text = text;
    BeginTurn(s, c);
    RunTurn(s, c, user, note);
}
}  // namespace

int AiChatUserOf(const AiChat& c, int block) {
    if (block < 0 || block >= static_cast<int>(c.blocks.size())) return -1;
    for (int i = block; i >= 0; --i)
        if (c.blocks[static_cast<size_t>(i)].kind == AiBlockKind::User) return i;
    return -1;
}

void AiChatSubmit(AppState& s, const std::wstring& prompt, const std::wstring& context) {
    AiChat& c = Chat(s);
    if (c.busy) return;
    if (DemoMode() && !c.noted) {
        // A2: say plainly that this is the demo driver.
        AiBlockView& note = c.Add(AiBlockKind::Note, 0);
        note.text = Text(I::AiAsstDemoNote);
        c.noted = true;
    }
    AiBlockView& u = c.Add(AiBlockKind::User, Now());
    u.text = prompt;
    u.context = context;
    BeginTurn(s, c);
    RunTurn(s, c, c.blocks.size() - 1, {});
    Redraw(s);
}

void AiChatRegenerate(AppState& s, int block, AiRegen kind) {
    AiChat& c = Chat(s);
    if (c.busy) return;
    const int u = AiChatUserOf(c, block);
    if (u < 0) return;
    const size_t user = static_cast<size_t>(u);
    // An organize that already ran in this turn keeps its card and its undo:
    // then the message is asked again as a new turn, as before.
    for (size_t k = user + 1; k < c.blocks.size() && c.blocks[k].kind != AiBlockKind::User; ++k)
        if (c.blocks[k].kind == AiBlockKind::Result) {
            const std::wstring text = c.blocks[user].text, context = c.blocks[user].context;
            AiChatSubmit(s, text, context);
            return;
        }
    const wchar_t* note = kind == AiRegen::Longer    ? L"Answer in more detail this time."
                          : kind == AiRegen::Shorter ? L"Answer more concisely this time, keeping only the key points."
                                                     : L"";
    const std::wstring text = c.blocks[user].text;
    NewVersion(s, c, user, text, note);
    Redraw(s);
}

void AiChatEditTurn(AppState& s, int user_block, const std::wstring& text) {
    AiChat& c = Chat(s);
    if (c.busy || text.empty() || user_block < 0 || user_block >= static_cast<int>(c.blocks.size())) return;
    const size_t user = static_cast<size_t>(user_block);
    if (c.blocks[user].kind != AiBlockKind::User) return;
    NewVersion(s, c, user, text, {});
    Redraw(s);
}

void AiChatSwitchVersion(AppState& s, int block, int delta) {
    AiChat& c = Chat(s);
    if (c.busy) return;
    const int u = AiChatUserOf(c, block);
    if (u < 0) return;
    const size_t user = static_cast<size_t>(u);
    const uint32_t uid = c.blocks[user].id;
    const auto it = c.branches.find(uid);
    if (it == c.branches.end()) return;
    AiTurnBranch& br = it->second;
    const int target = static_cast<int>(br.current) + delta;
    if (target < 0 || target >= static_cast<int>(br.versions.size()) || target == static_cast<int>(br.current)) return;
    Stash(s, c, user, br.versions[br.current]);
    AiTurnVersion& v = br.versions[static_cast<size_t>(target)];
    c.blocks[user].text = v.text;
    c.blocks[user].context = v.context;
    c.blocks.insert(c.blocks.end(), std::make_move_iterator(v.tail.begin()), std::make_move_iterator(v.tail.end()));
    v.tail.clear();
    AiAgentRestoreHistory(s, HistAt(s, c, uid), v.history);
    v.history.reset();
    br.current = static_cast<size_t>(target);
    SyncLastPrompt(c);
    MarkVersions(c);
    AiHistorySave(s);   // 55 P3: the version shown is the one kept
    Redraw(s);
}

void AiChatContinue(AppState& s, int block) {
    AiChat& c = Chat(s);
    if (c.busy) return;
    const int u = AiChatUserOf(c, block);
    if (u < 0) return;
    const size_t user = static_cast<size_t>(u);
    // Only the last turn goes on: the model history ends there.
    size_t text = c.blocks.size();
    for (size_t k = user + 1; k < c.blocks.size(); ++k) {
        if (c.blocks[k].kind == AiBlockKind::User) return;
        if (c.blocks[k].kind == AiBlockKind::Text && !Plain(c.blocks[k].text).empty()) text = k;
    }
    if (text == c.blocks.size()) return;
    // The acts row and the 已停止 line go; the words go on in the same block.
    c.blocks.erase(c.blocks.begin() + static_cast<std::ptrdiff_t>(text + 1), c.blocks.end());
    const std::wstring prompt = c.blocks[user].text;
    const bool with_context = !c.blocks[user].context.empty();
    const std::wstring partial = c.blocks[text].text;
    const uint32_t text_id = c.blocks[text].id;
    BeginTurn(s, c);
    c.last_prompt = prompt;
    c.blocks[text].streaming = true;
    if (DemoMode()) {
        const std::vector<std::wstring> answers = DemoAnswers();
        size_t ver = 0;
        if (auto it = c.branches.find(c.blocks[user].id); it != c.branches.end()) ver = it->second.current;
        const std::wstring answer = answers.empty() ? partial : answers[ver % answers.size()];
        StreamDemo(c, text_id, answer, answer.rfind(partial, 0) == 0 ? partial.size() : answer.size());
    } else {
        AiAgentContinue(s, prompt, with_context, partial, text_id);
    }
    Schedule(s);
    Redraw(s);
}

void AiChatStop(AppState& s) {
    AiChat& c = Chat(s);
    if (!c.busy) return;
    AiAgentStop(s);
    c.queue.clear();
    if (s.hwnd) KillTimer(s.hwnd, kRunTimer);
    for (AiBlockView& b : c.blocks) {
        if (b.kind == AiBlockKind::Thinking && b.running) {
            b.running = false;
            const uint64_t secs = std::max<uint64_t>(1, (Now() - c.think_start + 500) / 1000);
            b.label = Fill(Text(I::AiAsstThought), L"{n}", std::to_wstring(secs));
            b.meta.clear();
            b.seconds.clear();
        }
        b.streaming = false;
        for (ui::AiStepView& st : b.steps)
            if (st.state == ui::AiStepState::Running) { st.state = ui::AiStepState::Failed; st.progress = -1.0f; }
        if (b.kind == AiBlockKind::Confirm && !b.done && b.id == c.confirm_id) {
            b.done = true;
            b.declined = true;
            b.title = Text(I::AiAsstStopped);
        }
        if (b.kind == AiBlockKind::Error && !b.done && b.id == c.pause_id) {
            b.done = true;
            b.icon = ui::AiIcon::Stop;
            b.title = Text(I::AiAsstStopped);
        }
    }
    c.confirm_id = 0;
    c.on_confirm = nullptr;
    c.on_extra = nullptr;
    c.pause_id = 0;
    c.on_task_act = nullptr;
    // 55 P2: a partial answer keeps its acts row and can go on (继续生成);
    // without one, a quiet line says it stopped.
    bool partial = false;
    for (auto it = c.blocks.rbegin(); it != c.blocks.rend(); ++it) {
        if (it->kind == AiBlockKind::User) break;
        if (it->kind == AiBlockKind::Text && !Plain(it->text).empty()) { partial = true; break; }
    }
    if (partial) {
        AiChatAddActs(s, AiAgentToolCount(s));
        AiBlockView& st = c.Add(AiBlockKind::Stopped, Now());
        st.title = Text(I::AiStopVersion) + L" \u00B7 ";
        st.accept = Text(I::AiContinueGen);
    } else {
        AiBlockView& note = c.Add(AiBlockKind::Note, Now());
        note.text = Text(I::AiAsstStopped);
    }
    Finish(s, false);
    Redraw(s);
}

void AiChatReset(AppState& s) {
    if (!s.aiAssistant.chat) return;
    AiChatStop(s);
    AiHistorySave(s);   // 55 P3: before it goes (unchanged: nothing written)
    if (s.hwnd) KillTimer(s.hwnd, kRunTimer);
    s.aiAssistant.chat = std::make_shared<AiChat>();
    s.renderer.AiPanel().ScrollToEnd();
}

void AiChatActivate(AppState& s, ui::AiPanelPart part, int block) {
    AiChat& c = Chat(s);
    if (block < 0 || block >= static_cast<int>(c.blocks.size())) return;
    AiBlockView& b = c.blocks[static_cast<size_t>(block)];
    using P = ui::AiPanelPart;
    switch (part) {
    case P::ThinkToggle:
        if (!b.running) b.open = !b.open;
        break;
    case P::StepsToggle:
        if (b.collapsed) b.open = !b.open;
        break;
    case P::PlanToggle:
    case P::RenameToggle:
    case P::TaskToggle:
    case P::DiskToggle:
    case P::CleanToggle:
    case P::FunnelToggle:
    case P::DupToggle:
        b.collapsed = !b.collapsed;
        break;
    case P::ResultPurge:
        if (b.id == c.result_id && c.on_purge && !b.done && !b.gone) {
            auto fn = c.on_purge;   // asks first (a red confirmation card)
            fn(s);
        }
        break;
    case P::RenameMore:
        b.open = !b.open;
        break;
    case P::ConfirmExtra:
        if (b.id == c.confirm_id && c.on_extra) {
            auto fn = c.on_extra;   // may open a modal dialog; the handler clears itself
            fn(s);
        }
        break;
    case P::ConfirmAccept:
    case P::ConfirmDeny:
        if (part == P::ConfirmAccept && b.id == c.confirm_id && c.on_confirm && !b.arm.empty() && !b.armed) {
            // A permanent delete: the first click only arms the button (P4c).
            b.armed = true;
            std::swap(b.accept, b.arm);
            break;
        }
        if (b.id == c.confirm_id && c.on_confirm) {
            b.declined = part == P::ConfirmDeny;   // the done card: a muted ✕ (每张卡的处理函数只改标题)
            auto fn = c.on_confirm;   // the handler clears itself
            fn(s, part == P::ConfirmAccept);
        }
        break;
    case P::ResultUndo:
        if (b.id == c.result_id && c.on_undo && !b.done) {
            auto fn = c.on_undo;
            c.on_undo = nullptr;
            fn(s);
        }
        break;
    case P::ResultShow:
        if (b.id == c.result_id && c.on_show) {
            auto fn = c.on_show;
            fn(s);
        }
        break;
    case P::ActCopy: {
        // The answer of this turn: Text blocks back to its user bubble.
        std::wstring text;
        for (int i = block - 1; i >= 0; --i) {
            const AiBlockView& t = c.blocks[static_cast<size_t>(i)];
            if (t.kind == AiBlockKind::User) break;
            if (t.kind == AiBlockKind::Text) text = Plain(t.text) + (text.empty() ? L"" : L"\n") + text;
        }
        CopyText(s.hwnd, text);
        break;
    }
    case P::CutRetry:
        AiChatRetryTurn(s);
        break;
    case P::ActRetry:
        AiChatRegenerate(s, block, AiRegen::Again);
        break;
    case P::ActThumb:
        b.rating = b.rating > 0 ? 0 : 1;   // only shown here: nothing is sent
        AiHistorySave(s);
        break;
    case P::ActThumbDown:
        b.rating = b.rating < 0 ? 0 : -1;
        AiHistorySave(s);
        break;
    case P::VerPrev:
    case P::VerNext:
        AiChatSwitchVersion(s, block, part == P::VerPrev ? -1 : 1);
        break;
    case P::StopContinue:
        AiChatContinue(s, block);
        break;
    case P::UserCopy:
        if (b.kind == AiBlockKind::User) CopyText(s.hwnd, b.text);
        break;
    default:
        break;
    }
    Redraw(s);
}

void AiChatClean(AppState& s, ui::AiPanelPart part, int index) {
    AiChat& c = Chat(s);
    if (index < 0) return;
    const bool file = part == ui::AiPanelPart::CleanFile;
    const int row_key = file ? index / 32 : index;
    const int block = row_key / 32, row = row_key % 32, f = file ? index % 32 : -1;
    if (block >= static_cast<int>(c.blocks.size())) return;
    AiBlockView& b = c.blocks[static_cast<size_t>(block)];
    if (b.kind != AiBlockKind::Clean || row >= static_cast<int>(b.clean.rows.size())) return;
    if (b.id == c.clean_id && c.on_clean) {
        auto fn = c.on_clean;
        fn(s, part, row, f);
    } else if (part == ui::AiPanelPart::CleanLink) {
        // A settled card still unfolds its list.
        ui::AiCleanRowView& r = b.clean.rows[static_cast<size_t>(row)];
        r.open = !r.open && !r.files.empty();
    }
    Redraw(s);
}

void AiChatDup(AppState& s, ui::AiPanelPart part, int index) {
    AiChat& c = Chat(s);
    if (index < 0) return;
    const bool file = part == ui::AiPanelPart::DupFile;
    const int block = file ? index / 64 : index;
    if (block >= static_cast<int>(c.blocks.size())) return;
    const AiBlockView& b = c.blocks[static_cast<size_t>(block)];
    if (b.kind != AiBlockKind::Dup || b.id != c.dup_id || !c.on_dup) return;
    auto fn = c.on_dup;
    fn(s, part, file ? (index % 64) / 8 : -1, file ? index % 8 : -1);
    Redraw(s);
}

} // namespace pulse
