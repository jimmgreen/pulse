// System context menu registry: enumeration, CMM markers, enable/disable round
// trips and batch validation, all against a scratch key under HKCU.

#include "../app/system_ctx_registry.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace pulse::app::sysctx;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

std::wstring g_base;

void SetString(const std::wstring& rel, const wchar_t* name, const std::wstring& value) {
    HKEY k = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k,
                    nullptr);
    if (!k) return;
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

void SetDword(const std::wstring& rel, const wchar_t* name, DWORD value) {
    HKEY k = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k,
                    nullptr);
    if (!k) return;
    RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(k);
}

bool HasValue(const std::wstring& rel, const wchar_t* name) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    const bool has = RegQueryValueExW(k, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return has;
}

DWORD GetDword(const std::wstring& rel, const wchar_t* name) {
    HKEY k = nullptr;
    DWORD v = 0, size = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    RegQueryValueExW(k, name, nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &size);
    RegCloseKey(k);
    return v;
}

std::wstring GetString(const std::wstring& rel, const wchar_t* name) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return {};
    wchar_t buf[256] = {};
    DWORD type = 0, size = sizeof(buf) - sizeof(wchar_t);
    const LSTATUS st = RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(k);
    return st == ERROR_SUCCESS && type == REG_SZ ? std::wstring(buf) : std::wstring();
}

bool KeyExists(const std::wstring& rel) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + rel).c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    RegCloseKey(k);
    return true;
}

const Item* Find(const std::vector<Item>& items, const std::wstring& key_name) {
    for (const auto& it : items) if (it.key_name == key_name) return &it;
    return nullptr;
}

std::wstring Snapshot(const Roots& roots) {
    // Export the whole scratch tree through one fake op per layer root.
    std::wstring out;
    for (const wchar_t* layer : {L"User", L"Machine"}) {
        Roots r = roots;
        r.user_classes = g_base + L"\\" + layer;
        Op all;
        all.root = Root::User;
        all.kind = Kind::Verb;
        all.key_path = L"";
        // ExportReg skips nothing for an empty path: it exports the layer root.
        out += ExportReg(r, {all});
    }
    return out;
}

