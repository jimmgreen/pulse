// action_files.cpp - The plans and requests of action_files.h.
//
// Requests are the menus' own: a copy or move keeps both on a clash (as the
// assistant's sort does), 合并到文件夹 is a new folder and a move, 解散文件夹
// is app_commands' DissolveFolders, and the Recycle Bin, a permanent delete,
// a restore and emptying the bin are the requests app_ops_ui queues after its
// dialogs. Each plan is read when the step is reached, so the steps before it
// (select.items, nav.go) have already chosen what it works on.
#include "action_files.h"

#include "../app_internal.h"
#include "../ai_cleanup_scan.h"
#include "../app_commands.h"
#include "../app_runtime.h"
#include "../selection_caps.h"
#include "../../common/localization.h"
#include "../../common/path_utils.h"
#include "../../fs/fs_enum.h"
#include "../../ops/clipboard.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <initializer_list>
#include <utility>

namespace pulse::app::actions {

namespace {

using I = l10n::StringId;

constexpr size_t kMaxItems = 5000;

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

std::wstring Fill(std::wstring text, const wchar_t* key, const std::wstring& value) {
    const std::wstring k = key;
    for (size_t at = text.find(k); at != std::wstring::npos; at = text.find(k, at + value.size()))
        text.replace(at, k.size(), value);
    return text;
}

std::wstring Gb1(uint64_t bytes) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

// By reference: `why` is often made from the plan's own fields, and a by-value
// plan could be moved from before they are read.
FilePlan Fail(FilePlan&& p, const char* error, std::wstring why) {
    p.ok = false;
    p.error = error;
    p.why = std::move(why);
    return std::move(p);
}

std::wstring Leaf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

std::wstring Parent(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    std::wstring p = path.substr(0, slash);
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    return p;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    return dir.back() == L'\\' ? dir + name : dir + L'\\' + name;
}

bool Same(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

bool Under(const std::wstring& path, const std::wstring& root) {
    if (root.empty() || path.size() < root.size()) return false;
    if (_wcsnicmp(path.c_str(), root.c_str(), root.size()) != 0) return false;
    return path.size() == root.size() || path[root.size()] == L'\\' || root.back() == L'\\';
}

bool IsDriveRoot(const std::wstring& path) {
    return (path.size() == 2 || (path.size() == 3 && path[2] == L'\\')) && path[1] == L':';
}

bool IsDir(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring KnownFolder(const KNOWNFOLDERID& id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

// Windows, Program Files and ProgramData: the assistant never moves,
// renames or deletes anything in them, nor puts anything there.
std::wstring SystemFolderOf(const std::wstring& path) {
    for (const KNOWNFOLDERID* id : {&FOLDERID_Windows, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86,
                                    &FOLDERID_ProgramData})
        if (const std::wstring root = KnownFolder(*id); Under(path, root)) return root;
    return {};
}

bool ValidLeaf(const std::wstring& name) {
    if (name.empty() || name.size() > 240 || name == L"." || name == L"..") return false;
    if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
    if (name.back() == L' ' || name.back() == L'.') return false;
    for (wchar_t c : name)
        if (c < 32) return false;
    return true;
}

std::wstring Trimmed(std::wstring v) {
    while (!v.empty() && std::iswspace(v.front())) v.erase(0, 1);
    while (!v.empty() && std::iswspace(v.back())) v.pop_back();
    return v;
}

// args.items, else the focused pane's selection.
bool Items(AppState& s, const json::Value& args, std::vector<std::wstring>& out, FilePlan& p) {
    if (const json::Value* items = args.find("items"); items && items->is_array() && items->size()) {
        for (size_t i = 0; i < items->size(); ++i) {
            if (!items->at(i).is_string()) continue;
            std::wstring path = pulse::path::StripExtendedPathPrefix(Trimmed(json::Wide(items->at(i).as_string())));
            for (wchar_t& c : path)
                if (c == L'/') c = L'\\';
            while (path.size() > 3 && path.back() == L'\\') path.pop_back();
            if (path.size() < 3 || !(path[1] == L':' || path.rfind(L"\\\\", 0) == 0)) {
                p = Fail(std::move(p), "items must be absolute paths", Fill(Text(I::AiFoGone), L"{name}", path));
                return false;
            }
            if (!Exists(path)) {
                p = Fail(std::move(p), "an item does not exist", Fill(Text(I::AiFoGone), L"{name}", Leaf(path)));
                return false;
            }
            out.push_back(std::move(path));
        }
    } else {
        app::Tab* tab = ActiveTab(s);
        if (!tab || !tab->snapshot || IsRecycleTab(tab) || tab->SelectedCount() == 0) {
            p = Fail(std::move(p), "nothing is selected: select the items first (select.items) or give items",
                     Text(I::AiFoNoItems));
            return false;
        }
        const app::SelectionCaps caps = app::ComputeSelectionCaps(*tab, &s.places);
        if (!caps.copy) {
            p = Fail(std::move(p), "the selection is not files or folders", Text(I::AiFoNoItems));
            return false;
        }
        out = SelectedFullPaths(*tab);
        // A tab opened on \\?\C:\… keeps the prefix; plans, drives and
        // cards work on the plain path.
        for (std::wstring& x : out) x = pulse::path::StripExtendedPathPrefix(x);
    }
    if (out.empty()) {
        p = Fail(std::move(p), "nothing to work on", Text(I::AiFoNoItems));
        return false;
    }
    if (out.size() > kMaxItems) {
        p = Fail(std::move(p), "too many items for one step (5000 at most)",
                 Fill(Text(I::AiFoTooMany), L"{n}", std::to_wstring(kMaxItems)));
        return false;
    }
    for (const std::wstring& path : out) {
        if (IsDriveRoot(path) || app::IsUncServerRoot(path)) {
            p = Fail(std::move(p), "a whole drive cannot be moved, renamed or deleted", Text(I::AiFoDriveRoot));
            return false;
        }
        if (const std::wstring root = SystemFolderOf(path); !root.empty()) {
            p = Fail(std::move(p), "the item is inside a system or program folder; Pulse never touches those",
                     Fill(Text(I::AiFoSystem), L"{folder}", root));
            return false;
        }
    }
    return true;
}

void AddRows(FilePlan& p, const std::wstring& to) {
    for (const std::wstring& path : p.sources) {
        FileRow r;
        r.name = Leaf(path);
        r.to = to;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
            r.folder = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!r.folder) r.bytes = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        }
        if (r.folder) p.files_only = false;
        p.bytes += r.bytes;
        p.rows.push_back(std::move(r));
    }
    p.count = p.rows.size();
}

// A real folder the user could open, not a search or This PC.
bool RealFolder(const std::wstring& path) {
    return !path.empty() && !fs::IsVirtualPath(path) && !app::IsUncServerRoot(path);
}

FilePlan PlanTransfer(AppState& s, FilePlan p, const json::Value& args, DestResolver resolve) {
    if (p.paste) {
        app::Tab* tab = ActiveTab(s);
        if (!tab || !RealFolder(tab->current_path) || tab->net_readonly)
            return Fail(std::move(p), "the focused pane is not a folder to paste into", Text(I::AiFoNoDest));
        ops::ClipboardData cb;
        if (!ops::ReadClipboard(cb) || cb.paths.empty())
            return Fail(std::move(p), "the clipboard holds no files", Text(I::AiFoClipEmpty));
        p.op = cb.cut ? FileOp::Move : FileOp::Copy;
        for (const std::wstring& path : cb.paths) p.sources.push_back(fs::NormalizePath(path));
        if (p.op == FileOp::Move)
            for (const std::wstring& path : p.sources)
                if (const std::wstring root = SystemFolderOf(path); !root.empty())
                    return Fail(std::move(p), "a cut item is inside a system or program folder",
                                Fill(Text(I::AiFoSystem), L"{folder}", root));
        p.dest = pulse::path::StripExtendedPathPrefix(tab->current_path);
    } else {
        std::vector<std::wstring> items;
        if (!Items(s, args, items, p)) return p;
        p.sources = std::move(items);
        const std::string dest = args.get_string("dest", args.get_string("path"));
        if (!dest.empty()) {
            p.dest = resolve ? resolve(json::Wide(dest)) : json::Wide(dest);
            if (!RealFolder(p.dest))
                return Fail(std::move(p), "dest must be an absolute folder or start with a known folder (Documents)",
                            Fill(Text(I::AiFoBadDest), L"{dest}", json::Wide(dest)));
        } else {
            // As Ctrl+Alt+C / Ctrl+Alt+X: the pane marked as the destination.
            app::Tab* dst = s.targetPane && s.targetPane != s.pane ? s.targetPane->ActiveTab() : nullptr;
            if (!dst || !RealFolder(dst->current_path) || dst->net_readonly)
                return Fail(std::move(p), "no destination: give dest (a folder)", Text(I::AiFoNoDest));
            p.dest = pulse::path::StripExtendedPathPrefix(dst->current_path);
        }
    }
    if (const std::wstring root = SystemFolderOf(p.dest); !root.empty())
        return Fail(std::move(p), "dest is inside a system or program folder", Fill(Text(I::AiFoSystem), L"{folder}", root));
    if (Exists(p.dest) && !IsDir(p.dest))
        return Fail(std::move(p), "dest is a file, not a folder", Fill(Text(I::AiFoBadDest), L"{dest}", p.dest));
    p.dest_exists = IsDir(p.dest);
    for (const std::wstring& src : p.sources)
        if (Under(p.dest, src))
            return Fail(std::move(p), "a folder cannot go into itself", Text(I::AiFoIntoItself));
    // A move into the folder an item is in already does nothing for it.
    if (p.op == FileOp::Move) {
        std::erase_if(p.sources, [&](const std::wstring& src) { return Same(Parent(src), p.dest); });
        if (p.sources.empty()) {
            p.ok = true;
            p.skip = true;
            p.why = Fill(Text(I::AiFoSameFolder), L"{folder}", Leaf(p.dest));
            return p;
        }
    }
    AddRows(p, p.dest);
    if (p.dest_exists)
        for (const FileRow& r : p.rows)
            if (Exists(Join(p.dest, r.name))) ++p.clashes;
    p.ok = true;
    return p;
}

FilePlan PlanRename(AppState& s, FilePlan p, const json::Value& args) {
    std::vector<std::wstring> items;
    json::Value one = json::Value::Object();
    if (const json::Value* it = args.find("item"); it && it->is_string()) {
        json::Value list = json::Value::Array();
        list.push(*it);
        one.set("items", std::move(list));
    }
    if (!Items(s, one.find("items") ? one : args, items, p)) return p;
    if (items.size() != 1) return Fail(std::move(p), "file.rename renames one item; use rename_files for several", Text(I::AiFoOneItem));
    p.new_name = Trimmed(json::Wide(args.get_string("name", args.get_string("new_name"))));
    if (!ValidLeaf(p.new_name))
        return Fail(std::move(p), "name must be a plain file name", Fill(Text(I::AiFoBadName), L"{name}", p.new_name));
    p.sources = std::move(items);
    const std::wstring old = Leaf(p.sources[0]);
    if (old == p.new_name) {
        p.ok = true;
        p.skip = true;
        return p;
    }
    const std::wstring target = Join(Parent(p.sources[0]), p.new_name);
    if (!Same(old, p.new_name) && Exists(target))
        return Fail(std::move(p), "an item of that name is already there", Fill(Text(I::AiFoNameTaken), L"{name}", p.new_name));
    AddRows(p, p.new_name);
    p.dest = Parent(p.sources[0]);
    p.ok = true;
    return p;
}

FilePlan PlanGroup(AppState& s, FilePlan p, const json::Value& args) {
    std::vector<std::wstring> items;
    if (!Items(s, args, items, p)) return p;
    p.sources = std::move(items);
    p.new_name = Trimmed(json::Wide(args.get_string("name")));
    if (!ValidLeaf(p.new_name))
        return Fail(std::move(p), "give name: the new folder's name", Fill(Text(I::AiFoBadName), L"{name}", p.new_name));
    p.dest = Parent(p.sources[0]);
    for (const std::wstring& src : p.sources)
        if (!Same(Parent(src), p.dest))
            return Fail(std::move(p), "the items must be in one folder", Text(I::AiFoOneFolder));
    if (Exists(Join(p.dest, p.new_name)))
        return Fail(std::move(p), "an item of that name is already there", Fill(Text(I::AiFoNameTaken), L"{name}", p.new_name));
    p.dest_exists = false;
    AddRows(p, p.new_name);
    p.ok = true;
    return p;
}

FilePlan PlanDissolve(AppState& s, FilePlan p, const json::Value& args) {
    std::vector<std::wstring> items;
    if (!Items(s, args, items, p)) return p;
    p.sources = std::move(items);
    p.dest = Parent(p.sources[0]);
    for (const std::wstring& src : p.sources) {
        if (!IsDir(src)) return Fail(std::move(p), "only folders can be dissolved", Text(I::AiFoNotFolders));
        if (!Same(Parent(src), p.dest))
            return Fail(std::move(p), "the folders must be in one folder", Text(I::AiFoOneFolder));
    }
    AddRows(p, p.dest);
    for (FileRow& r : p.rows) {
        WIN32_FIND_DATAW fd{};
        size_t n = 0;
        HANDLE h = FindFirstFileExW(Join(Join(p.dest, r.name), L"*").c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (std::wcscmp(fd.cFileName, L".") != 0 && std::wcscmp(fd.cFileName, L"..") != 0) ++n;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        r.bytes = n;   // a folder's row says how many items move up
        p.inner += n;
    }
    p.ok = true;
    return p;
}

FilePlan PlanRecycle(AppState& s, FilePlan p, const json::Value& args) {
    std::vector<std::wstring> items;
    if (!Items(s, args, items, p)) return p;
    p.sources = std::move(items);
    AddRows(p, {});
    // Each drive's bin must take its share (the second batch's rule): one
    // that has none, or has no room, deletes for good, so nothing is done.
    std::vector<std::pair<wchar_t, uint64_t>> drives;
    for (size_t i = 0; i < p.sources.size(); ++i) {
        const std::wstring& path = p.sources[i];
        const wchar_t d = path.size() >= 2 && path[1] == L':' ? static_cast<wchar_t>(std::towupper(path[0])) : 0;
        auto it = std::find_if(drives.begin(), drives.end(), [&](const auto& x) { return x.first == d; });
        if (it == drives.end()) drives.push_back({d, p.rows[i].bytes});
        else it->second += p.rows[i].bytes;
    }
    for (const auto& [drive, bytes] : drives) {
        const app::ai::BinState bin = drive ? app::ai::ReadBin(drive) : app::ai::BinState{};
        if (!bin.available) {
            p = Fail(std::move(p), "this place has no Recycle Bin, so deleting would be permanent; the user deletes it",
                     Text(I::AiFoBinOff));
            p.error += drive ? std::string(" (drive ") + static_cast<char>(drive) + ":)" : std::string(" (not on a drive)");
            return p;
        }
        p.bin_max = bin.max_bytes;
        p.bin_used = bin.used_bytes;
        if (bin.used_bytes + bytes > bin.max_bytes)
            return Fail(std::move(p), "the Recycle Bin is too small for these files",
                        Fill(Fill(Text(I::AiClBinFull), L"{max}", Gb1(bin.max_bytes)), L"{used}", Gb1(bin.used_bytes)));
    }
    p.ok = true;
    return p;
}

FilePlan PlanPurge(AppState& s, FilePlan p, const json::Value& args) {
    if (const app::Tab* tab = ActiveTab(s); tab && IsRecycleTab(tab) && !args.find("items"))
        return Fail(std::move(p), "items in the Recycle Bin: use recycle.empty, or the user deletes them", Text(I::AiFoInBin));
    std::vector<std::wstring> items;
    if (!Items(s, args, items, p)) return p;
    p.sources = std::move(items);
    AddRows(p, {});
    p.ok = true;
    return p;
}

FilePlan PlanRestore(AppState& s, FilePlan p) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || !IsRecycleTab(tab) || tab->SelectedCount() == 0)
        return Fail(std::move(p), "open the Recycle Bin (nav.go pulse:recycle) and select the items first", Text(I::AiFoNotBin));
    for (int index : tab->SelectedIndices()) {
        if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
        const fs::DirEntry entry = tab->EntryAt(static_cast<size_t>(index));
        if (entry.recycle_path.empty()) continue;
        const std::wstring original = entry.full_path;
        if (const std::wstring root = SystemFolderOf(original); !root.empty())
            return Fail(std::move(p), "the item came from a system or program folder", Fill(Text(I::AiFoSystem), L"{folder}", root));
        FileRow r;
        r.name = entry.name;
        r.to = Parent(original);
        r.folder = entry.is_dir;
        r.bytes = entry.is_dir ? 0 : entry.size;
        // Something is at its place again: the restore would fail on it, so
        // it stays in the bin and the card says so.
        if (!original.empty() && Exists(original)) {
            ++p.clashes;
            r.skip = true;
        } else {
            p.sources.push_back(entry.recycle_path);
            if (r.folder) p.files_only = false;
            p.bytes += r.bytes;
        }
        p.rows.push_back(std::move(r));
    }
    p.count = p.rows.size();
    if (!p.count) return Fail(std::move(p), "nothing in the Recycle Bin is selected", Text(I::AiFoNotBin));
    if (p.sources.empty())
        return Fail(std::move(p), "every item's original place is taken by another item", Text(I::AiFoRestoreTaken));
    p.ok = true;
    return p;
}

FilePlan PlanEmptyBin(FilePlan p) {
    SHQUERYRBINFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(SHQueryRecycleBinW(nullptr, &info)))
        return Fail(std::move(p), "Windows did not say what is in the Recycle Bin", Text(I::AiActWhyFailed));
    p.count = static_cast<size_t>(info.i64NumItems);
    p.bytes = static_cast<uint64_t>(info.i64Size);
    p.ok = true;
    if (p.count == 0) {
        p.skip = true;
        p.why = Text(I::AiFoBinEmpty);
    }
    return p;
}

// file.organize: moves[{item, dest}] in one plan. An item or a dest that is
// not a full path is a name in the focused folder (下载 + 文档 = 下载\文档);
// without a focused folder a dest goes through the resolver (Documents\发票).
// Any name already taken in its folder refuses the whole plan: nothing is
// overwritten or renamed to (2), and the model is told which name to change.
FilePlan PlanOrganize(AppState& s, FilePlan p, const json::Value& args, DestResolver resolve) {
    const json::Value* moves = args.find("moves");
    if (!moves || !moves->is_array() || moves->size() == 0)
        return Fail(std::move(p), "moves must be a non-empty array of {item, dest}", Text(I::AiFoNoItems));
    if (moves->size() > kMaxItems)
        return Fail(std::move(p), "too many items for one step (5000 at most)",
                    Fill(Text(I::AiFoTooMany), L"{n}", std::to_wstring(kMaxItems)));
    app::Tab* tab = ActiveTab(s);
    const std::wstring here = tab && RealFolder(tab->current_path) && !tab->net_readonly
                                  ? pulse::path::StripExtendedPathPrefix(tab->current_path)
                                  : std::wstring();
    const auto clean = [](std::wstring x) {
        x = pulse::path::StripExtendedPathPrefix(Trimmed(std::move(x)));
        for (wchar_t& c : x)
            if (c == L'/') c = L'\\';
        while (x.size() > 3 && x.back() == L'\\') x.pop_back();
        return x;
    };
    const auto absolute = [](const std::wstring& x) {
        return x.size() >= 3 && ((x[1] == L':' && x[2] == L'\\') || x.rfind(L"\\\\", 0) == 0);
    };
    json::Value list = json::Value::Array();
    std::vector<std::wstring> dest_of;
    for (size_t i = 0; i < moves->size(); ++i) {
        const json::Value& m = moves->at(i);
        if (!m.is_object()) return Fail(std::move(p), "each move is an object {item, dest}", Text(I::AiFoNoItems));
        std::wstring item = clean(json::Wide(m.get_string("item", m.get_string("path"))));
        std::wstring dest = clean(json::Wide(m.get_string("dest", m.get_string("dest_folder"))));
        if (item.empty()) return Fail(std::move(p), "a move has no item", Text(I::AiFoNoItems));
        if (dest.empty()) return Fail(std::move(p), "a move has no dest", Text(I::AiFoNoDest));
        if (!absolute(item)) {
            if (here.empty())
                return Fail(std::move(p), "item is a name but the focused pane is not a folder: give full paths",
                            Fill(Text(I::AiFoOrgNeedFolder), L"{name}", item));
            item = Join(here, item);
        }
        if (!absolute(dest)) {
            if (!here.empty()) {
                dest = Join(here, dest);
            } else {
                const std::wstring raw = dest;
                dest = resolve ? clean(resolve(raw)) : std::wstring();
                if (!absolute(dest))
                    return Fail(std::move(p), "dest is a name but the focused pane is not a folder: give full paths",
                                Fill(Text(I::AiFoOrgNeedFolder), L"{name}", raw));
            }
        }
        list.push(json::Value(json::Utf8(item)));
        dest_of.push_back(std::move(dest));
    }
    json::Value items_args = json::Value::Object();
    items_args.set("items", std::move(list));
    std::vector<std::wstring> items;
    if (!Items(s, items_args, items, p)) return p;   // exists, not a drive, not a system folder
    if (items.size() != dest_of.size())
        return Fail(std::move(p), "every item must be a path string", Text(I::AiFoNoItems));
    for (size_t i = 0; i < items.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (Same(items[i], items[j]))
                return Fail(std::move(p), "an item is listed twice", Fill(Text(I::AiFoOrgTwice), L"{name}", Leaf(items[i])));
    for (size_t i = 0; i < items.size(); ++i) {
        const std::wstring& dest = dest_of[i];
        if (!RealFolder(dest))
            return Fail(std::move(p), "dest must be a folder path", Fill(Text(I::AiFoBadDest), L"{dest}", dest));
        if (const std::wstring root = SystemFolderOf(dest); !root.empty())
            return Fail(std::move(p), "dest is inside a system or program folder",
                        Fill(Text(I::AiFoSystem), L"{folder}", root));
        if (Exists(dest) && !IsDir(dest))
            return Fail(std::move(p), "dest is a file, not a folder", Fill(Text(I::AiFoBadDest), L"{dest}", dest));
        if (Under(dest, items[i]))
            return Fail(std::move(p), "a folder cannot go into itself", Text(I::AiFoIntoItself));
    }
    // Group by folder (first one named first); an item already there does nothing.
    for (size_t i = 0; i < items.size(); ++i) {
        if (Same(Parent(items[i]), dest_of[i])) continue;
        auto it = std::find_if(p.dests.begin(), p.dests.end(), [&](const OrganizeDest& d) { return Same(d.path, dest_of[i]); });
        if (it == p.dests.end()) {
            OrganizeDest d;
            d.path = dest_of[i];
            d.make = !IsDir(d.path);
            p.dests.push_back(std::move(d));
            it = p.dests.end() - 1;
        }
        it->sources.push_back(items[i]);
    }
    if (p.dests.empty()) {
        p.ok = true;
        p.skip = true;
        p.why = Text(I::AiFoOrgAlready);
        return p;
    }
    // A taken name anywhere refuses the whole plan.
    for (const OrganizeDest& d : p.dests) {
        for (size_t i = 0; i < d.sources.size(); ++i) {
            const std::wstring leaf = Leaf(d.sources[i]);
            bool taken = !d.make && Exists(Join(d.path, leaf));
            for (size_t j = 0; j < i && !taken; ++j) taken = Same(Leaf(d.sources[j]), leaf);
            if (taken) {
                p.error = "conflict: \"" + json::Utf8(Join(d.path, leaf)) +
                          "\" is taken (already there, or two items share the name); nothing was moved. "
                          "Pick another dest for that item and try again.";
                p.ok = false;
                p.why = Fill(Fill(Text(I::AiFoOrgClash), L"{name}", leaf), L"{folder}", Leaf(d.path));
                return std::move(p);
            }
        }
    }
    // Missing folders and their missing parents, parents first.
    for (const OrganizeDest& d : p.dests) {
        if (!d.make) continue;
        std::vector<std::wstring> chain;
        for (std::wstring at = d.path; !at.empty() && !IsDir(at); at = Parent(at)) {
            if (IsDriveRoot(at) || app::IsUncServerRoot(at) || Same(Parent(at), at))
                return Fail(std::move(p), "dest is on a drive or share that is not there",
                            Fill(Text(I::AiFoBadDest), L"{dest}", d.path));
            if (Exists(at)) return Fail(std::move(p), "a folder on the dest path is a file",
                                        Fill(Text(I::AiFoBadDest), L"{dest}", at));
            chain.push_back(at);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            if (std::none_of(p.make_folders.begin(), p.make_folders.end(), [&](const std::wstring& f) { return Same(f, *it); }))
                p.make_folders.push_back(*it);
    }
    for (const OrganizeDest& d : p.dests)
        for (const std::wstring& src : d.sources) p.sources.push_back(src);
    for (const OrganizeDest& d : p.dests) {
        FilePlan part;
        part.sources = d.sources;
        AddRows(part, d.path);
        for (FileRow& r : part.rows) p.rows.push_back(std::move(r));
        p.bytes += part.bytes;
        if (!part.files_only) p.files_only = false;
    }
    p.count = p.rows.size();
    p.dest = Parent(p.sources[0]);
    for (const std::wstring& src : p.sources)
        if (!Same(Parent(src), p.dest)) p.one_source_folder = false;
    if (!p.one_source_folder && !here.empty()) p.dest = here;
    p.dest_exists = true;
    p.ok = true;
    return p;
}

} // namespace

FileOp FileOpOf(std::string_view id) noexcept {
    if (id == "file.copy_to") return FileOp::Copy;
    if (id == "file.move_to" || id == "file.paste") return FileOp::Move;
    if (id == "file.rename") return FileOp::Rename;
    if (id == "file.group_into_folder") return FileOp::Group;
    if (id == "file.dissolve") return FileOp::Dissolve;
    if (id == "file.recycle") return FileOp::Recycle;
    if (id == "file.delete_permanent") return FileOp::Purge;
    if (id == "recycle.restore") return FileOp::Restore;
    if (id == "recycle.empty") return FileOp::EmptyBin;
    if (id == "file.organize") return FileOp::Organize;
    return FileOp::None;
}

bool FileOpUndoes(FileOp op) noexcept {
    switch (op) {
    case FileOp::Copy:
    case FileOp::Move:
    case FileOp::Rename:
    case FileOp::Group:
    case FileOp::Dissolve:
    case FileOp::Recycle:
    case FileOp::Organize: return true;
    default: return false;
    }
}

FilePlan PlanFileOp(AppState& s, std::string_view id, const json::Value& args, DestResolver resolve) {
    FilePlan p;
    p.op = FileOpOf(id);
    p.paste = id == "file.paste";
    switch (p.op) {
    case FileOp::Copy:
    case FileOp::Move: return PlanTransfer(s, std::move(p), args, resolve);
    case FileOp::Rename: return PlanRename(s, std::move(p), args);
    case FileOp::Group: return PlanGroup(s, std::move(p), args);
    case FileOp::Dissolve: return PlanDissolve(s, std::move(p), args);
    case FileOp::Recycle: return PlanRecycle(s, std::move(p), args);
    case FileOp::Purge: return PlanPurge(s, std::move(p), args);
    case FileOp::Restore: return PlanRestore(s, std::move(p));
    case FileOp::EmptyBin: return PlanEmptyBin(std::move(p));
    case FileOp::Organize: return PlanOrganize(s, std::move(p), args, resolve);
    case FileOp::None: break;
    }
    return Fail(std::move(p), "not a file action", Text(I::AiActWhyUnknown));
}

std::vector<uint64_t> SubmitFilePlan(AppState& s, const FilePlan& plan, uint64_t undo_group,
                                     const std::wstring& undo_label) {
    std::vector<uint64_t> tasks;
    if (!plan.ok || plan.skip) return tasks;
    const auto request = [&](ops::OpType type) {
        ops::OpRequest r;
        r.type = type;
        r.undo_group = undo_group;
        r.undo_label = undo_label;
        return r;
    };
    switch (plan.op) {
    case FileOp::Copy:
    case FileOp::Move: {
        if (!plan.dest_exists) {
            ops::OpRequest make = request(ops::OpType::CreateFolder);
            make.sources.push_back(plan.dest);
            tasks.push_back(s.ops.Submit(std::move(make)));
        }
        ops::OpRequest r = request(plan.op == FileOp::Move ? ops::OpType::Move : ops::OpType::Copy);
        r.sources = plan.sources;
        r.dest_dir = plan.dest;
        r.collision_policy = ops::CollisionPolicy::KeepBoth;   // nothing is overwritten
        if (plan.op == FileOp::Move) s.cutPaths.clear();
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::Rename: {
        ops::OpRequest r = request(ops::OpType::Rename);
        r.sources = plan.sources;
        r.new_name = plan.new_name;
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::Group: {
        // 合并到文件夹 with the name given: the folder, then the move into it.
        const std::wstring folder = Join(plan.dest, plan.new_name);
        ops::OpRequest make = request(ops::OpType::CreateFolder);
        make.sources.push_back(folder);
        tasks.push_back(s.ops.Submit(std::move(make)));
        ops::OpRequest r = request(ops::OpType::Move);
        r.sources = plan.sources;
        r.dest_dir = folder;
        r.collision_policy = ops::CollisionPolicy::KeepBoth;
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::Dissolve: {
        app::Tab* tab = ActiveTab(s);
        if (tab) tasks = DissolveFolders(s, *tab, plan.sources, undo_group, undo_label);
        break;
    }
    case FileOp::Recycle: {
        ops::OpRequest r = request(ops::OpType::RecycleDelete);
        r.sources = plan.sources;
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::Purge: {
        ops::OpRequest r = request(ops::OpType::RealDelete);
        r.undo_group = 0;
        r.sources = plan.sources;
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::Restore: {
        ops::OpRequest r = request(ops::OpType::RestoreRecycle);
        r.undo_group = 0;
        r.sources = plan.sources;
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::EmptyBin: {
        ops::OpRequest r = request(ops::OpType::EmptyRecycle);
        r.undo_group = 0;
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::Organize: {
        // The new folders (parents first), then one move per folder; all one undo group.
        for (const std::wstring& folder : plan.make_folders) {
            ops::OpRequest make = request(ops::OpType::CreateFolder);
            make.sources.push_back(folder);
            tasks.push_back(s.ops.Submit(std::move(make)));
        }
        s.cutPaths.clear();
        for (const OrganizeDest& d : plan.dests) {
            ops::OpRequest r = request(ops::OpType::Move);
            r.sources = d.sources;
            r.dest_dir = d.path;
            r.collision_policy = ops::CollisionPolicy::KeepBoth;   // checked free; never overwrite a late arrival
            tasks.push_back(s.ops.Submit(std::move(r)));
        }
        break;
    }
    case FileOp::None: break;
    }
    return tasks;
}

} // namespace pulse::app::actions
