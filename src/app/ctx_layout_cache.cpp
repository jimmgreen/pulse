#include "ctx_layout_cache.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>

namespace pulse::app {

std::wstring GetPulseDataDir();

namespace {

std::wstring LastComponent(std::wstring path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    // A drive root ("C:") or a shell namespace path has no name to template.
    if (name.find(L':') != std::wstring::npos) name.clear();
    return name;
}

std::wstring ParentFolder(std::wstring path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : LastComponent(path.substr(0, slash));
}

bool IsMarker(wchar_t c) {
    return c == kCtxNameFile || c == kCtxNameStem || c == kCtxNameFolder;
}

bool IsWordChar(wchar_t c) {
    return c != 0 && (std::iswalnum(c) || c == L'_');
}

bool Opens(wchar_t c) {
    return c != 0 && std::wcschr(L"\"'\x201C\x2018\x300C\x300E(\xFF08[\x3010<\x300A", c) != nullptr;
}

bool ClosesOrExtends(wchar_t c) {
    return c != 0 &&
           std::wcschr(L"\"'\x201D\x2019\x300D\x300F)\xFF09]\x3011>\x300B.\\/", c) != nullptr;
}

bool NameAt(const std::wstring& text, size_t at, const std::wstring& name) {
    if (name.empty() || at + name.size() > text.size() ||
        text.compare(at, name.size(), name) != 0)
        return false;
    const wchar_t before = at == 0 ? 0 : text[at - 1];
    const size_t end = at + name.size();
    const wchar_t after = end >= text.size() ? 0 : text[end];
    if (IsWordChar(before) || IsWordChar(after)) return false;
    return Opens(before) || ClosesOrExtends(after);
}

bool SameClsid(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

int TimeBucket(uint32_t ms) {
    constexpr uint32_t kEdges[] = { 50, 100, 200, 500, 1000, 2000 };
    int bucket = 0;
    for (const uint32_t edge : kEdges) {
        if (ms < edge) break;
        ++bucket;
    }
    return bucket;
}

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 4);
}

void PutU64(std::vector<uint8_t>& out, uint64_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 8);
}

void PutW(std::vector<uint8_t>& out, std::wstring_view s) {
    PutU32(out, static_cast<uint32_t>(s.size()));
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    out.insert(out.end(), p, p + s.size() * sizeof(wchar_t));
}

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool U32(uint32_t& v) {
        if (end - p < 4) return false;
        std::memcpy(&v, p, 4);
        p += 4;
        return true;
    }
    bool U64(uint64_t& v) {
        if (end - p < 8) return false;
        std::memcpy(&v, p, 8);
        p += 8;
        return true;
    }
    bool W(std::wstring& s) {
        uint32_t n = 0;
        if (!U32(n) || n > 4096 ||
            static_cast<size_t>(end - p) < static_cast<size_t>(n) * sizeof(wchar_t))
            return false;
        s.assign(reinterpret_cast<const wchar_t*>(p), n);
        p += static_cast<size_t>(n) * sizeof(wchar_t);
        return true;
    }
};

constexpr uint32_t kRowEnabled = 1u << 0;
constexpr uint32_t kRowSeparator = 1u << 1;
constexpr uint32_t kRowHasChildren = 1u << 2;
constexpr uint32_t kRowChild = 1u << 3;

class Fnv {
public:
    void Mix(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            hash_ ^= p[i];
            hash_ *= 1099511628211ull;
        }
    }
    void Mix(uint64_t v) { Mix(&v, sizeof(v)); }
    void Mix(std::wstring_view s) {
        Mix(static_cast<uint64_t>(s.size()));
        Mix(s.data(), s.size() * sizeof(wchar_t));
    }
    uint64_t value() const { return hash_; }
private:
    uint64_t hash_ = 1469598103934665603ull;
};

