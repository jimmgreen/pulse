#include "system_ctx_registry.h"
#include "../common/os_version.h"

#include <aclapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <sddl.h>

#include <algorithm>
#include <cwctype>
#include <set>
#include <utility>

namespace pulse::app::sysctx {
namespace {

constexpr DWORD kVelocityHideId = 0x639bc8;  // the value ContextMenuManager writes
constexpr wchar_t kBatchHeader[] = L"PULSE-SYSCTX 1";
int g_velocity_override = -1;

struct Key {
    HKEY h = nullptr;
    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    ~Key() { if (h) RegCloseKey(h); }
    explicit operator bool() const { return h != nullptr; }
};

std::wstring Lower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

bool IEquals(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                                         b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring Join(const std::wstring& a, std::wstring_view b) {
    if (a.empty()) return std::wstring(b);
    if (b.empty()) return a;
    return a + L"\\" + std::wstring(b);
}

LSTATUS Open(HKEY root, const std::wstring& path, REGSAM access, Key& key) {
    return RegOpenKeyExW(root, path.c_str(), 0, access, &key.h);
}

bool ReadString(HKEY key, const wchar_t* name, std::wstring& out) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &bytes) != ERROR_SUCCESS)
        return false;
    buf.resize(wcsnlen(buf.c_str(), buf.size()));
    if (type == REG_EXPAND_SZ) {
        wchar_t expanded[MAX_PATH * 2];
        const DWORD n = ExpandEnvironmentStringsW(buf.c_str(), expanded, ARRAYSIZE(expanded));
        if (n > 0 && n <= ARRAYSIZE(expanded)) buf = expanded;
    }
    out = std::move(buf);
    return true;
}

// Unknown Position strings count as the default placement, as in Explorer.
Place ReadPlace(HKEY v) {
    std::wstring text;
    if (!ReadString(v, L"Position", text)) return Place::Default;
    if (IEquals(text, L"Top")) return Place::Top;
    if (IEquals(text, L"Bottom")) return Place::Bottom;
    return Place::Default;
}

bool HasValue(HKEY key, const wchar_t* name) {
    return RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

bool ReadDword(HKEY key, const wchar_t* name, DWORD& out) {
    DWORD type = 0, bytes = sizeof(out);
    return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&out), &bytes) == ERROR_SUCCESS &&
           type == REG_DWORD;
}

std::vector<std::wstring> Subkeys(HKEY key) {
    std::vector<std::wstring> out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = ARRAYSIZE(name);
        const LSTATUS st = RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st == ERROR_SUCCESS) out.emplace_back(name, len);
        else if (st != ERROR_MORE_DATA) break;
    }
    return out;
}

HKEY RootKey(const Roots& r, Root root) { return root == Root::Machine ? r.machine : r.user; }
const std::wstring& RootBase(const Roots& r, Root root) {
    return root == Root::Machine ? r.machine_classes : r.user_classes;
}
std::wstring FullPath(const Roots& r, Root root, std::wstring_view rel) { return Join(RootBase(r, root), rel); }

std::wstring ResolveIndirect(const Roots& roots, const std::wstring& s) {
    if (!roots.resolve_strings || s.empty() || s[0] != L'@') return s;
    wchar_t buf[512];
    if (SUCCEEDED(SHLoadIndirectString(s.c_str(), buf, ARRAYSIZE(buf), nullptr))) return buf;
    return s;
}

std::wstring StripMnemonic(std::wstring s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'&') {
            if (i + 1 < s.size() && s[i + 1] == L'&') { out += L'&'; ++i; }
            continue;
        }
        out += s[i];
    }
    return out;
}

bool LooksLikeGuid(std::wstring_view s) {
    return s.size() == 38 && s.front() == L'{' && s.back() == L'}';
}

// First token of a command line or an Icon value, with environment expanded.
std::wstring RawPath(std::wstring c) {
    while (!c.empty() && std::iswspace(c.front())) c.erase(c.begin());
    if (c.empty()) return {};
    wchar_t expanded[MAX_PATH * 2];
    const DWORD n = ExpandEnvironmentStringsW(c.c_str(), expanded, ARRAYSIZE(expanded));
    if (n > 0 && n <= ARRAYSIZE(expanded)) c = expanded;
    std::wstring path;
    if (c.front() == L'"') {
        const size_t end = c.find(L'"', 1);
        path = c.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
    } else {
        const std::wstring lower = Lower(c);
        size_t cut = std::wstring::npos;
        for (const wchar_t* ext : {L".exe", L".dll", L".ico", L".cpl"}) {
            const size_t at = lower.find(ext);
            if (at != std::wstring::npos) cut = (std::min)(cut, at + 4);
        }
        path = cut != std::wstring::npos ? c.substr(0, cut) : c.substr(0, c.find(L' '));
    }
    if (!path.empty() && path[0] == L'@') path.erase(0, 1);
    const size_t comma = path.rfind(L',');
    if (comma != std::wstring::npos && comma > 2) {
        bool digits = comma + 1 < path.size();
        for (size_t i = comma + 1; i < path.size(); ++i)
            if (!(std::iswdigit(path[i]) || (i == comma + 1 && path[i] == L'-'))) digits = false;
        if (digits) path.resize(comma);
    }
    return path;
}

bool IsAbsolute(const std::wstring& p) {
    return p.size() > 2 && std::iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/');
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Resolves bare names ("notepad.exe", "shell32.dll") through the search path.
std::wstring ResolveFile(const std::wstring& raw) {
    if (raw.empty()) return {};
    if (IsAbsolute(raw)) return FileExists(raw) ? raw : std::wstring{};
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, raw.c_str(), nullptr, ARRAYSIZE(found), found, nullptr) > 0) return found;
    return {};
}

bool PointsAtMissingFile(const std::wstring& value) {
    const std::wstring raw = RawPath(value);
    return IsAbsolute(raw) && !FileExists(raw);
}

bool IsProtectedVerb(std::wstring_view verb) {
    static const wchar_t* const kVerbs[] = {L"open", L"opennewwindow", L"opennewprocess", L"opennewtab",
                                            L"explore", L"runas", L"runasuser", L"properties", L"find"};
    for (const auto* v : kVerbs) if (IEquals(verb, v)) return true;
    return false;
}

bool IsProtectedClsid(std::wstring_view clsid) {
    static const wchar_t* const kClsids[] = {
        L"{09799AFB-AD67-11d1-ABCD-00C04FC30936}",  // Open With
        L"{D969A300-E7FF-11d0-A93B-00A0C90F2719}",  // New
        L"{37ea3a21-7493-4208-a011-7f9ea79ce9f5}",  // Open containing folder
        L"{7BA4C740-9E81-11CF-99D3-00AA004AE837}",  // Send To
    };
    for (const auto* c : kClsids) if (IEquals(clsid, c)) return true;
    return false;
}

