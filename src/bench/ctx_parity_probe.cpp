// ctx_parity_probe.cpp - Explorer parity check for Pulse's right-click menu.
//
//   pulse_ctx_parity_probe [--extended] [--report <file>] <path> [<path> ...]
//
// For each path, the classic menu Explorer itself builds (DefCM, every flyout
// initialised) is compared row by row with what Pulse's own pipeline yields:
// the extensions pulse_shell would load (EnumerateCtxHandlers, then
// QueryOneHandler + CollectHandlerItems on one STA worker each, as the host
// does) plus the registry verbs (EnumerateStaticVerbs). Context-menu settings
// are not applied: this checks what Pulse *can* show, not what a user turned
// off.
//
// [FAIL] when a native row has no Pulse source (never loaded, or dropped while
// collecting), or when Pulse yields a row Explorer does not show. It reads the
// real machine's registry and runs the real extensions, so it is a regression
// / diagnosis tool to run on a configured desktop, not a hermetic unit test.
// --report writes the evidence: association, registry keys, both menus,
// per-extension timings and the row-by-row diff.
#include "../shell_host/ctx_handlers.h"
#include "../app/shell_verbs.h"
#include "../ipc/ctx_menu_util.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;
FILE* g_report = nullptr;

void Check(bool ok, const std::wstring& label) {
    wprintf(L"[%ls] %ls\n", ok ? L"PASS" : L"FAIL", label.c_str());
    if (!ok) ++failures;
}

void Info(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fputws(L"[INFO] ", stdout);
    vfwprintf(stdout, fmt, args);
    va_end(args);
}

void Out(const wchar_t* fmt, ...) {
    if (!g_report) return;
    va_list args;
    va_start(args, fmt);
    vfwprintf(g_report, fmt, args);
    va_end(args);
}

std::wstring Lower(std::wstring text) {
    for (auto& ch : text) ch = static_cast<wchar_t>(towlower(ch));
    return text;
}

// Row identity across the two menus: cleaned text without ellipsis / spaces.
std::wstring MatchKey(const std::wstring& text) {
    const std::wstring clean = pulse::ipc::CleanMenuText(text);
    std::wstring key;
    for (wchar_t ch : clean) {
        if (ch == L'.' || ch == 0x2026 || ch == L' ' || ch == L'\t') continue;
        key += static_cast<wchar_t>(towlower(ch));
    }
    return key;
}

std::wstring RegString(HKEY root, const std::wstring& sub, const wchar_t* value) {
    wchar_t buffer[1024]{};
    DWORD bytes = sizeof(buffer) - sizeof(wchar_t);
    if (RegGetValueW(root, sub.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     nullptr, buffer, &bytes) != ERROR_SUCCESS)
        return {};
    return buffer;
}

std::vector<std::wstring> SubKeys(HKEY root, const std::wstring& sub) {
    std::vector<std::wstring> names;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return names;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]{};
        DWORD cch = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &cch, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        names.emplace_back(name);
    }
    RegCloseKey(key);
    return names;
}

void DumpClassKey(const std::wstring& cls) {
    const auto verbs = SubKeys(HKEY_CLASSES_ROOT, cls + L"\\shell");
    const auto handlers = SubKeys(HKEY_CLASSES_ROOT, cls + L"\\shellex\\ContextMenuHandlers");
    if (verbs.empty() && handlers.empty()) return;
    Out(L"  HKCR\\%ls\n", cls.c_str());
    for (const auto& verb : verbs) {
        const std::wstring sub = cls + L"\\shell\\" + verb;
        Out(L"    shell\\%ls  \"%ls\"%ls\n", verb.c_str(), RegString(HKEY_CLASSES_ROOT, sub, nullptr).c_str(),
            RegString(HKEY_CLASSES_ROOT, sub, L"AppliesTo").empty() ? L"" : L"  (AppliesTo)");
    }
    for (const auto& handler : handlers) {
        Out(L"    shellex\\%ls  %ls\n", handler.c_str(),
            RegString(HKEY_CLASSES_ROOT, cls + L"\\shellex\\ContextMenuHandlers\\" + handler, nullptr).c_str());
    }
}

struct Row {
    int depth = 0;
    std::wstring text;
    std::wstring verb;
    bool disabled = false;
    bool submenu = false;
    std::wstring source;
};