std::wstring RegString(HKEY root, const std::wstring& sub, const wchar_t* value) {
    wchar_t buf[512]{};
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(root, sub.c_str(), value, RRF_RT_REG_SZ, nullptr, buf, &bytes) !=
        ERROR_SUCCESS)
        return {};
    return buf;
}

void MixKey(Fnv& fnv, HKEY root, const std::wstring& sub) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &key) !=
        ERROR_SUCCESS) {
        fnv.Mix(uint64_t{0});
        return;
    }
    DWORD subkeys = 0, values = 0;
    FILETIME written{};
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
                         nullptr, nullptr, nullptr, &written) == ERROR_SUCCESS) {
        fnv.Mix(uint64_t{1});
        fnv.Mix((static_cast<uint64_t>(subkeys) << 32) | values);
        fnv.Mix((static_cast<uint64_t>(written.dwHighDateTime) << 32) | written.dwLowDateTime);
    } else {
        fnv.Mix(uint64_t{2});
    }
    RegCloseKey(key);
}

} // namespace

CtxNames CtxNamesFor(const std::vector<std::wstring>& paths, bool background) {
    CtxNames names;
    if (paths.empty()) return names;
    if (background || paths.size() == 1) {
        names.file = LastComponent(paths.front());
        names.stem = names.file;
        const size_t dot = names.file.find_last_of(L'.');
        if (!background && dot != std::wstring::npos && dot > 0)
            names.stem = names.file.substr(0, dot);
    } else {
        names.folder = ParentFolder(paths.front());
    }
    for (auto* name : { &names.file, &names.stem, &names.folder }) {
        if (std::any_of(name->begin(), name->end(), IsMarker)) name->clear();
    }
    return names;
}

std::wstring TemplateCtxLabel(const std::wstring& text, const CtxNames& names) {
    struct Candidate {
        const std::wstring* name;
        wchar_t marker;
    };
    Candidate candidates[] = { { &names.file, kCtxNameFile }, { &names.stem, kCtxNameStem },
                               { &names.folder, kCtxNameFolder } };
    // Longest first: "a.tar" must win over "a" in 解压到 "a.tar\".
    std::stable_sort(std::begin(candidates), std::end(candidates),
                     [](const Candidate& a, const Candidate& b) {
                         return a.name->size() > b.name->size();
                     });
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        bool matched = false;
        for (const auto& c : candidates) {
            if (!NameAt(text, i, *c.name)) continue;
            out.push_back(c.marker);
            i += c.name->size();
            matched = true;
            break;
        }
        if (!matched) out.push_back(text[i++]);
    }
    return out;
}

std::wstring ExpandCtxLabel(const std::wstring& text, const CtxNames& names) {
    if (std::none_of(text.begin(), text.end(), IsMarker)) return text;
    std::wstring out;
    out.reserve(text.size() + 32);
    for (const wchar_t c : text) {
        if (c == kCtxNameFile) out += names.file;
        else if (c == kCtxNameStem) out += names.stem;
        else if (c == kCtxNameFolder) out += names.folder;
        else out.push_back(c);
    }
    return out;
}

std::wstring CtxLayoutBaseKey(const std::wstring& key) {
    const size_t bar = key.find(L'|');
    return bar == std::wstring::npos ? key : key.substr(0, bar);
}

CtxLayoutRows MergeCtxHandlerShards(const CtxLayoutRows& previous, const CtxLayoutRows& live,
                                    const std::vector<ops::ShellMenuHandlerStatus>& handlers) {
    CtxLayoutRows out;
    out.reserve(previous.size() + live.size());
    std::vector<const std::wstring*> listed;
    listed.reserve(handlers.size());
    const auto is_listed = [&](const std::wstring& clsid) {
        return std::any_of(listed.begin(), listed.end(),
                           [&](const std::wstring* c) { return SameClsid(*c, clsid); });
    };
    for (const auto& handler : handlers) {
        if (handler.clsid.empty() || is_listed(handler.clsid)) continue;
        listed.push_back(&handler.clsid);
        const CtxLayoutRows& from = handler.done ? live : previous;
        for (const auto& row : from)
            if (SameClsid(row.clsid, handler.clsid)) out.push_back(row);
    }
    for (const auto& row : live)
        if (!is_listed(row.clsid)) out.push_back(row);
    return out;
}