bool OwnedByTrustedInstaller(const Roots& roots, Root root, const std::wstring& rel) {
    if (!roots.check_owner || root != Root::Machine || roots.machine != HKEY_LOCAL_MACHINE) return false;
    const std::wstring object = L"MACHINE\\" + FullPath(roots, root, rel);
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(object.c_str(), SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION, &owner, nullptr,
                              nullptr, nullptr, &sd) != ERROR_SUCCESS) return false;
    bool ti = false;
    LPWSTR sid = nullptr;
    if (owner && ConvertSidToStringSidW(owner, &sid)) {
        ti = wcscmp(sid, L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464") == 0;
        LocalFree(sid);
    }
    LocalFree(sd);
    return ti;
}

bool VelocitySupported() {
    if (g_velocity_override >= 0) return g_velocity_override != 0;
    // Kernel build (Windows 10 1703+), immune to a compatibility layer's shim.
    static const bool supported = pulse::os::TrueWindowsBuild() >= 15063;
    return supported;
}

// HKCR view of a class key: the user layer wins.
bool OpenMerged(const Roots& roots, const std::wstring& rel, Key& key) {
    if (Open(roots.user, FullPath(roots, Root::User, rel), KEY_READ, key) == ERROR_SUCCESS) return true;
    return Open(roots.machine, FullPath(roots, Root::Machine, rel), KEY_READ, key) == ERROR_SUCCESS;
}

// HKCR merges per value: an HKCU key with an empty default (common for .png
// after "Open with") still inherits HKLM's ProgID.
std::wstring ReadMergedString(const Roots& roots, const std::wstring& rel, const wchar_t* name) {
    for (const Root root : {Root::User, Root::Machine}) {
        Key k;
        std::wstring value;
        if (Open(RootKey(roots, root), FullPath(roots, root, rel), KEY_READ, k) == ERROR_SUCCESS &&
            ReadString(k.h, name, value) && !value.empty()) return value;
    }
    return {};
}

bool MergedKeyExists(const Roots& roots, const std::wstring& rel) {
    Key k;
    return OpenMerged(roots, rel, k);
}

// Explorer's effective ProgIDs for an extension: the user's choice, then the
// registered default.
std::vector<std::wstring> ExtensionProgIds(const Roots& roots, const std::wstring& ext) {
    std::vector<std::wstring> ids;
    std::wstring choice;
    if (!roots.file_exts.empty()) {
        Key uc;
        if (Open(roots.user, roots.file_exts + L"\\" + ext + L"\\UserChoice", KEY_READ, uc) == ERROR_SUCCESS)
            ReadString(uc.h, L"ProgId", choice);
    }
    for (auto& id : {choice, ReadMergedString(roots, ext, nullptr)}) {
        if (id.empty()) continue;
        if (std::none_of(ids.begin(), ids.end(), [&](const std::wstring& x) { return IEquals(x, id); }))
            ids.push_back(id);
    }
    return ids;
}

void FillHandler(const Roots& roots, Item& it) {
    Key cls;
    if (it.clsid.empty() || !OpenMerged(roots, L"CLSID\\" + it.clsid, cls)) {
        it.missing = !it.clsid.empty();
        return;
    }
    std::wstring name;
    if (roots.resolve_strings && ReadString(cls.h, nullptr, name) && !name.empty() && LooksLikeGuid(it.display))
        it.display = name;
    Key inproc;
    std::wstring dll;
    if (Open(cls.h, L"InprocServer32", KEY_READ, inproc) == ERROR_SUCCESS && ReadString(inproc.h, nullptr, dll)) {
        it.file = ResolveFile(RawPath(dll));
        if (it.file.empty() && PointsAtMissingFile(dll)) it.missing = true;
    }
}

void ScanVerbs(const Roots& roots, const Scene& scene, const std::wstring& cls, Root root,
               std::vector<Item>& out) {
    Key shell;
    if (Open(RootKey(roots, root), FullPath(roots, root, cls + L"\\shell"), KEY_READ, shell) != ERROR_SUCCESS)
        return;
    for (const auto& verb : Subkeys(shell.h)) {
        // printto only serves drag-onto-printer; Explorer never lists it.
        if (IEquals(verb, L"printto")) continue;
        Key v;
        if (Open(shell.h, verb, KEY_READ, v) != ERROR_SUCCESS) continue;
        Item it;
        it.kind = Kind::Verb;
        it.root = root;
        it.scene = scene.id;
        it.class_key = cls;
        it.key_name = verb;
        it.key_path = cls + L"\\shell\\" + verb;
        std::wstring text;
        if (!ReadString(v.h, L"MUIVerb", text) || text.empty()) ReadString(v.h, nullptr, text);
        it.display = text.empty() ? verb : StripMnemonic(ResolveIndirect(roots, text));
        if (!it.display.empty() && it.display[0] == L'@') it.display = verb;
        ReadString(v.h, L"Icon", it.icon);
        Key cmd;
        if (Open(v.h, L"command", KEY_READ, cmd) == ERROR_SUCCESS) ReadString(cmd.h, nullptr, it.command);
        it.file = ResolveFile(RawPath(it.command));
        if (it.file.empty() && !it.icon.empty()) it.file = ResolveFile(RawPath(it.icon));
        it.missing = PointsAtMissingFile(it.command) || PointsAtMissingFile(it.icon);
        it.extended = HasValue(v.h, L"Extended");
        it.position = ReadPlace(v.h);
        it.legacy_disable = HasValue(v.h, L"LegacyDisable");
        it.programmatic_only = HasValue(v.h, L"ProgrammaticAccessOnly");
        DWORD velocity = 0, flags = 0;
        it.velocity_hidden = ReadDword(v.h, L"HideBasedOnVelocityId", velocity);
        it.command_flags_hidden = ReadDword(v.h, L"CommandFlags", flags) && (flags & 0x8);
        it.enabled = !(it.legacy_disable || it.programmatic_only || it.velocity_hidden || it.command_flags_hidden);
        it.is_protected = IsProtectedVerb(verb);
        it.trusted_installer = OwnedByTrustedInstaller(roots, root, it.key_path);
        out.push_back(std::move(it));
    }
}

void ScanHandlers(const Roots& roots, const Scene& scene, const std::wstring& cls, Root root,
                  const wchar_t* group, Kind kind, std::vector<Item>& out) {
    for (const bool dashed : {false, true}) {
        const std::wstring sub = cls + L"\\shellex\\" + (dashed ? L"-" : L"") + group;
        Key handlers;
        if (Open(RootKey(roots, root), FullPath(roots, root, sub), KEY_READ, handlers) != ERROR_SUCCESS) continue;
        for (const auto& name : Subkeys(handlers.h)) {
            Key h;
            if (Open(handlers.h, name, KEY_READ, h) != ERROR_SUCCESS) continue;
            Item it;
            it.kind = kind;
            it.root = root;
            it.scene = scene.id;
            it.class_key = cls;
            it.key_name = name;
            it.key_path = sub + L"\\" + name;
            if (LooksLikeGuid(name)) it.clsid = name;
            else ReadString(h.h, nullptr, it.clsid);
            if (!LooksLikeGuid(it.clsid)) it.clsid.clear();
            it.display = name;
            FillHandler(roots, it);
            it.dash_marker = dashed;
            it.enabled = !dashed;
            it.is_protected = IsProtectedClsid(it.clsid);
            it.trusted_installer = OwnedByTrustedInstaller(roots, root, it.key_path);
            out.push_back(std::move(it));
        }
    }
}