// Read-only: lists what Settings would show for the real registry.
int Scan(const wchar_t* out_path) {
    const Roots roots;
    std::vector<Scene> scenes = BuiltinScenes();
    for (const wchar_t* ext : {L".png", L".jpg", L".mp4", L".dwg", L".zip", L".pdf", L".txt", L".docx"})
        scenes.push_back(ExtensionScene(roots, ext));
    for (const wchar_t* type : {L"image", L"audio", L"text", L"video"}) scenes.push_back(PerceivedScene(type));
    std::wstring out;
    size_t total = 0, disabled = 0, missing = 0;
    const ULONGLONG start = GetTickCount64();
    for (const Scene& scene : scenes) {
        for (const Item& it : Enumerate(roots, scene)) {
            ++total;
            disabled += it.enabled ? 0 : 1;
            missing += it.missing ? 1 : 0;
            static const wchar_t* kinds[] = {L"verb", L"shellex", L"dragdrop", L"shellnew", L"sendto", L"openwith"};
            out += scene.id + L"\t" + kinds[static_cast<int>(it.kind)] + L"\t" +
                   (it.root == Root::User ? L"HKCU" : it.root == Root::Machine ? L"HKLM" : L"file") + L"\t" +
                   it.key_path + L"\t" + (it.enabled ? L"on" : L"off") + L"\t" + (it.missing ? L"missing" : L"") +
                   L"\t" + (it.is_protected ? L"prot" : L"") + L"\t" + (it.trusted_installer ? L"ti" : L"") +
                   L"\t" + it.display + L"\t" + it.file + L"\n";
        }
    }
    const ULONGLONG ms = GetTickCount64() - start;
    HANDLE f = CreateFileW(out_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 1;
    const int n = WideCharToMultiByte(CP_UTF8, 0, out.data(), static_cast<int>(out.size()), nullptr, 0, nullptr,
                                      nullptr);
    std::string bytes(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, out.data(), static_cast<int>(out.size()), bytes.data(), n, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(f);
    std::printf("scan: %zu items, %zu disabled, %zu missing, %llu ms\n", total, disabled, missing, ms);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && wcscmp(argv[1], L"--scan") == 0) return Scan(argv[2]);
    g_base = L"Software\\PulseTest\\SysCtx_" + std::to_wstring(GetCurrentProcessId());
    Roots roots;
    roots.user = HKEY_CURRENT_USER;
    roots.user_classes = g_base + L"\\User";
    roots.machine = HKEY_CURRENT_USER;
    roots.machine_classes = g_base + L"\\Machine";
    roots.file_exts = g_base + L"\\FileExts";
    roots.user_trash = g_base + L"\\TrashUser";
    roots.machine_trash = g_base + L"\\TrashMachine";
    roots.check_owner = false;
    roots.resolve_strings = false;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    roots.sendto_dir = std::wstring(tmp) + L"pulse_sysctx_sendto_" + std::to_wstring(GetCurrentProcessId());
    SetVelocitySupportedForTesting(1);

    // ---- fixtures --------------------------------------------------------
    SetString(L"User\\*\\shell\\FooVerb", L"MUIVerb", L"Foo");
    SetString(L"User\\*\\shell\\FooVerb\\command", nullptr, L"\"C:\\pulse_missing_dir\\gone.exe\" \"%1\"");
    SetString(L"User\\*\\shell\\Dup", L"MUIVerb", L"Dup (user)");
    SetString(L"Machine\\*\\shell\\Dup", L"MUIVerb", L"Dup (machine)");
    SetDword(L"Machine\\Directory\\shell\\cmd", L"HideBasedOnVelocityId", 0x639bc8);
    SetString(L"Machine\\Directory\\shell\\cmd\\command", nullptr, L"cmd.exe /s /k pushd \"%V\"");
    SetDword(L"Machine\\Directory\\shell\\BarVerb", L"CommandFlags", 0x28);
    SetString(L"Machine\\Directory\\shell\\BarVerb", L"Extended", L"");
    SetString(L"Machine\\*\\shellex\\ContextMenuHandlers\\Baz", nullptr, L"{11111111-2222-3333-4444-555555555555}");
    SetString(L"Machine\\*\\shellex\\ContextMenuHandlers\\Baz\\Nested", L"Keep", L"me");
    SetString(L"Machine\\CLSID\\{11111111-2222-3333-4444-555555555555}\\InprocServer32", nullptr,
              L"C:\\Windows\\System32\\shell32.dll");
    SetString(L"Machine\\*\\shellex\\-ContextMenuHandlers\\Qux", nullptr, L"{66666666-2222-3333-4444-555555555555}");
    SetString(L"Machine\\Folder\\shell\\open\\command", nullptr, L"explorer.exe");
    SetString(L"Machine\\Folder\\shell\\opennewwindow", L"MUIVerb", L"new window");
    SetString(L"Machine\\Directory\\shellex\\DragDropHandlers\\Zip", nullptr,
              L"{11111111-2222-3333-4444-555555555555}");
    SetString(L"Machine\\Applications\\tool.exe\\shell\\open\\command", nullptr, L"\"C:\\pulse_missing_dir\\tool.exe\" \"%1\"");
    SetString(L"Machine\\Applications\\tool.exe", L"NoOpenWith", L"");
    SetString(L"User\\.pulsetest", nullptr, L"PulseTest.File");
    SetString(L"User\\.pulsetest\\ShellNew", L"NullFile", L"");
    SetString(L"User\\PulseTest.File", nullptr, L"Pulse test file");
    // .png: HKCU key with an empty default still inherits HKLM's ProgID; the
    // user's choice comes first; printto is never listed.
    SetString(L"User\\.pulsepng", L"Content Type", L"image/png");
    SetString(L"Machine\\.pulsepng", nullptr, L"PulsePng.File");
    SetString(L"Machine\\PulsePng.File\\shell\\printto\\command", nullptr, L"print.exe");
    SetString(L"Machine\\PulsePng.File\\shell\\Rotate\\command", nullptr, L"rotate.exe");
    SetString(L"FileExts\\.pulsepng\\UserChoice", L"ProgId", L"PulsePng.Choice");
    SetString(L"User\\PulsePng.Choice\\shell\\Edit\\command", nullptr, L"edit.exe");
    // Inert ShellNew (no creation value), ProgID-scoped ShellNew wins, Folder.
    SetString(L"Machine\\.pulseinert", nullptr, L"Inert.File");
    SetString(L"Machine\\.pulseinert\\Inert.File\\ShellNew", L"ItemName", L"x");
    SetString(L"Machine\\.pulsescoped", nullptr, L"Scoped.File");
    SetString(L"Machine\\.pulsescoped\\ShellNew", L"NullFile", L"");
    SetString(L"Machine\\.pulsescoped\\Scoped.File\\ShellNew", L"FileName", L"template.bin");
    SetString(L"Machine\\Folder\\ShellNew", L"Directory", L"");
    // .docx-style: no default ProgID, but the user's choice has a ShellNew.
    SetString(L"Machine\\.pulsedoc\\Pulse.Doc.12\\ShellNew", L"FileName", L"template.docx");
    SetString(L"Machine\\Pulse.Doc.12", nullptr, L"Pulse document");
    SetString(L"FileExts\\.pulsedoc\\UserChoice", L"ProgId", L"Pulse.Doc.12");
    SetString(L"Machine\\Applications\\Some.ProgId.cpp\\shell\\open\\command", nullptr, L"x.exe");
    // Pulse's 新建 menu: Data bytes, an absolute template, a COM-only creator,
    // a disabled entry.
    SetString(L"Machine\\.pulsedata\\ShellNew", L"Data", L"AB");
    SetString(L"Machine\\.pulsedata\\ShellNew", L"ItemName", L"A data file");
    const std::wstring template_path = std::wstring(tmp) + L"pulse_sysctx_template_" +
                                       std::to_wstring(GetCurrentProcessId()) + L".bin";
    CloseHandle(CreateFileW(template_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    SetString(L"Machine\\.pulsetpl\\ShellNew", L"FileName", template_path);
    SetString(L"Machine\\.pulsetpl\\ShellNew", L"ItemName", L"B template file");
    SetString(L"Machine\\.pulselib\\ShellNew", L"Handler", L"{11111111-2222-3333-4444-555555555555}");
    SetString(L"Machine\\.pulselib\\ShellNew", L"NullFile", L"");
    SetString(L"Machine\\.pulseoff\\-ShellNew", L"NullFile", L"");
    CreateDirectoryW(roots.sendto_dir.c_str(), nullptr);
    const std::wstring hidden_link = roots.sendto_dir + L"\\Hidden target.lnk";
    CloseHandle(CreateFileW(hidden_link.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr));
    CloseHandle(CreateFileW((roots.sendto_dir + L"\\Visible.lnk").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr));

    const Scene file{L"file", {L"*"}};
    const Scene folder{L"folder", {L"Directory", L"Folder"}};

    // ---- S1: enumeration ---------------------------------------------------
    auto files = Enumerate(roots, file);
    const Item* foo = Find(files, L"FooVerb");
    Check(foo && foo->enabled && foo->display == L"Foo" && foo->root == Root::User, "verb: user layer, MUIVerb");
    Check(foo && foo->missing, "verb: command pointing at a deleted program is reported as missing");
    const Item* dup = Find(files, L"Dup");
    int dup_count = 0;
    for (const auto& it : files) if (it.key_name == L"Dup") ++dup_count;
    Check(dup && dup_count == 1 && dup->root == Root::User, "merged view: user layer wins, listed once");
    const Item* baz = Find(files, L"Baz");
    Check(baz && baz->kind == Kind::ShellEx && baz->enabled && baz->root == Root::Machine &&
              baz->clsid == L"{11111111-2222-3333-4444-555555555555}" && !baz->file.empty() && !baz->missing,
          "shellex: CLSID and InprocServer32 resolved");
    const Item* qux = Find(files, L"Qux");
    Check(qux && !qux->enabled && qux->dash_marker && qux->missing, "shellex: -ContextMenuHandlers (CMM) = disabled");

    auto folders = Enumerate(roots, folder);
    const Item* cmd = Find(folders, L"cmd");
    Check(cmd && !cmd->enabled && cmd->velocity_hidden && !cmd->missing, "verb: HideBasedOnVelocityId = disabled");
    const Item* bar = Find(folders, L"BarVerb");
    Check(bar && !bar->enabled && bar->command_flags_hidden && bar->extended, "verb: CommandFlags & 8 = disabled; Extended");
    const Item* open = Find(folders, L"open");
    Check(open && open->is_protected, "protected: Folder open");
    Check(Find(folders, L"Zip") == nullptr, "folder scene does not list drag-drop handlers");

    auto drags = Enumerate(roots, Scene{L"dragdrop", {L"Directory"}, SceneMode::DragDrop});
    Check(Find(drags, L"Zip") && Find(drags, L"Zip")->kind == Kind::DragDrop, "drag-drop scene lists DragDropHandlers");
    auto apps = Enumerate(roots, Scene{L"openwith", {}, SceneMode::OpenWith});
    const Item* tool = Find(apps, L"tool.exe");
    Check(tool && !tool->enabled && tool->no_open_with && tool->missing, "open with: NoOpenWith = disabled");
    auto news = Enumerate(roots, Scene{L"shellnew", {}, SceneMode::ShellNew});
    const Item* sn = Find(news, L".pulsetest");
    Check(sn && sn->enabled && sn->display == L"Pulse test file", "shell new: listed with the ProgID name");
    Check(Find(news, L".pulseinert") == nullptr, "shell new: keys without a creation value are ignored");
    const Item* scoped = Find(news, L".pulsescoped");
    Check(scoped && scoped->key_path == L".pulsescoped\\Scoped.File\\ShellNew", "shell new: .ext\\<ProgID>\\ShellNew wins");
    const Item* doc = Find(news, L".pulsedoc");
    Check(doc && doc->display == L"Pulse document", "shell new: ProgID from the user's choice when there is no default");
    const Item* nf = Find(news, L"Folder");
    Check(nf && nf->is_protected, "shell new: Folder is listed and protected");
    {
        const auto templates = ListNewTemplates(roots);
        const auto find = [&](const wchar_t* ext) -> const NewTemplate* {
            for (const auto& t : templates) if (t.ext == ext) return &t;
            return nullptr;
        };
        const NewTemplate* data = find(L".pulsedata");
        const NewTemplate* tpl = find(L".pulsetpl");
        const NewTemplate* test = find(L".pulsetest");
        Check(data && data->display == L"A data file" && data->data == std::vector<BYTE>{'A', 'B'},
              "new menu: string Data is written as ANSI bytes");
        Check(tpl && tpl->template_file == template_path, "new menu: absolute FileName resolves to the template");
        Check(test && test->null_file && test->display == L"Pulse test file", "new menu: NullFile entry with the ProgID name");
        Check(!find(L".pulselib") && !find(L".pulseoff") && !find(L".pulseinert") && !find(L"Folder"),
              "new menu: COM-only, disabled, inert entries and Folder are left out");
        Check(!find(L".pulsescoped") && !find(L".pulsedoc"), "new menu: templates that resolve to nothing are left out");
        Check(data && tpl && test && data < tpl && tpl < test, "new menu: sorted by menu text");
        NewTemplate lnk;
        lnk.ext = L".lnk";
        lnk.display = L"Shortcut";
        lnk.handler = true;
        Check(CanCreateNewTemplate(lnk), "new menu: .lnk keeps its handler entry (shortcut wizard)");
        DeleteFileW(template_path.c_str());
    }
    Check(Find(apps, L"Some.ProgId.cpp") == nullptr, "open with: only Applications\\*.exe");
    const Scene png = ExtensionScene(roots, L".pulsepng");
    Check(png.class_keys.size() == 3 && png.class_keys[1] == L"PulsePng.Choice" && png.class_keys[2] == L"PulsePng.File",
          "extension scene: user choice, then the inherited ProgID");
    auto pngs = Enumerate(roots, png);
    Check(Find(pngs, L"Edit") && Find(pngs, L"Rotate") && !Find(pngs, L"printto"), "extension scene: printto hidden");
    auto sendto = Enumerate(roots, Scene{L"sendto", {}, SceneMode::SendTo});
    const Item* hid = Find(sendto, L"Hidden target.lnk");
    const Item* vis = Find(sendto, L"Visible.lnk");
    Check(hid && !hid->enabled && vis && vis->enabled, "send to: Hidden attribute = disabled");

    // ---- S2: disable -> enable restores byte for byte -----------------------
    const std::wstring before = Snapshot(roots);
    std::vector<Op> done;
    auto run = [&](const Item* it, Action a) {
        if (!it) return OpResult::NotFound;
        Op op = MakeOp(*it, a);
        PlanOp(roots, op);
        const OpResult r = Apply(roots, op);
        if (r == OpResult::Ok) done.push_back(op);
        return r;
    };
    Check(run(foo, Action::Disable) == OpResult::Ok &&
              HasValue(L"User\\*\\shell\\FooVerb", L"LegacyDisable") &&
              HasValue(L"User\\*\\shell\\FooVerb", L"ProgrammaticAccessOnly") &&
              GetDword(L"User\\*\\shell\\FooVerb", L"HideBasedOnVelocityId") == 0x639bc8,
          "disable verb writes the three CMM markers");
    Check(run(baz, Action::Disable) == OpResult::Ok &&
              !KeyExists(L"Machine\\*\\shellex\\ContextMenuHandlers\\Baz") &&
              KeyExists(L"Machine\\*\\shellex\\-ContextMenuHandlers\\Baz\\Nested"),
          "disable shellex moves the key tree to -ContextMenuHandlers");
    auto after = Enumerate(roots, file);
    Check(Find(after, L"FooVerb") && !Find(after, L"FooVerb")->enabled && Find(after, L"Baz") &&
              !Find(after, L"Baz")->enabled, "enumeration sees the disabled state");
    Check(run(bar, Action::Enable) == OpResult::Ok && GetDword(L"Machine\\Directory\\shell\\BarVerb", L"CommandFlags") == 0x20,
          "enable verb clears CommandFlags bit 8 only");
    Check(run(qux, Action::Enable) == OpResult::Ok && KeyExists(L"Machine\\*\\shellex\\ContextMenuHandlers\\Qux"),
          "enable CMM-disabled shellex moves it back");
    // Undo everything in reverse order.
    bool undo_ok = true;
    for (auto it = done.rbegin(); it != done.rend(); ++it) undo_ok = Apply(roots, Inverse(*it)) == OpResult::Ok && undo_ok;
    Check(undo_ok && Snapshot(roots) == before, "undo restores the registry byte for byte");

    // opennewwindow keeps Win+E: no LegacyDisable.
    const Item* onw = Find(folders, L"opennewwindow");
    Check(onw && Apply(roots, MakeOp(*onw, Action::Disable)) == OpResult::Ok &&
              !HasValue(L"Machine\\Folder\\shell\\opennewwindow", L"LegacyDisable") &&
              HasValue(L"Machine\\Folder\\shell\\opennewwindow", L"ProgrammaticAccessOnly"),
          "Folder opennewwindow is disabled without LegacyDisable");
    if (onw) Apply(roots, MakeOp(*onw, Action::Enable));

    // Position = Top / Bottom for static verbs; undo restores the old placement.
    {
        const std::wstring snap = Snapshot(roots);
        const auto items = Enumerate(roots, file);
        const Item* verb = Find(items, L"FooVerb");
        Op top = verb ? MakePositionOp(*verb, Place::Top) : Op{};
        PlanOp(roots, top);
        const bool wrote = verb && Apply(roots, top) == OpResult::Ok &&
                           GetString(L"User\\*\\shell\\FooVerb", L"Position") == L"Top";
        const auto moved = Enumerate(roots, file);
        const bool seen = Find(moved, L"FooVerb") && Find(moved, L"FooVerb")->position == Place::Top &&
                          Find(moved, L"FooVerb")->enabled;
        Op bottom = MakePositionOp(*Find(moved, L"FooVerb"), Place::Bottom);
        PlanOp(roots, bottom);
        const bool second = Apply(roots, bottom) == OpResult::Ok &&
                            GetString(L"User\\*\\shell\\FooVerb", L"Position") == L"Bottom";
        const bool undone = Apply(roots, Inverse(bottom)) == OpResult::Ok && Apply(roots, Inverse(top)) == OpResult::Ok;
        Check(wrote && seen && second && undone && Snapshot(roots) == snap,
              "position: Top / Bottom written, enumerated, and undone byte for byte");
        Op shellex_pos{Action::Position, Kind::ShellEx, Root::Machine, L"*\\shellex\\ContextMenuHandlers\\Baz"};
        shellex_pos.place = Place::Top;
        Check(Apply(roots, shellex_pos) == OpResult::Unsupported && Snapshot(roots) == snap,
              "position: refused for shellex handlers");
    }

    // 删除残留项: the key moves into the trash whole; Restore puts it back.
    {
        const std::wstring snap = Snapshot(roots);
        const auto items = Enumerate(roots, file);
        const Item* leftover = Find(items, L"FooVerb");  // its command points at a missing exe
        const bool missing = leftover && leftover->missing;
        Op del = leftover ? MakeOp(*leftover, Action::Delete) : Op{};
        PlanOp(roots, del);
        const bool deleted = leftover && Apply(roots, del) == OpResult::Ok && !KeyExists(L"User\\*\\shell\\FooVerb") &&
                             KeyExists(L"TrashUser\\*\\shell\\FooVerb\\command");
        const bool gone_from_scan = !Find(Enumerate(roots, file), L"FooVerb");
        const bool restored = Apply(roots, Inverse(del)) == OpResult::Ok &&
                              KeyExists(L"User\\*\\shell\\FooVerb\\command") && !KeyExists(L"TrashUser\\*");
        Check(missing && deleted && gone_from_scan && restored && Snapshot(roots) == snap,
              "delete leftover verb: into the trash, gone from the scan, restored byte for byte");
        const Item* handler = Find(items, L"Baz");
        Op hdel = handler ? MakeOp(*handler, Action::Delete) : Op{};
        const bool handler_deleted = handler && Apply(roots, hdel) == OpResult::Ok &&
                                     !KeyExists(L"Machine\\*\\shellex\\ContextMenuHandlers\\Baz") &&
                                     KeyExists(L"TrashMachine\\*\\shellex\\ContextMenuHandlers\\Baz\\Nested");
        const bool handler_back = Apply(roots, Inverse(hdel)) == OpResult::Ok;
        Check(handler_deleted && handler_back && Snapshot(roots) == snap && !KeyExists(L"TrashMachine\\*"),
              "delete shellex key with subkeys and put it back");
        Op stray{Action::Restore, Kind::Verb, Root::User, L"*\\shell\\NotInTrash"};
        Op stray_new{Action::Delete, Kind::ShellNew, Root::User, L".pulsetest\\ShellNew"};
        Check(Apply(roots, stray) == OpResult::NotFound && Apply(roots, stray_new) == OpResult::Unsupported &&
                  Snapshot(roots) == snap,
              "restore without a trashed copy reports NotFound; other kinds are refused");
    }

    // A program re-registered its key while an older -ContextMenuHandlers copy
    // was still there. Disabling keeps the key the program wrote now and drops
    // the stale copy instead of blocking on it forever (the batch is exported
    // to .reg first, so the older copy stays in Backups).
    SetString(L"Machine\\*\\shellex\\ContextMenuHandlers\\Clash", nullptr, L"{11111111-2222-3333-4444-555555555555}");
    SetString(L"Machine\\*\\shellex\\-ContextMenuHandlers\\Clash", nullptr, L"{99999999-8888-7777-6666-555555555555}");
    Op clash{Action::Disable, Kind::ShellEx, Root::Machine, L"*\\shellex\\ContextMenuHandlers\\Clash"};
    Check(Apply(roots, clash) == OpResult::Ok &&
              !KeyExists(L"Machine\\*\\shellex\\ContextMenuHandlers\\Clash") &&
              GetString(L"Machine\\*\\shellex\\-ContextMenuHandlers\\Clash", nullptr) ==
                  L"{11111111-2222-3333-4444-555555555555}",
          "shellex disable replaces a stale -ContextMenuHandlers copy with the live key");
    // Enabling again while the live key is back: the disabled copy is a duplicate.
    SetString(L"Machine\\*\\shellex\\ContextMenuHandlers\\Clash", nullptr, L"{11111111-2222-3333-4444-555555555555}");
    Op unclash{Action::Enable, Kind::ShellEx, Root::Machine, L"*\\shellex\\-ContextMenuHandlers\\Clash"};
    Check(Apply(roots, unclash) == OpResult::Ok &&
              KeyExists(L"Machine\\*\\shellex\\ContextMenuHandlers\\Clash") &&
              GetString(L"Machine\\*\\shellex\\ContextMenuHandlers\\Clash", nullptr) ==
                  L"{11111111-2222-3333-4444-555555555555}" &&
              !KeyExists(L"Machine\\*\\shellex\\-ContextMenuHandlers\\Clash"),
          "shellex enable with the live key back drops the duplicate copy");
    Op gone{Action::Disable, Kind::Verb, Root::Machine, L"*\\shell\\DoesNotExist"};
    Check(Apply(roots, gone) == OpResult::NotFound, "missing key reports NotFound");

    // ---- batches for the elevated helper ------------------------------------
    std::vector<Op> batch = {Op{Action::Disable, Kind::Verb, Root::Machine, L"Directory\\Background\\shell\\git_gui", true, 7},
                             Op{Action::Enable, Kind::ShellEx, Root::Machine, L"*\\shellex\\-ContextMenuHandlers\\Qux"}};
    Op place_op{Action::Position, Kind::Verb, Root::Machine, L"Directory\\shell\\git_shell"};
    place_op.place = Place::Bottom;
    std::vector<Op> parsed;
    Check(ParseBatch(SerializeBatch({place_op}), parsed) && parsed.size() == 1 &&
              parsed[0].action == Action::Position && parsed[0].place == Place::Bottom && !parsed[0].planned,
          "batch round trip keeps a position op");
    Check(ParseBatch(SerializeBatch(batch), parsed) && parsed.size() == 2 && parsed[0].key_path == batch[0].key_path &&
              parsed[0].planned && parsed[0].mask == 7 && parsed[1].kind == Kind::ShellEx &&
              parsed[1].action == Action::Enable && !parsed[1].planned,
          "batch round trip keeps the verb plan");
    const wchar_t* bad[] = {
        L"PULSE-SYSCTX 1\ndisable\tverb\tmachine\t..\\..\\Microsoft\\Windows\\shell\\x\n",
        L"PULSE-SYSCTX 1\ndisable\tverb\tmachine\tC:\\x\\shell\\y\n",
        L"PULSE-SYSCTX 1\ndisable\tverb\tmachine\t\\*\\shell\\y\n",
        L"PULSE-SYSCTX 1\ndisable\tverb\tmachine\t*\\shellex\\y\n",
        L"PULSE-SYSCTX 1\ndisable\tshellex\tmachine\t*\\shell\\ContextMenuHandlers\\y\n",
        L"PULSE-SYSCTX 1\nremove\tverb\tmachine\t*\\shell\\y\n",  // delete / restore are real ops now
        L"PULSE-SYSCTX 1\ndisable\tshellnew\tmachine\t.txt\\ShellNew\n",
        L"disable\tverb\tmachine\t*\\shell\\y\n",
        L"PULSE-SYSCTX 1\ndisable\tverb\tmachine\t*\\shell\\y\t99\n",
        L"PULSE-SYSCTX 1\ndisable\tshellex\tmachine\t*\\shellex\\ContextMenuHandlers\\y\t3\n",
        L"PULSE-SYSCTX 1\nposition\tshellex\tmachine\t*\\shellex\\ContextMenuHandlers\\y\ttop\n",
        L"PULSE-SYSCTX 1\nposition\tverb\tmachine\t*\\shell\\y\tleft\n",
        L"PULSE-SYSCTX 1\nposition\tverb\tmachine\t*\\shell\\y\n",
    };
    bool all_rejected = true;
    for (const auto* b : bad) all_rejected = !ParseBatch(b, parsed) && all_rejected;
    Check(all_rejected, "batch parser rejects traversal, absolute paths, wrong layouts and unknown ops");

    {
        const std::vector<Op> trash_ops = {
            Op{Action::Delete, Kind::ShellEx, Root::Machine, L"*\\shellex\\ContextMenuHandlers\\Gone"},
            Op{Action::Restore, Kind::Verb, Root::Machine, L"*\\shell\\Gone"}};
        std::vector<Op> back;
        const bool round_trip = ParseBatch(SerializeBatch(trash_ops), back) && back.size() == 2 &&
                                back[0].action == Action::Delete && back[1].action == Action::Restore &&
                                back[0].key_path == trash_ops[0].key_path;
        Check(round_trip && !ParseBatch(L"PULSE-SYSCTX 1\ndelete\tverb\tmachine\t*\\shell\\x\t3\n", back) &&
                  !ParseBatch(L"PULSE-SYSCTX 1\ndelete\tverb\tmachine\t..\\shell\\x\n", back),
              "batch: delete / restore round trip; masks and traversal refused");
    }
    std::vector<OpResult> results;
    Check(ParseResults(SerializeResults({OpResult::Ok, OpResult::Conflict, OpResult::AccessDenied}), results) &&
              results.size() == 3 && results[1] == OpResult::Conflict && results[2] == OpResult::AccessDenied &&
              !ParseResults(L"PULSE-SYSCTX-RESULT 1\nmaybe\n", results) && !ParseResults(L"ok\n", results),
          "helper result file round trip");
    // The helper refuses per-user ops and malformed input before touching anything.
    wchar_t batch_path[MAX_PATH], result_path[MAX_PATH];
    GetTempFileNameW(tmp, L"psc", 0, batch_path);
    GetTempFileNameW(tmp, L"psr", 0, result_path);
    {
        HANDLE f = CreateFileW(batch_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        const char user_batch[] = "PULSE-SYSCTX 1\ndisable\tverb\tuser\t*\\shell\\FooVerb\n";
        DWORD written = 0;
        WriteFile(f, user_batch, sizeof(user_batch) - 1, &written, nullptr);
        CloseHandle(f);
    }
    Check(RunElevatedBatch(batch_path, result_path) == 2 && RunElevatedBatch(L"Z:\\no\\such.txt", result_path) == 2,
          "elevated helper rejects user-root and unreadable batches");
    DeleteFileW(batch_path);
    DeleteFileW(result_path);

    const std::wstring reg = ExportReg(roots, {Op{Action::Disable, Kind::Verb, Root::Machine, L"Directory\\shell\\BarVerb"}});
    Check(reg.rfind(L"Windows Registry Editor Version 5.00", 0) == 0 &&
              reg.find(L"Directory\\shell\\BarVerb]") != std::wstring::npos &&
              reg.find(L"\"CommandFlags\"=dword:00000028") != std::wstring::npos &&
              reg.find(L"\"Extended\"=\"\"") != std::wstring::npos,
          "ExportReg writes REGEDIT5 text");

    // ---- cleanup -------------------------------------------------------------
    RegDeleteTreeW(HKEY_CURRENT_USER, g_base.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\PulseTest");  // only succeeds when empty
    SetFileAttributesW(hidden_link.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(hidden_link.c_str());
    DeleteFileW((roots.sendto_dir + L"\\Visible.lnk").c_str());
    RemoveDirectoryW(roots.sendto_dir.c_str());
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