bool MergeCtxHandlerStats(CtxHandlerStats& stats,
                          const std::vector<ops::ShellMenuHandlerStatus>& handlers) {
    CtxHandlerStats next;
    next.reserve(handlers.size());
    bool changed = false;
    for (const auto& handler : handlers) {
        if (handler.clsid.empty() ||
            std::any_of(next.begin(), next.end(),
                        [&](const CtxHandlerStat& s) { return SameClsid(s.clsid, handler.clsid); }))
            continue;
        const auto old = std::find_if(stats.begin(), stats.end(), [&](const CtxHandlerStat& s) {
            return SameClsid(s.clsid, handler.clsid);
        });
        CtxHandlerStat stat;
        stat.clsid = handler.clsid;
        if (handler.done) {
            stat.done = true;
            stat.rows = handler.rows;
            stat.elapsed_ms = handler.elapsed_ms;
            if (old == stats.end() || !old->done || old->rows != stat.rows ||
                TimeBucket(old->elapsed_ms) != TimeBucket(stat.elapsed_ms))
                changed = true;
        } else if (old != stats.end()) {
            // Still running: what it gave last time stands. A handler never
            // seen finishing records the longest wait.
            stat = *old;
            stat.clsid = handler.clsid;
            if (!stat.done && handler.elapsed_ms > stat.elapsed_ms) {
                if (TimeBucket(handler.elapsed_ms) != TimeBucket(stat.elapsed_ms)) changed = true;
                stat.elapsed_ms = handler.elapsed_ms;
            }
        } else {
            stat.elapsed_ms = handler.elapsed_ms;
            changed = true;
        }
        next.push_back(std::move(stat));
    }
    if (next.size() != stats.size()) changed = true;
    for (size_t i = 0; !changed && i < next.size(); ++i)
        changed = !SameClsid(next[i].clsid, stats[i].clsid);
    stats = std::move(next);
    return changed;
}

std::vector<uint8_t> SerializeCtxLayout(const CtxLayoutRecords& records, std::wstring_view build) {
    std::vector<uint8_t> out;
    out.insert(out.end(), { 'P', 'C', 'L', 'C' });
    PutU32(out, kCtxLayoutVersion);
    PutW(out, build);
    std::vector<const std::wstring*> keys;
    keys.reserve(records.size());
    for (const auto& [key, record] : records) keys.push_back(&key);
    // Stable bytes for the same content (unordered_map order is not).
    std::sort(keys.begin(), keys.end(),
              [](const std::wstring* a, const std::wstring* b) { return *a < *b; });
    if (keys.size() > kCtxLayoutMaxKeys) keys.resize(kCtxLayoutMaxKeys);
    PutU32(out, static_cast<uint32_t>(keys.size()));
    for (const auto* key : keys) {
        const auto& record = records.at(*key);
        PutW(out, *key);
        PutU64(out, record.fingerprint);
        PutU32(out, static_cast<uint32_t>(record.rows.size()));
        for (const auto& row : record.rows) {
            uint32_t flags = 0;
            if (row.enabled) flags |= kRowEnabled;
            if (row.separator_after) flags |= kRowSeparator;
            if (row.has_children) flags |= kRowHasChildren;
            if (row.child) flags |= kRowChild;
            flags |= static_cast<uint32_t>(row.depth) << 8;
            PutU32(out, row.id);
            PutU32(out, flags);
            PutU32(out, static_cast<uint32_t>(row.mnemonic));
            PutW(out, row.verb);
            PutW(out, row.text);
            PutW(out, row.clsid);
            PutW(out, row.handler);
        }
        PutU32(out, static_cast<uint32_t>(record.handlers.size()));
        for (const auto& stat : record.handlers) {
            PutW(out, stat.clsid);
            PutU32(out, stat.elapsed_ms);
            PutU32(out, stat.rows);
            PutU32(out, stat.done ? 1u : 0u);
        }
    }
    return out;
}