// Extensions are foreign code; a fault in one must not end the probe.
bool SafeVerb(IContextMenu* menu, UINT offset, wchar_t* buffer, UINT cch) {
    __try {
        return SUCCEEDED(menu->GetCommandString(offset, GCS_VERBW, nullptr,
                                                reinterpret_cast<CHAR*>(buffer), cch));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SafeInitPopup(IContextMenu3* menu3, IContextMenu2* menu2, HMENU sub, UINT pos) {
    __try {
        LRESULT result = 0;
        if (menu3)
            menu3->HandleMenuMsg2(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(sub), MAKELPARAM(pos, FALSE), &result);
        else if (menu2)
            menu2->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(sub), MAKELPARAM(pos, FALSE));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

std::wstring VerbOf(IContextMenu* menu, UINT id, UINT first) {
    if (id < first || id - first > 0x7FFF) return {};
    wchar_t buffer[256]{};
    return SafeVerb(menu, id - first, buffer, ARRAYSIZE(buffer) - 1) ? std::wstring(buffer) : std::wstring();
}

void InitPopup(IContextMenu* menu, HMENU sub, UINT pos) {
    IContextMenu3* menu3 = nullptr;
    IContextMenu2* menu2 = nullptr;
    menu->QueryInterface(IID_PPV_ARGS(&menu3));
    if (!menu3) menu->QueryInterface(IID_PPV_ARGS(&menu2));
    SafeInitPopup(menu3, menu2, sub, pos);
    if (menu3) menu3->Release();
    if (menu2) menu2->Release();
}

// Explorer's menu loop keeps pumping while a flyout is open, and 发送到 /
// 包含到库中 fill from posted messages; the reference does the same until the
// flyout has been unchanged for 400 ms (2 s at most).
void SettleFlyout(HMENU sub) {
    int last = GetMenuItemCount(sub);
    ULONGLONG changed = GetTickCount64();
    const ULONGLONG cap = changed + 2000;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= cap || now - changed >= 400) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const int count = GetMenuItemCount(sub);
        if (count != last) {
            last = count;
            changed = GetTickCount64();
        }
    }
}

// Full, unbudgeted walk of a built HMENU (flyouts initialised, 3 levels).
void Walk(IContextMenu* menu, HMENU hmenu, UINT first, int depth, const std::wstring& source,
          std::vector<Row>& rows) {
    const int count = GetMenuItemCount(hmenu);
    for (int i = 0; i < count; ++i) {
        wchar_t text[512]{};
        MENUITEMINFOW info{ sizeof(info) };
        info.fMask = MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU | MIIM_STRING;
        info.dwTypeData = text;
        info.cch = ARRAYSIZE(text) - 1;
        if (!GetMenuItemInfoW(hmenu, static_cast<UINT>(i), TRUE, &info)) continue;
        if (info.fType & MFT_SEPARATOR) continue;
        Row row;
        row.depth = depth;
        row.text = pulse::ipc::CleanMenuText(text);
        row.verb = VerbOf(menu, info.wID, first);
        row.disabled = (info.fState & (MFS_DISABLED | MFS_GRAYED)) != 0;
        row.submenu = info.hSubMenu != nullptr;
        row.source = source;
        rows.push_back(row);
        if (info.hSubMenu && depth < 3) {
            InitPopup(menu, info.hSubMenu, static_cast<UINT>(i));
            if (source == L"native") SettleFlyout(info.hSubMenu);
            Walk(menu, info.hSubMenu, first, depth + 1, source, rows);
        }
    }
}

void PrintRows(const std::vector<Row>& rows) {
    for (const auto& row : rows) {
        Out(L"  %ls%ls%ls  [verb=%ls]%ls\n", std::wstring(static_cast<size_t>(row.depth) * 4, L' ').c_str(),
            row.submenu ? L"> " : L"", row.text.empty() ? L"(no text)" : row.text.c_str(), row.verb.c_str(),
            row.disabled ? L" DISABLED" : L"");
    }
}

std::vector<Row> NativeMenu(const std::wstring& path, UINT flags, ULONGLONG& query_ms) {
    std::vector<Row> rows;
    PIDLIST_ABSOLUTE pidl = nullptr;
    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    IContextMenu* menu = nullptr;
    const ULONGLONG started = GetTickCount64();
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) &&
        SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child)) &&
        SUCCEEDED(parent->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr,
                                        reinterpret_cast<void**>(&menu)))) {
        HMENU hmenu = CreatePopupMenu();
        menu->QueryContextMenu(hmenu, 0, 1, 0x7FFF, flags | CMF_CANRENAME);
        query_ms = GetTickCount64() - started;
        Walk(menu, hmenu, 1, 0, L"native", rows);
        DestroyMenu(hmenu);
        menu->Release();
    }
    if (parent) parent->Release();
    if (pidl) CoTaskMemFree(pidl);
    return rows;
}