bool IsEffectiveShellNew(HKEY sn) {
    for (const wchar_t* name : {L"NullFile", L"Data", L"FileName", L"Directory", L"Command", L"Handler"})
        if (HasValue(sn, name)) return true;
    return false;
}

void ScanShellNew(const Roots& roots, const Scene& scene, std::vector<Item>& out) {
    // Explorer: .ext's ProgID decides; .ext\<ProgID>\ShellNew when that key
    // exists, otherwise .ext\ShellNew. Keys without a creation value are inert.
    std::set<std::wstring> exts;
    for (const Root root : {Root::User, Root::Machine}) {
        Key classes;
        if (Open(RootKey(roots, root), RootBase(roots, root), KEY_READ, classes) != ERROR_SUCCESS) continue;
        for (const auto& ext : Subkeys(classes.h))
            if (ext.size() >= 2 && ext[0] == L'.') exts.insert(ext);
    }
    auto add = [&](Root root, const std::wstring& cls, const std::wstring& base, const std::wstring& progid) {
        Key classes;
        if (Open(RootKey(roots, root), RootBase(roots, root), KEY_READ, classes) != ERROR_SUCCESS) return;
        for (const bool dashed : {false, true}) {
            const std::wstring path = base + (dashed ? L"\\-ShellNew" : L"\\ShellNew");
            Key sn;
            if (Open(classes.h, path, KEY_READ, sn) != ERROR_SUCCESS || !IsEffectiveShellNew(sn.h)) continue;
            Item it;
            it.kind = Kind::ShellNew;
            it.root = root;
            it.scene = scene.id;
            it.class_key = cls;
            it.key_name = cls;
            it.key_path = path;
            it.dash_marker = dashed;
            it.enabled = !dashed;
            ReadString(sn.h, L"Command", it.command);
            std::wstring text;
            if (!ReadString(sn.h, L"MenuText", text) || text.empty()) ReadString(sn.h, L"ItemName", text);
            if ((text.empty() || text[0] == L'@') && !progid.empty()) {
                std::wstring friendly = ReadMergedString(roots, progid, L"FriendlyTypeName");
                if (friendly.empty()) friendly = ReadMergedString(roots, progid, nullptr);
                if (!friendly.empty()) text = friendly;
            }
            std::wstring resolved = text.empty() ? std::wstring() : ResolveIndirect(roots, text);
            it.display = resolved.empty() || resolved[0] == L'@' ? cls : resolved;
            if (!it.command.empty()) it.missing = PointsAtMissingFile(it.command);
            out.push_back(std::move(it));
        }
    };
    for (const auto& ext : exts) {
        std::wstring progid, base = ext;
        for (const auto& id : ExtensionProgIds(roots, ext)) {
            if (progid.empty()) progid = id;
            if (MergedKeyExists(roots, ext + L"\\" + id)) {
                progid = id;
                base = ext + L"\\" + id;
                break;
            }
        }
        for (const Root root : {Root::User, Root::Machine}) add(root, ext, base, progid);
    }
    // 新建 → 文件夹 lives on the Folder class; listed but never switchable.
    const size_t first_folder = out.size();
    for (const Root root : {Root::User, Root::Machine}) add(root, L"Folder", L"Folder", L"");
    for (size_t k = first_folder; k < out.size(); ++k) out[k].is_protected = true;
}

void ScanSendTo(const Roots& roots, const Scene& scene, std::vector<Item>& out) {
    std::wstring dir = roots.sendto_dir;
    if (dir.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SendTo, 0, nullptr, &p))) dir = p;
        CoTaskMemFree(p);
    }
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || IEquals(name, L"desktop.ini")) continue;
        Item it;
        it.kind = Kind::SendTo;
        it.root = Root::File;
        it.scene = scene.id;
        it.key_name = name;
        it.key_path = dir + L"\\" + name;
        const size_t dot = name.rfind(L'.');
        it.display = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
        it.enabled = !(fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN);
        it.file = it.key_path;
        out.push_back(std::move(it));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

void ScanOpenWith(const Roots& roots, const Scene& scene, std::vector<Item>& out) {
    for (const Root root : {Root::User, Root::Machine}) {
        Key apps;
        if (Open(RootKey(roots, root), FullPath(roots, root, L"Applications"), KEY_READ, apps) != ERROR_SUCCESS)
            continue;
        for (const auto& exe : Subkeys(apps.h)) {
            // Open With only offers Applications\<name>.exe; ProgID-like
            // keys some installers drop here are never shown.
            if (exe.size() < 5 || !IEquals(std::wstring_view(exe).substr(exe.size() - 4), L".exe")) continue;
            Key ak, cmd;
            if (Open(apps.h, exe, KEY_READ, ak) != ERROR_SUCCESS) continue;
            if (Open(ak.h, L"shell\\open\\command", KEY_READ, cmd) != ERROR_SUCCESS) continue;
            Item it;
            it.kind = Kind::OpenWith;
            it.root = root;
            it.scene = scene.id;
            it.key_name = exe;
            it.key_path = L"Applications\\" + exe;
            ReadString(cmd.h, nullptr, it.command);
            it.file = ResolveFile(RawPath(it.command));
            it.missing = PointsAtMissingFile(it.command);
            std::wstring friendly;
            if (ReadString(ak.h, L"FriendlyAppName", friendly) && !friendly.empty())
                it.display = ResolveIndirect(roots, friendly);
            if (it.display.empty() || it.display[0] == L'@') it.display = exe;
            it.no_open_with = HasValue(ak.h, L"NoOpenWith");
            it.enabled = !it.no_open_with;
            out.push_back(std::move(it));
        }
    }
}

std::wstring ReplaceSegment(const std::wstring& path, std::wstring_view from, std::wstring_view to) {
    const std::wstring lower = Lower(path);
    const std::wstring needle = L"\\" + Lower(from) + L"\\";
    const size_t at = lower.rfind(needle);
    if (at == std::wstring::npos) return {};
    return path.substr(0, at + 1) + std::wstring(to) + path.substr(at + 1 + from.size());
}

const wchar_t* GroupName(Kind kind) {
    return kind == Kind::DragDrop ? L"DragDropHandlers" : L"ContextMenuHandlers";
}

OpResult FromStatus(LSTATUS st) {
    switch (st) {
    case ERROR_SUCCESS: return OpResult::Ok;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return OpResult::NotFound;
    case ERROR_ACCESS_DENIED: return OpResult::AccessDenied;
    default: return OpResult::Failed;
    }
}