bool ParseCtxLayout(const uint8_t* data, size_t size, std::wstring_view build,
                    CtxLayoutRecords& out) {
    constexpr uint32_t kMaxHandlerStats = 256;  // ipc::kMaxCtxHandlerStatuses
    out.clear();
    if (!data || size < 12 || std::memcmp(data, "PCLC", 4) != 0) return false;
    Reader r{ data + 4, data + size };
    uint32_t version = 0, count = 0;
    std::wstring writer;
    if (!r.U32(version) || version != kCtxLayoutVersion || !r.W(writer) || writer != build ||
        !r.U32(count) || count > kCtxLayoutMaxKeys)
        return false;
    CtxLayoutRecords parsed;
    for (uint32_t i = 0; i < count; ++i) {
        std::wstring key;
        CtxLayoutRecord record;
        uint32_t rows = 0;
        if (!r.W(key) || key.empty() || !r.U64(record.fingerprint) || !r.U32(rows) || rows > 512)
            return false;
        record.rows.reserve(rows);
        for (uint32_t j = 0; j < rows; ++j) {
            ops::ShellMenuItem row;
            uint32_t flags = 0, mnemonic = 0;
            if (!r.U32(row.id) || !r.U32(flags) || !r.U32(mnemonic) || mnemonic > 0xFFFF ||
                !r.W(row.verb) || !r.W(row.text) || !r.W(row.clsid) || !r.W(row.handler))
                return false;
            row.enabled = (flags & kRowEnabled) != 0;
            row.separator_after = (flags & kRowSeparator) != 0;
            row.has_children = (flags & kRowHasChildren) != 0;
            row.child = (flags & kRowChild) != 0;
            row.depth = static_cast<uint8_t>((flags >> 8) & 0xFF);
            row.mnemonic = static_cast<wchar_t>(mnemonic);
            record.rows.push_back(std::move(row));
        }
        uint32_t stats = 0;
        if (!r.U32(stats) || stats > kMaxHandlerStats) return false;
        record.handlers.reserve(stats);
        for (uint32_t j = 0; j < stats; ++j) {
            CtxHandlerStat stat;
            uint32_t done = 0;
            if (!r.W(stat.clsid) || !r.U32(stat.elapsed_ms) || !r.U32(stat.rows) ||
                !r.U32(done) || done > 1)
                return false;
            stat.done = done != 0;
            record.handlers.push_back(std::move(stat));
        }
        parsed[std::move(key)] = std::move(record);
    }
    if (r.p != r.end) return false;
    out = std::move(parsed);
    return true;
}