// One extension on its own STA thread, the way pulse_shell's workers run it.
struct Worker {
    pulse::shell::CtxHandlerDesc desc;
    std::wstring path;
    UINT first = 0;
    UINT last = 0;
    UINT flags = 0;
    HANDLE done = nullptr;
    HRESULT hr = E_PENDING;
    ULONGLONG total_ms = 0;
    std::vector<pulse::shell::CtxItemOut> kept; // CollectHandlerItems
    std::vector<Row> full;                      // everything the extension built
};

DWORD WINAPI WorkerThread(LPVOID param) {
    auto* worker = static_cast<Worker*>(param);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const ULONGLONG started = GetTickCount64();
    {
        pulse::shell::CtxBind bind;
        pulse::shell::CtxHandlerSlot slot;
        if (pulse::shell::BindCtxSelection({ worker->path }, false, bind)) {
            worker->hr = pulse::shell::QueryOneHandler(worker->desc, bind, nullptr, worker->first,
                                                       worker->last, worker->flags, slot);
            if (SUCCEEDED(worker->hr) && slot.menu) {
                pulse::shell::CollectHandlerItems(slot, false, worker->kept);
                worker->total_ms = GetTickCount64() - started;
                Walk(slot.menu, slot.hmenu, worker->first, 0, worker->desc.name, worker->full);
            }
            pulse::shell::ReleaseHandlerSlot(slot);
        } else {
            worker->hr = E_NOINTERFACE;
        }
    }
    if (!worker->total_ms) worker->total_ms = GetTickCount64() - started;
    SetEvent(worker->done);
    CoUninitialize();
    return 0;
}

const Row* FindRowIn(const std::vector<Row>& rows, const Row& wanted, bool default_menu) {
    const std::wstring key = MatchKey(wanted.text);
    auto eligible = [&](const Row& row) { return (row.source == L"default menu") == default_menu; };
    if (!key.empty())
        for (const auto& row : rows)
            if (eligible(row) && MatchKey(row.text) == key) return &row;
    if (!wanted.verb.empty())
        for (const auto& row : rows)
            if (eligible(row) && !row.verb.empty() && _wcsicmp(row.verb.c_str(), wanted.verb.c_str()) == 0)
                return &row;
    return nullptr;
}

// An extension's own row is a better culprit than the default-menu copy.
const Row* FindExposed(const std::vector<Row>& rows, const Row& wanted) {
    const Row* own = FindRowIn(rows, wanted, false);
    return own ? own : FindRowIn(rows, wanted, true);
}

const Row* FindRow(const std::vector<Row>& rows, const Row& wanted) {
    const std::wstring key = MatchKey(wanted.text);
    if (!key.empty())
        for (const auto& row : rows)
            if (MatchKey(row.text) == key) return &row;
    if (!wanted.verb.empty())
        for (const auto& row : rows)
            if (!row.verb.empty() && _wcsicmp(row.verb.c_str(), wanted.verb.c_str()) == 0) return &row;
    return nullptr;
}

std::wstring StaticVerbKey(const std::wstring& path, DWORD attributes) {
    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
        return PathIsRootW(path.c_str()) ? std::wstring(pulse::ipc::kDriveVerbKey)
                                         : std::wstring(pulse::ipc::kFolderVerbKey);
    const wchar_t* dot = PathFindExtensionW(path.c_str());
    return Lower(dot ? dot : L"");
}

void DumpAssociation(const std::wstring& ext) {
    const std::wstring file_exts =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + ext;
    const std::wstring user_choice = RegString(HKEY_CURRENT_USER, file_exts + L"\\UserChoice", L"ProgId");
    const std::wstring hkcr_default = RegString(HKEY_CLASSES_ROOT, ext, nullptr);
    const std::wstring perceived = RegString(HKEY_CLASSES_ROOT, ext, L"PerceivedType");
    wchar_t progid[256]{};
    DWORD cch = ARRAYSIZE(progid);
    AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_PROGID, ext.c_str(), nullptr, progid, &cch);
    Out(L"## Association\n  AssocQueryString(PROGID) = %ls\n  UserChoice = %ls\n  HKCR\\%ls = %ls\n"
        L"  PerceivedType = %ls\n\n## Registry\n",
        progid, user_choice.c_str(), ext.c_str(), hkcr_default.c_str(), perceived.c_str());
    std::vector<std::wstring> classes;
    if (progid[0]) classes.emplace_back(progid);
    classes.push_back(L"SystemFileAssociations\\" + ext);
    if (!perceived.empty()) classes.push_back(L"SystemFileAssociations\\" + perceived);
    classes.emplace_back(L"*");
    classes.emplace_back(L"AllFilesystemObjects");
    for (const auto& cls : classes) DumpClassKey(cls);
}