bool SkipsLegacyDisable(const std::wstring& key_path) {
    // Folder\\shell\\opennewwindow without LegacyDisable keeps Win+E working.
    return IEquals(key_path, L"Folder\\shell\\opennewwindow");
}

const wchar_t* PlaceToken(Place place) {
    return place == Place::Top ? L"top" : place == Place::Bottom ? L"bottom" : L"default";
}

OpResult ApplyPlace(HKEY v, Place place) {
    if (place == Place::Default) {
        const LSTATUS d = RegDeleteValueW(v, L"Position");
        return d == ERROR_SUCCESS || d == ERROR_FILE_NOT_FOUND ? OpResult::Ok : FromStatus(d);
    }
    const wchar_t* text = place == Place::Top ? L"Top" : L"Bottom";
    const DWORD bytes = static_cast<DWORD>((wcslen(text) + 1) * sizeof(wchar_t));
    const LSTATUS w = RegSetValueExW(v, L"Position", 0, REG_SZ, reinterpret_cast<const BYTE*>(text), bytes);
    return w == ERROR_SUCCESS ? OpResult::Ok : FromStatus(w);
}

unsigned PlanVerb(HKEY v, const Op& op) {
    const bool velocity = HasValue(v, L"HideBasedOnVelocityId");
    const bool legacy = HasValue(v, L"LegacyDisable");
    const bool programmatic = HasValue(v, L"ProgrammaticAccessOnly");
    DWORD flags = 0;
    const bool flag_bit = ReadDword(v, L"CommandFlags", flags) && (flags & 0x8);
    unsigned mask = 0;
    if (op.action == Action::Disable) {
        if (VelocitySupported() && !velocity) mask |= kMarkVelocity;
        if (!SkipsLegacyDisable(op.key_path) && !legacy) mask |= kMarkLegacy;
        if (!programmatic) mask |= kMarkProgrammatic;
    } else {
        if (velocity) mask |= kMarkVelocity;
        if (legacy) mask |= kMarkLegacy;
        if (programmatic) mask |= kMarkProgrammatic;
        if (flag_bit) mask |= kMarkFlags;
    }
    return mask;
}

OpResult ApplyVerb(const Roots& roots, const Op& op) {
    Key v;
    const LSTATUS st = Open(RootKey(roots, op.root), FullPath(roots, op.root, op.key_path),
                            KEY_QUERY_VALUE | KEY_SET_VALUE, v);
    if (st != ERROR_SUCCESS) return FromStatus(st);
    if (op.action == Action::Position) return ApplyPlace(v.h, op.place);
    const unsigned mask = op.planned ? op.mask : PlanVerb(v.h, op);
    const wchar_t empty[] = L"";
    LSTATUS w = ERROR_SUCCESS;
    DWORD flags = 0;
    const bool has_flags = ReadDword(v.h, L"CommandFlags", flags);
    if (op.action == Action::Disable) {
        if (mask & kMarkVelocity) {
            const DWORD id = kVelocityHideId;
            w = RegSetValueExW(v.h, L"HideBasedOnVelocityId", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&id),
                               sizeof(id));
            if (w != ERROR_SUCCESS) return FromStatus(w);
        }
        if (mask & kMarkLegacy) {
            w = RegSetValueExW(v.h, L"LegacyDisable", 0, REG_SZ, reinterpret_cast<const BYTE*>(empty), sizeof(empty));
            if (w != ERROR_SUCCESS) return FromStatus(w);
        }
        if (mask & kMarkProgrammatic) {
            w = RegSetValueExW(v.h, L"ProgrammaticAccessOnly", 0, REG_SZ, reinterpret_cast<const BYTE*>(empty),
                               sizeof(empty));
            if (w != ERROR_SUCCESS) return FromStatus(w);
        }
        if ((mask & kMarkFlags) && has_flags) {
            flags |= 0x8;
            w = RegSetValueExW(v.h, L"CommandFlags", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&flags), sizeof(flags));
            if (w != ERROR_SUCCESS) return FromStatus(w);
        }
        return OpResult::Ok;
    }
    const std::pair<unsigned, const wchar_t*> values[] = {{kMarkVelocity, L"HideBasedOnVelocityId"},
                                                          {kMarkLegacy, L"LegacyDisable"},
                                                          {kMarkProgrammatic, L"ProgrammaticAccessOnly"}};
    for (const auto& [bit, name] : values) {
        if (!(mask & bit)) continue;
        const LSTATUS d = RegDeleteValueW(v.h, name);
        if (d != ERROR_SUCCESS && d != ERROR_FILE_NOT_FOUND) return FromStatus(d);
    }
    if ((mask & kMarkFlags) && has_flags && (flags & 0x8)) {
        flags &= ~DWORD{0x8};
        w = RegSetValueExW(v.h, L"CommandFlags", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&flags), sizeof(flags));
        if (w != ERROR_SUCCESS) return FromStatus(w);
    }
    return OpResult::Ok;
}

bool IsEmptyKey(HKEY base, const std::wstring& path) {
    Key k;
    if (Open(base, path, KEY_READ, k) != ERROR_SUCCESS) return false;
    DWORD subkeys = 0, values = 0;
    if (RegQueryInfoKeyW(k.h, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr,
                         nullptr, nullptr) != ERROR_SUCCESS) return false;
    return subkeys == 0 && values == 0;
}

// Moves the key `src` to `dst` (full paths under `base`), keeping values and
// subkeys. `dst` must not exist yet.
OpResult MoveTree(HKEY base, const std::wstring& src, const std::wstring& dst) {
    Key s;
    LSTATUS st = Open(base, src, KEY_READ, s);
    if (st != ERROR_SUCCESS) return FromStatus(st);
    {
        Key probe;
        if (Open(base, dst, KEY_READ, probe) == ERROR_SUCCESS) return OpResult::Conflict;
    }
    Key d;
    DWORD disposition = 0;
    st = RegCreateKeyExW(base, dst.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &d.h,
                         &disposition);
    if (st != ERROR_SUCCESS) return FromStatus(st);
    st = RegCopyTreeW(s.h, nullptr, d.h);
    if (st != ERROR_SUCCESS) {
        RegCloseKey(d.h);
        d.h = nullptr;
        RegDeleteTreeW(base, dst.c_str());
        return FromStatus(st);
    }
    RegCloseKey(s.h);
    s.h = nullptr;
    // RegDeleteTreeW with a subkey name removes that key itself as well.
    st = RegDeleteTreeW(base, src.c_str());
    if (st != ERROR_SUCCESS) {
        // The delete may have stopped half way: rebuild the original from the
        // copy before dropping the copy, so nothing is lost either way.
        Key back;
        bool restored = RegCreateKeyExW(base, src.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
                                        nullptr, &back.h, nullptr) == ERROR_SUCCESS &&
                        RegCopyTreeW(d.h, nullptr, back.h) == ERROR_SUCCESS;
        RegCloseKey(d.h);
        d.h = nullptr;
        if (restored) RegDeleteTreeW(base, dst.c_str());
        return FromStatus(st);
    }
    return OpResult::Ok;
}