uint64_t CtxLayoutFingerprint(const std::wstring& base_key) {
    std::vector<std::wstring> scenes;
    std::vector<std::wstring> names;  // ProgIDs / perceived types read on the way
    const auto lower = [](std::wstring s) {
        for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
        return s;
    };
    if (base_key == L":bg") {
        scenes = { L"Directory\\Background", L"DesktopBackground" };
    } else if (base_key == L":folder") {
        scenes = { L"Directory", L"Folder", L"AllFilesystemObjects" };
    } else if (base_key == L":drive") {
        scenes = { L"Drive", L"Folder", L"AllFilesystemObjects" };
    } else if (!base_key.empty() && base_key[0] == L'.') {
        scenes = { L"*", L"AllFilesystemObjects", base_key,
                   L"SystemFileAssociations\\" + base_key };
        const std::wstring prog_id = RegString(HKEY_CLASSES_ROOT, base_key, nullptr);
        const std::wstring perceived = RegString(HKEY_CLASSES_ROOT, base_key, L"PerceivedType");
        const std::wstring choice_key =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + base_key +
            L"\\UserChoice";
        const std::wstring chosen = RegString(HKEY_CURRENT_USER, choice_key, L"ProgId");
        names = { lower(prog_id), lower(perceived), lower(chosen) };
        if (!prog_id.empty()) scenes.push_back(prog_id);
        if (!chosen.empty() && lower(chosen) != lower(prog_id)) scenes.push_back(chosen);
        if (!perceived.empty()) scenes.push_back(L"SystemFileAssociations\\" + perceived);
    } else {
        scenes = { L"*", L"AllFilesystemObjects", L"Unknown" };
    }
    Fnv fnv;
    fnv.Mix(base_key);
    for (const auto& name : names) fnv.Mix(name);
    static constexpr const wchar_t* kTails[] = { L"", L"\\shell", L"\\shellex\\ContextMenuHandlers" };
    for (const auto& scene : scenes) {
        for (const auto* tail : kTails) {
            const std::wstring sub = L"Software\\Classes\\" + scene + tail;
            MixKey(fnv, HKEY_CURRENT_USER, sub);
            MixKey(fnv, HKEY_LOCAL_MACHINE, sub);
        }
    }
    const std::wstring extensions = L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions";
    MixKey(fnv, HKEY_CURRENT_USER, extensions + L"\\Blocked");
    MixKey(fnv, HKEY_LOCAL_MACHINE, extensions + L"\\Blocked");
    MixKey(fnv, HKEY_LOCAL_MACHINE, extensions + L"\\Approved");
    if (!base_key.empty() && base_key[0] == L'.') {
        MixKey(fnv, HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + base_key +
                   L"\\UserChoice");
    }
    return fnv.value();
}

std::wstring CtxLayoutCachePath() {
    wchar_t over[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"PULSE_CTX_LAYOUT_PATH", over, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return over;
    const std::wstring dir = GetPulseDataDir();
    return dir.empty() ? std::wstring() : dir + L"\\ctx_layout.bin";
}

bool LoadCtxLayoutFile(const std::wstring& path, std::wstring_view build,
                       const CtxFingerprintFn& fingerprint, CtxLayoutMap& out) {
    out.clear();
    if (path.empty()) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 12 || size.QuadPart > 8 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr) &&
                    read == buf.size();
    CloseHandle(h);
    CtxLayoutRecords records;
    if (!ok || !ParseCtxLayout(buf.data(), buf.size(), build, records)) return false;
    std::unordered_map<std::wstring, uint64_t> now;
    for (auto& [key, record] : records) {
        const std::wstring base = CtxLayoutBaseKey(key);
        auto found = now.find(base);
        if (found == now.end())
            found = now.emplace(base, fingerprint ? fingerprint(base) : 0).first;
        if (found->second != record.fingerprint) continue;
        out.emplace(key, CtxLayoutEntry{ std::move(record.rows), std::move(record.handlers) });
    }
    return true;
}

bool SaveCtxLayoutFile(const std::wstring& path, std::wstring_view build,
                       const CtxLayoutMap& layout, const CtxFingerprintFn& fingerprint,
                       const std::function<bool()>& keep_going) {
    if (path.empty()) return false;
    CtxLayoutRecords records;
    std::unordered_map<std::wstring, uint64_t> now;
    for (const auto& [key, entry] : layout) {
        const std::wstring base = CtxLayoutBaseKey(key);
        auto found = now.find(base);
        if (found == now.end())
            found = now.emplace(base, fingerprint ? fingerprint(base) : 0).first;
        records[key] = CtxLayoutRecord{ found->second, entry.rows, entry.handlers };
    }
    const auto bytes = SerializeCtxLayout(records, build);
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                              nullptr) && written == bytes.size();
    CloseHandle(h);
    if (!ok || (keep_going && !keep_going())) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

} // namespace pulse::app