void ProbePath(const std::wstring& path, bool extended) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        Check(false, path + L": path exists");
        return;
    }
    const UINT flags = CMF_NORMAL | (extended ? CMF_EXTENDEDVERBS : 0);
    const std::wstring verb_key = StaticVerbKey(path, attributes);
    Out(L"\n\n# %ls  (static verb key %ls, extended=%d)\n\n", path.c_str(), verb_key.c_str(), extended ? 1 : 0);
    if (!verb_key.empty() && verb_key[0] == L'.') DumpAssociation(verb_key);

    ULONGLONG native_ms = 0;
    const auto native = NativeMenu(path, flags, native_ms);
    Out(L"\n## Explorer classic menu: %zu rows, QueryContextMenu %llu ms\n", native.size(), native_ms);
    PrintRows(native);
    if (native.empty()) {
        Check(false, path + L": Explorer builds a menu for the path");
        return;
    }

    // Every extension at once, like the host; hung ones are left behind.
    const auto handlers = pulse::shell::EnumerateCtxHandlers(false, path, {});
    std::vector<std::unique_ptr<Worker>> workers;
    UINT next_id = 0x1000;
    const ULONGLONG started = GetTickCount64();
    for (const auto& handler : handlers) {
        auto worker = std::make_unique<Worker>();
        worker->desc = handler;
        worker->path = path;
        worker->first = next_id;
        worker->last = next_id + 255;
        worker->flags = flags;
        next_id += 256;
        worker->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (HANDLE thread = CreateThread(nullptr, 0, WorkerThread, worker.get(), 0, nullptr))
            CloseHandle(thread);
        else
            SetEvent(worker->done);
        workers.push_back(std::move(worker));
    }
    constexpr ULONGLONG kWaitMs = 15000;
    for (auto& worker : workers) {
        const ULONGLONG elapsed = GetTickCount64() - started;
        WaitForSingleObject(worker->done, elapsed >= kWaitMs ? 0 : static_cast<DWORD>(kWaitMs - elapsed));
    }

    std::vector<Row> kept;
    std::vector<Row> exposed;
    int over_budget = 0;
    int hung = 0;
    ULONGLONG slowest = 0;
    std::wstring slowest_name;
    Out(L"\n## Pulse extensions: %zu\n", workers.size());
    for (auto& worker : workers) {
        const bool finished = WaitForSingleObject(worker->done, 0) == WAIT_OBJECT_0;
        if (!finished) {
            ++hung;
            Out(L"  %ls %ls  STILL RUNNING after %llu ms\n", worker->desc.name.c_str(),
                worker->desc.clsid_text.c_str(), kWaitMs);
            continue;
        }
        if (worker->total_ms > 80) ++over_budget;
        if (worker->total_ms > slowest) {
            slowest = worker->total_ms;
            slowest_name = worker->desc.name;
        }
        Out(L"  %ls %ls%ls  hr=0x%08X  %llu ms  kept=%zu built=%zu\n", worker->desc.name.c_str(),
            worker->desc.clsid_text.c_str(), worker->desc.explorer_command ? L" (IExplorerCommand)" : L"",
            static_cast<unsigned>(worker->hr), worker->total_ms, worker->kept.size(), worker->full.size());
        for (const auto& item : worker->kept) {
            Row row;
            row.depth = item.child ? 1 : 0;
            row.text = item.text;
            row.verb = item.verb;
            row.disabled = !item.enabled;
            row.submenu = item.has_children;
            row.source = worker->desc.name;
            kept.push_back(row);
        }
        // The SendTo slot holds the whole default Shell menu; a row found only
        // there is Explorer's own, not the Send to extension's.
        const bool default_menu =
            _wcsicmp(worker->desc.clsid_text.c_str(), L"{7ba4c740-9e81-11cf-99d3-00aa004ae837}") == 0;
        for (auto row : worker->full) {
            if (default_menu) row.source = L"default menu";
            exposed.push_back(std::move(row));
        }
    }

    std::vector<Row> statics;
    for (const auto& verb : pulse::app::EnumerateStaticVerbs(verb_key)) {
        Row row;
        row.text = verb.display;
        row.verb = verb.verb;
        row.source = L"static";
        statics.push_back(row);
        for (const auto& child : verb.children) {
            Row sub;
            sub.depth = 1;
            sub.text = child.display;
            sub.verb = child.verb;
            sub.source = L"static";
            statics.push_back(sub);
        }
    }
    Out(L"\n## Pulse registry verbs (%ls): %zu\n", verb_key.c_str(), statics.size());
    PrintRows(statics);

    // Native -> Pulse.
    Out(L"\n## Explorer rows -> Pulse source\n");
    std::vector<std::wstring> missing;
    for (const auto& row : native) {
        if (row.text.empty()) continue;
        const wchar_t* status = nullptr;
        std::wstring source;
        if (const Row* hit = FindRow(statics, row)) {
            status = L"OK-static";
            source = hit->source;
        } else if (const Row* com = FindRow(kept, row)) {
            status = L"OK-com";
            source = com->source;
        } else if (pulse::ipc::IsBuiltinContextVerb(row.verb, false)) {
            status = L"BUILTIN";
        } else if (const Row* dropped = FindExposed(exposed, row)) {
            status = L"LOST-IN-COLLECT";
            source = dropped->source;
            missing.push_back(row.text + L" (dropped from " + source + L")");
        } else {
            status = L"NOT-LOADED";
            missing.push_back(row.text + (row.verb.empty() ? L"" : L" [" + row.verb + L"]"));
        }
        Out(L"  %-16ls %ls%ls [verb=%ls]%ls%ls\n", status,
            std::wstring(static_cast<size_t>(row.depth) * 4, L' ').c_str(), row.text.c_str(), row.verb.c_str(),
            source.empty() ? L"" : L"  <- ", source.c_str());
    }

    // Pulse -> native. "__openwith" rows are Pulse's own recent-apps option.
    Out(L"\n## Pulse rows Explorer does not show\n");
    std::vector<std::wstring> extra;
    auto collect_extra = [&](const std::vector<Row>& rows) {
        for (const auto& row : rows) {
            if (row.text.empty() || row.verb == L"__openwith") continue;
            if (FindRow(native, row)) continue;
            extra.push_back(row.text + L" <- " + row.source);
            Out(L"  EXTRA %ls [verb=%ls]  <- %ls\n", row.text.c_str(), row.verb.c_str(), row.source.c_str());
        }
    };
    collect_extra(statics);
    collect_extra(kept);

    Out(L"\n  summary: native=%zu missing=%zu extra=%zu extensions=%zu over80ms=%d hung=%d\n",
        native.size(), missing.size(), extra.size(), workers.size(), over_budget, hung);
    Info(L"%ls: Explorer %zu rows (QCM %llu ms); %zu extensions, %d over 80 ms, slowest %ls %llu ms, %d hung\n",
         path.c_str(), native.size(), native_ms, workers.size(), over_budget, slowest_name.c_str(), slowest, hung);
    for (const auto& text : missing) Info(L"  missing: %ls\n", text.c_str());
    for (const auto& text : extra) Info(L"  extra:   %ls\n", text.c_str());
    Check(missing.empty(), path + L": every Explorer row has a Pulse source");
    Check(extra.empty(), path + L": Pulse adds no row Explorer does not show");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    bool extended = false;
    std::wstring report;
    std::vector<std::wstring> paths;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--extended") extended = true;
        else if (arg == L"--report" && i + 1 < argc) report = argv[++i];
        else paths.push_back(arg);
    }
    if (paths.empty()) {
        fwprintf(stderr, L"usage: pulse_ctx_parity_probe [--extended] [--report <file>] <path>...\n");
        return 2;
    }
    if (!report.empty() && (_wfopen_s(&g_report, report.c_str(), L"w, ccs=UTF-8") != 0 || !g_report)) {
        fwprintf(stderr, L"cannot write %ls\n", report.c_str());
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (const auto& path : paths) {
        wchar_t full[MAX_PATH * 2]{};
        const DWORD length = GetFullPathNameW(path.c_str(), ARRAYSIZE(full), full, nullptr);
        ProbePath(length && length < ARRAYSIZE(full) ? std::wstring(full) : path, extended);
    }
    if (g_report) fclose(g_report);
    fflush(stdout);
    // Extensions that hang (or crash) on unload must not decide the result:
    // leave without tearing their threads down.
    TerminateProcess(GetCurrentProcess(), failures ? 1u : 0u);
    return failures ? 1 : 0;
}