// Removes the empty parents of `path` up to (not including) `stop`.
void PruneEmptyParents(HKEY base, std::wstring path, const std::wstring& stop) {
    for (;;) {
        const size_t slash = path.rfind(L'\\');
        if (slash == std::wstring::npos) return;
        path.resize(slash);
        if (path.size() <= stop.size() || !IsEmptyKey(base, path)) return;
        if (RegDeleteKeyW(base, path.c_str()) != ERROR_SUCCESS) return;
    }
}

// Drops `path` and its group when that group ends up empty.
OpResult DropTree(HKEY base, const std::wstring& path) {
    const LSTATUS st = RegDeleteTreeW(base, path.c_str());
    if (st != ERROR_SUCCESS) return FromStatus(st);
    const size_t slash = path.rfind(L'\\');
    if (slash != std::wstring::npos) {
        const std::wstring group = path.substr(0, slash);
        if (IsEmptyKey(base, group)) RegDeleteKeyW(base, group.c_str());
    }
    return OpResult::Ok;
}

// Moves <parent>\\<from>\\<name> to <parent>\\<to>\\<name>, keeping values and subkeys.
// A program can re-register its key while an older copy still sits in the
// paired group, leaving both present; the keys then must not block each other
// forever. Disabling keeps the key the program wrote now and drops the stale
// copy, enabling is already satisfied by the live key and drops the duplicate.
// Every batch is exported to .reg before it runs, so a dropped copy stays in
// the Backups folder.
OpResult MoveKey(const Roots& roots, Root root, const std::wstring& src_rel,
                 const std::wstring& dst_rel, bool replace_destination) {
    HKEY base = RootKey(roots, root);
    const std::wstring src = FullPath(roots, root, src_rel);
    const std::wstring dst = FullPath(roots, root, dst_rel);
    OpResult moved = MoveTree(base, src, dst);
    if (moved == OpResult::Conflict) {
        moved = replace_destination ? DropTree(base, dst) : DropTree(base, src);
        if (moved == OpResult::Ok && replace_destination) moved = MoveTree(base, src, dst);
    }
    if (moved != OpResult::Ok) return moved;
    // An emptied (-)ContextMenuHandlers group goes too, so a round trip leaves
    // no trace.
    const std::wstring group = src.substr(0, src.rfind(L'\\'));
    if (IsEmptyKey(base, group)) RegDeleteKeyW(base, group.c_str());
    return OpResult::Ok;
}

const std::wstring& TrashBase(const Roots& r, Root root) {
    return root == Root::Machine ? r.machine_trash : r.user_trash;
}

// 删除残留项 and its undo. The trash path comes from the checked key path
// only; nothing from outside names it.
OpResult ApplyTrash(const Roots& roots, const Op& op) {
    HKEY base = RootKey(roots, op.root);
    const std::wstring& trash_base = TrashBase(roots, op.root);
    if (trash_base.empty()) return OpResult::Unsupported;
    const std::wstring live = FullPath(roots, op.root, op.key_path);
    const std::wstring trash = Join(trash_base, op.key_path);
    if (op.action == Action::Delete) {
        // An older copy of the same key (deleted, put back, deleted again)
        // gives way; its .reg backup is still in the Backups folder.
        {
            Key live_key;
            const LSTATUS st = Open(base, live, KEY_READ, live_key);
            if (st != ERROR_SUCCESS) return FromStatus(st);
        }
        RegDeleteTreeW(base, trash.c_str());
        const OpResult moved = MoveTree(base, live, trash);
        if (moved != OpResult::Ok) return moved;
        // Like MoveKey: an emptied shell / ContextMenuHandlers group goes too.
        const std::wstring group = live.substr(0, live.rfind(L'\\'));
        if (IsEmptyKey(base, group)) RegDeleteKeyW(base, group.c_str());
        return OpResult::Ok;
    }
    const OpResult moved = MoveTree(base, trash, live);
    if (moved == OpResult::Ok) PruneEmptyParents(base, trash, trash_base);
    return moved;
}

std::vector<std::wstring> Split(std::wstring_view path) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find(L'\\', start);
        parts.emplace_back(path.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start));
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return parts;
}

const wchar_t* KindToken(Kind k) {
    switch (k) {
    case Kind::Verb: return L"verb";
    case Kind::ShellEx: return L"shellex";
    case Kind::DragDrop: return L"dragdrop";
    case Kind::ShellNew: return L"shellnew";
    case Kind::SendTo: return L"sendto";
    case Kind::OpenWith: return L"openwith";
    }
    return L"";
}

std::wstring RootName(HKEY h) {
    if (h == HKEY_LOCAL_MACHINE) return L"HKEY_LOCAL_MACHINE";
    if (h == HKEY_CURRENT_USER) return L"HKEY_CURRENT_USER";
    if (h == HKEY_CLASSES_ROOT) return L"HKEY_CLASSES_ROOT";
    if (h == HKEY_USERS) return L"HKEY_USERS";
    return L"HKEY_CURRENT_USER";
}

void AppendHex(std::wstring& out, const BYTE* data, DWORD size) {
    static const wchar_t kHex[] = L"0123456789abcdef";
    size_t col = out.size() - out.rfind(L'\n');
    for (DWORD i = 0; i < size; ++i) {
        if (i) { out += L','; ++col; }
        if (col > 76) { out += L"\\\r\n  "; col = 2; }
        out += kHex[data[i] >> 4];
        out += kHex[data[i] & 0xF];
        col += 2;
    }
}

void ExportKey(HKEY root, const std::wstring& path, std::wstring& out) {
    Key k;
    if (Open(root, path, KEY_READ, k) != ERROR_SUCCESS) return;
    out += L"[" + RootName(root) + L"\\" + path + L"]\r\n";
    std::vector<wchar_t> name(16384);
    std::vector<BYTE> data;
    for (DWORD i = 0;; ++i) {
        DWORD name_len = static_cast<DWORD>(name.size()), type = 0, size = 0;
        LSTATUS st = RegEnumValueW(k.h, i, name.data(), &name_len, nullptr, &type, nullptr, &size);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) continue;
        data.assign(size + sizeof(wchar_t), 0);
        name_len = static_cast<DWORD>(name.size());
        st = RegEnumValueW(k.h, i, name.data(), &name_len, nullptr, &type, data.data(), &size);
        if (st != ERROR_SUCCESS) continue;
        std::wstring line;
        if (name_len == 0) line = L"@=";
        else {
            line = L"\"";
            for (DWORD c = 0; c < name_len; ++c) {
                if (name[c] == L'\\' || name[c] == L'"') line += L'\\';
                line += name[c];
            }
            line += L"\"=";
        }
        bool plain = type == REG_SZ;
        const wchar_t* text = reinterpret_cast<const wchar_t*>(data.data());
        const size_t chars = size / sizeof(wchar_t);
        if (plain)
            for (size_t c = 0; c < chars && text[c]; ++c)
                if (text[c] == L'\r' || text[c] == L'\n') plain = false;
        if (plain) {
            line += L'"';
            for (size_t c = 0; c < chars && text[c]; ++c) {
                if (text[c] == L'\\' || text[c] == L'"') line += L'\\';
                line += text[c];
            }
            line += L'"';
        } else if (type == REG_DWORD && size == sizeof(DWORD)) {
            wchar_t buf[16];
            swprintf_s(buf, L"dword:%08lx", *reinterpret_cast<const DWORD*>(data.data()));
            line += buf;
        } else {
            if (type == REG_BINARY) line += L"hex:";
            else {
                wchar_t buf[16];
                swprintf_s(buf, L"hex(%lx):", type);
                line += buf;
            }
            out += line;
            AppendHex(out, data.data(), size);
            out += L"\r\n";
            continue;
        }
        out += line + L"\r\n";
    }
    out += L"\r\n";
    for (const auto& sub : Subkeys(k.h)) ExportKey(root, path + L"\\" + sub, out);
}

} // namespace

void SetVelocitySupportedForTesting(int supported) { g_velocity_override = supported; }

const std::vector<Scene>& BuiltinScenes() {
    static const std::vector<Scene> scenes = {
        {L"file", {L"*"}},
        {L"allobj", {L"AllFilesystemObjects"}},
        {L"folder", {L"Directory", L"Folder"}},
        {L"dirbg", {L"Directory\\Background"}},
        {L"desk", {L"DesktopBackground"}},
        {L"drive", {L"Drive"}},
        {L"pc", {L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}"}},
        {L"bin", {L"CLSID\\{645FF040-5081-101B-9F08-00AA002F954E}"}},
        {L"lnk", {L"lnkfile"}},
        {L"exe", {L"exefile"}},
        {L"shellnew", {}, SceneMode::ShellNew},
        {L"sendto", {}, SceneMode::SendTo},
        {L"openwith", {}, SceneMode::OpenWith},
        {L"dragdrop", {L"Directory", L"Drive", L"Folder", L"*"}, SceneMode::DragDrop},
    };
    return scenes;
}

Scene ExtensionScene(const Roots& roots, std::wstring_view ext) {
    Scene s;
    s.id = L"ext:" + Lower(ext);
    s.class_keys.push_back(L"SystemFileAssociations\\" + std::wstring(ext));
    // The user's choice decides which ProgID's verbs Explorer shows; the
    // registered default still applies when there is none.
    for (const auto& id : ExtensionProgIds(roots, std::wstring(ext)))
        if (MergedKeyExists(roots, id)) s.class_keys.push_back(id);
    return s;
}

Scene PerceivedScene(std::wstring_view type) {
    Scene s;
    s.id = L"perc:" + Lower(type);
    s.class_keys.push_back(L"SystemFileAssociations\\" + std::wstring(type));
    return s;
}

std::vector<Item> Enumerate(const Roots& roots, const Scene& scene) {
    std::vector<Item> raw;
    switch (scene.mode) {
    case SceneMode::ShellNew: ScanShellNew(roots, scene, raw); break;
    case SceneMode::SendTo: ScanSendTo(roots, scene, raw); break;
    case SceneMode::OpenWith: ScanOpenWith(roots, scene, raw); break;
    case SceneMode::DragDrop:
        for (const auto& cls : scene.class_keys)
            for (const Root root : {Root::User, Root::Machine})
                ScanHandlers(roots, scene, cls, root, L"DragDropHandlers", Kind::DragDrop, raw);
        break;
    case SceneMode::Menu:
        for (const auto& cls : scene.class_keys)
            for (const Root root : {Root::User, Root::Machine}) {
                ScanVerbs(roots, scene, cls, root, raw);
                ScanHandlers(roots, scene, cls, root, L"ContextMenuHandlers", Kind::ShellEx, raw);
            }
        break;
    }
    // HKCR shows one entry per key; the user layer wins over the machine one.
    std::vector<Item> out;
    std::set<std::wstring> seen;
    for (auto& it : raw) {
        std::wstring id = Lower(it.key_path);
        if (it.kind == Kind::ShellEx || it.kind == Kind::DragDrop) {
            const std::wstring normal = ReplaceSegment(it.key_path, std::wstring(L"-") + GroupName(it.kind),
                                                       GroupName(it.kind));
            if (!normal.empty()) id = Lower(normal);
        } else if (it.kind == Kind::ShellNew && it.dash_marker) {
            id = Lower(it.key_path.substr(0, it.key_path.size() - 10)) + L"\\shellnew";
        }
        id = std::wstring(KindToken(it.kind)) + L"|" + id;
        if (!seen.insert(id).second) continue;
        out.push_back(std::move(it));
    }
    return out;
}

namespace {
// FileName: absolute (after %VAR% expansion), else looked up the way
// Explorer does: the user's Templates, the common Templates, %WINDIR%\ShellNew.
std::wstring ResolveTemplateFile(const std::wstring& raw) {
    if (raw.empty()) return {};
    wchar_t expanded[MAX_PATH * 2]{};
    const DWORD n = ExpandEnvironmentStringsW(raw.c_str(), expanded, ARRAYSIZE(expanded));
    std::wstring name = n > 0 && n <= ARRAYSIZE(expanded) ? expanded : raw;
    if (!PathIsRelativeW(name.c_str())) return FileExists(name) ? name : std::wstring();
    std::vector<std::wstring> dirs;
    for (const KNOWNFOLDERID* id : {&FOLDERID_Templates, &FOLDERID_CommonTemplates}) {
        PWSTR dir = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, nullptr, &dir)) && dir) dirs.emplace_back(dir);
        CoTaskMemFree(dir);
    }
    wchar_t windows[MAX_PATH]{};
    if (GetWindowsDirectoryW(windows, ARRAYSIZE(windows))) dirs.push_back(std::wstring(windows) + L"\\ShellNew");
    for (const auto& dir : dirs) {
        const std::wstring candidate = dir + L"\\" + name;
        if (FileExists(candidate)) return candidate;
    }
    return {};
}

std::vector<BYTE> ReadData(HKEY key) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, L"Data", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0) return {};
    std::vector<BYTE> raw(bytes);
    if (RegQueryValueExW(key, L"Data", nullptr, &type, raw.data(), &bytes) != ERROR_SUCCESS) return {};
    raw.resize(bytes);
    if (type != REG_SZ && type != REG_EXPAND_SZ) return raw;
    // A string Data value is written as ANSI text, without the terminator.
    std::wstring text(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / sizeof(wchar_t));
    text.resize(wcsnlen(text.c_str(), text.size()));
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::vector<BYTE> out(static_cast<size_t>(std::max(size, 0)));
    if (size > 0)
        WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()),
                            reinterpret_cast<char*>(out.data()), size, nullptr, nullptr);
    return out;
}
} // namespace

bool CanCreateNewTemplate(const NewTemplate& t) {
    if (t.ext.size() < 2 || t.ext[0] != L'.' || t.display.empty()) return false;
    if (IEquals(t.ext, L".lnk")) return true;
    if (t.handler) return false;
    if (!t.missing_file.empty() && t.data.empty() && !t.null_file && t.command.empty()) return false;
    return !t.template_file.empty() || !t.data.empty() || t.null_file || !t.command.empty();
}

std::vector<NewTemplate> ListNewTemplates(const Roots& roots) {
    Scene scene;
    scene.id = L"shellnew";
    scene.mode = SceneMode::ShellNew;
    std::vector<NewTemplate> out;
    std::set<std::wstring> seen;
    for (const Item& it : Enumerate(roots, scene)) {
        if (it.kind != Kind::ShellNew || !it.enabled || it.dash_marker) continue;
        if (it.class_key.empty() || it.class_key[0] != L'.') continue;   // Folder
        if (it.missing) continue;                                         // Command exe is gone
        if (!seen.insert(Lower(it.class_key)).second) continue;           // user layer first
        Key sn;
        if (Open(RootKey(roots, it.root), FullPath(roots, it.root, it.key_path), KEY_READ, sn) != ERROR_SUCCESS)
            continue;
        NewTemplate t;
        t.ext = it.class_key;
        t.display = StripMnemonic(it.display);
        if (IEquals(t.display, t.ext) && roots.resolve_strings) {
            // No ItemName / type name in the registry: Explorer shows the
            // shell's type text ("MXG 文件").
            SHFILEINFOW info{};
            if (SHGetFileInfoW(t.ext.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                               SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME) && info.szTypeName[0])
                t.display = info.szTypeName;
        }
        t.command = it.command;
        t.null_file = HasValue(sn.h, L"NullFile");
        t.handler = HasValue(sn.h, L"Handler");
        t.data = ReadData(sn.h);
        std::wstring file;
        if (ReadString(sn.h, L"FileName", file) && !file.empty()) {
            t.template_file = ResolveTemplateFile(file);
            if (t.template_file.empty()) t.missing_file = file;
        }
        if (CanCreateNewTemplate(t)) out.push_back(std::move(t));
    }
    std::sort(out.begin(), out.end(), [](const NewTemplate& a, const NewTemplate& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                               a.display.c_str(), -1, b.display.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return out;
}

Op MakeOp(const Item& item, Action action) {
    Op op;
    op.action = action;
    op.kind = item.kind;
    op.root = item.root;
    op.key_path = item.key_path;
    return op;
}

Op MakePositionOp(const Item& item, Place place) {
    Op op = MakeOp(item, Action::Position);
    op.place = place;
    op.prev_place = item.position;
    return op;
}

void PlanOp(const Roots& roots, Op& op) {
    if (op.kind != Kind::Verb || op.root == Root::File) return;
    if (op.action == Action::Delete || op.action == Action::Restore) return;  // moves the whole key
    Key v;
    if (Open(RootKey(roots, op.root), FullPath(roots, op.root, op.key_path), KEY_READ, v) != ERROR_SUCCESS) return;
    if (op.action == Action::Position) {
        op.prev_place = ReadPlace(v.h);
        op.mask = 0;
    } else {
        op.mask = PlanVerb(v.h, op);
    }
    op.planned = true;
}

std::wstring KeyPathAfter(const Op& op) {
    if (op.kind != Kind::ShellEx && op.kind != Kind::DragDrop) return op.key_path;
    if (op.action != Action::Disable && op.action != Action::Enable) return op.key_path;
    const std::wstring group = GroupName(op.kind);
    const std::wstring dashed = L"-" + group;
    const std::wstring moved = op.action == Action::Disable ? ReplaceSegment(op.key_path, group, dashed)
                                                            : ReplaceSegment(op.key_path, dashed, group);
    return moved.empty() ? op.key_path : moved;
}

Op Inverse(const Op& op) {
    Op inv = op;
    if (op.action == Action::Position) {
        inv.place = op.prev_place;
        inv.prev_place = op.place;
        return inv;
    }
    if (op.action == Action::Delete || op.action == Action::Restore) {
        inv.action = op.action == Action::Delete ? Action::Restore : Action::Delete;
        return inv;
    }
    inv.action = op.action == Action::Disable ? Action::Enable : Action::Disable;
    inv.key_path = KeyPathAfter(op);
    return inv;
}

bool CanWriteWithoutElevation(const Op& op) { return op.root != Root::Machine; }

OpResult Apply(const Roots& roots, const Op& op) {
    if (op.root == Root::File) return OpResult::Unsupported;
    if (!IsAllowedKeyPath(op.kind, op.key_path)) return OpResult::Unsupported;
    if (op.action == Action::Position && op.kind != Kind::Verb) return OpResult::Unsupported;
    if (op.action == Action::Delete || op.action == Action::Restore) {
        if (op.kind != Kind::Verb && op.kind != Kind::ShellEx && op.kind != Kind::DragDrop) return OpResult::Unsupported;
        return ApplyTrash(roots, op);
    }
    switch (op.kind) {
    case Kind::Verb: return ApplyVerb(roots, op);
    case Kind::ShellEx:
    case Kind::DragDrop: {
        const std::wstring target = KeyPathAfter(op);
        if (IEquals(target, op.key_path)) return OpResult::Ok;  // already in the requested state
        return MoveKey(roots, op.root, op.key_path, target, op.action == Action::Disable);
    }
    default: return OpResult::Unsupported;  // ShellNew / SendTo / OpenWith: S3
    }
}

bool IsAllowedKeyPath(Kind kind, std::wstring_view key_path) {
    if (key_path.empty() || key_path.size() > 512) return false;
    if (key_path.front() == L'\\' || key_path.find(L"..") != std::wstring_view::npos ||
        key_path.find(L':') != std::wstring_view::npos || key_path.find(L'/') != std::wstring_view::npos)
        return false;
    for (const wchar_t c : key_path) if (c < 0x20) return false;
    const auto parts = Split(key_path);
    for (const auto& p : parts) if (p.empty()) return false;
    const size_t n = parts.size();
    switch (kind) {
    case Kind::Verb:
        return n >= 3 && IEquals(parts[n - 2], L"shell");
    case Kind::ShellEx:
    case Kind::DragDrop: {
        const std::wstring group = GroupName(kind);
        return n >= 4 && IEquals(parts[n - 3], L"shellex") &&
               (IEquals(parts[n - 2], group) || IEquals(parts[n - 2], L"-" + group));
    }
    default: return false;
    }
}

std::wstring SerializeBatch(const std::vector<Op>& ops) {
    std::wstring out = std::wstring(kBatchHeader) + L"\n";
    for (const auto& op : ops) {
        out += op.action == Action::Disable   ? L"disable\t"
               : op.action == Action::Enable  ? L"enable\t"
               : op.action == Action::Delete  ? L"delete\t"
               : op.action == Action::Restore ? L"restore\t"
                                              : L"position\t";
        out += KindToken(op.kind);
        out += op.root == Root::Machine ? L"\tmachine\t" : L"\tuser\t";
        out += op.key_path;
        if (op.action == Action::Position) out += std::wstring(L"\t") + PlaceToken(op.place);
        else if (op.kind == Kind::Verb && op.planned && (op.action == Action::Disable || op.action == Action::Enable))
            out += L"\t" + std::to_wstring(op.mask & 0xF);
        out += L"\n";
    }
    return out;
}

bool ParseBatch(std::wstring_view text, std::vector<Op>& ops) {
    ops.clear();
    size_t pos = 0;
    bool header = false;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring_view::npos) end = text.size();
        std::wstring_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        if (!header) {
            if (!line.empty() && line.front() == 0xFEFF) line.remove_prefix(1);
            if (line != kBatchHeader) return false;
            header = true;
            continue;
        }
        if (line.empty()) continue;
        std::vector<std::wstring> f;
        size_t s = 0;
        for (int i = 0; i < 3; ++i) {
            const size_t t = line.find(L'\t', s);
            if (t == std::wstring_view::npos) return false;
            f.emplace_back(line.substr(s, t - s));
            s = t + 1;
        }
        std::wstring_view rest = line.substr(s);
        std::wstring_view mask_field;
        const size_t t = rest.find(L'\t');
        if (t != std::wstring_view::npos) {
            mask_field = rest.substr(t + 1);
            rest = rest.substr(0, t);
        }
        f.emplace_back(rest);
        Op op;
        if (f[0] == L"position") {
            // position <kind> <root> <key> <top|bottom|default>; verbs only.
            op.action = Action::Position;
            if (mask_field == L"top") op.place = Place::Top;
            else if (mask_field == L"bottom") op.place = Place::Bottom;
            else if (mask_field == L"default") op.place = Place::Default;
            else return false;
            mask_field = {};
        }
        if (!mask_field.empty()) {
            if (mask_field.size() > 2) return false;
            unsigned m = 0;
            for (const wchar_t c : mask_field) {
                if (c < L'0' || c > L'9') return false;
                m = m * 10 + static_cast<unsigned>(c - L'0');
            }
            if (m > 0xF) return false;
            op.mask = m;
            op.planned = true;
        }
        if (f[0] == L"disable") op.action = Action::Disable;
        else if (f[0] == L"enable") op.action = Action::Enable;
        else if (f[0] == L"delete") op.action = Action::Delete;
        else if (f[0] == L"restore") op.action = Action::Restore;
        else if (f[0] != L"position") return false;
        if (f[1] == L"verb") op.kind = Kind::Verb;
        else if (f[1] == L"shellex") op.kind = Kind::ShellEx;
        else if (f[1] == L"dragdrop") op.kind = Kind::DragDrop;
        else return false;
        if (f[2] == L"machine") op.root = Root::Machine;
        else if (f[2] == L"user") op.root = Root::User;
        else return false;
        op.key_path = f[3];
        if (!IsAllowedKeyPath(op.kind, op.key_path)) return false;
        if (op.planned && op.kind != Kind::Verb) return false;
        if (op.action == Action::Position && op.kind != Kind::Verb) return false;
        if (op.planned && (op.action == Action::Delete || op.action == Action::Restore)) return false;
        ops.push_back(std::move(op));
    }
    return header && ops.size() <= 256;
}

std::wstring ExportReg(const Roots& roots, const std::vector<Op>& ops) {
    std::wstring out = L"Windows Registry Editor Version 5.00\r\n\r\n";
    for (const auto& op : ops) {
        if (op.root == Root::File) continue;
        // Restore: the key sits in the trash until the op runs.
        const std::wstring path = op.action == Action::Restore ? Join(TrashBase(roots, op.root), op.key_path)
                                                               : FullPath(roots, op.root, op.key_path);
        ExportKey(RootKey(roots, op.root), path, out);
    }
    return out;
}

namespace {

constexpr std::pair<OpResult, const wchar_t*> kResultNames[] = {
    {OpResult::Ok, L"ok"},           {OpResult::NotFound, L"notfound"},
    {OpResult::Conflict, L"conflict"}, {OpResult::AccessDenied, L"denied"},
    {OpResult::Unsupported, L"unsupported"}, {OpResult::Failed, L"failed"},
};

bool ReadUtf8File(const std::wstring& path, std::wstring& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::string bytes;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart <= 256 * 1024;
    if (ok) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = bytes.empty() || (ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                               read == bytes.size());
    }
    CloseHandle(f);
    if (!ok) return false;
    size_t skip = bytes.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;
    text.clear();
    if (bytes.size() == skip) return true;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + skip,
                                      static_cast<int>(bytes.size() - skip), nullptr, 0);
    if (n <= 0) return false;
    text.resize(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + skip, static_cast<int>(bytes.size() - skip),
                        text.data(), n);
    return true;
}

bool WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    std::string bytes;
    if (!text.empty()) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
        if (n <= 0) return false;
        bytes.resize(static_cast<size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), n, nullptr,
                            nullptr);
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = bytes.empty() ||
                    (WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                     written == bytes.size());
    CloseHandle(f);
    return ok;
}

} // namespace

const wchar_t* ResultName(OpResult result) {
    for (const auto& [r, name] : kResultNames) if (r == result) return name;
    return L"failed";
}

std::wstring SerializeResults(const std::vector<OpResult>& results) {
    std::wstring out = L"PULSE-SYSCTX-RESULT 1\n";
    for (const OpResult r : results) {
        out += ResultName(r);
        out += L"\n";
    }
    return out;
}

bool ParseResults(std::wstring_view text, std::vector<OpResult>& results) {
    results.clear();
    bool header = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring_view::npos) end = text.size();
        std::wstring_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (!header) {
            if (line != L"PULSE-SYSCTX-RESULT 1") return false;
            header = true;
            continue;
        }
        bool known = false;
        for (const auto& [r, name] : kResultNames) {
            if (line == name) {
                results.push_back(r);
                known = true;
                break;
            }
        }
        if (!known || results.size() > 256) return false;
    }
    return header;
}

int RunElevatedBatch(const std::wstring& batch_path, const std::wstring& result_path) {
    std::wstring text;
    std::vector<Op> ops;
    if (!ReadUtf8File(batch_path, text) || !ParseBatch(text, ops) || ops.empty()) return 2;
    // Per-user keys never need (or get) the elevated helper.
    for (const Op& op : ops) if (op.root != Root::Machine) return 2;
    const Roots roots;
    std::vector<OpResult> results;
    bool changed = false, all_ok = true;
    for (const Op& op : ops) {
        const OpResult r = Apply(roots, op);
        results.push_back(r);
        changed = changed || r == OpResult::Ok;
        all_ok = all_ok && r == OpResult::Ok;
    }
    if (changed) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (!WriteUtf8File(result_path, SerializeResults(results))) return 1;
    return all_ok ? 0 : 1;
}

} // namespace pulse::app::sysctx
