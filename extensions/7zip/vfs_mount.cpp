#include "vfs_mount.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include "engine.h"

namespace fs = std::filesystem;

namespace {
std::atomic<uint64_t> g_stage_sequence{0};

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    const int converted = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                              out.data(), needed, nullptr, nullptr);
    if (converted <= 0) return {};
    out.resize(static_cast<size_t>(converted));
    return out;
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const int converted = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                              out.data(), needed);
    if (converted <= 0) return {};
    out.resize(static_cast<size_t>(converted));
    return out;
}

std::wstring ToLowerInvariant(std::wstring_view text) {
    if (text.empty()) return {};
    const int needed = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                                     text.data(), static_cast<int>(text.size()),
                                     nullptr, 0, nullptr, nullptr, 0);
    if (needed <= 0) return std::wstring(text);
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const int mapped = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                                     text.data(), static_cast<int>(text.size()),
                                     out.data(), needed, nullptr, nullptr, 0);
    if (mapped <= 0) return std::wstring(text);
    out.resize(static_cast<size_t>(mapped));
    return out;
}

std::wstring CanonicalArchiveKey(const std::wstring& archive_path) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(archive_path), ec);
    if (ec || p.empty()) {
        p = fs::absolute(fs::path(archive_path), ec);
    }
    std::wstring raw = p.empty() ? archive_path : p.wstring();
    std::replace(raw.begin(), raw.end(), L'/', L'\\');
    return ToLowerInvariant(raw);
}

std::string MakeDisplayName(const std::wstring& archive_path) {
    std::string utf8 = WideToUtf8(fs::path(archive_path).filename().wstring());
    if (utf8.empty()) utf8 = "archive";
    constexpr size_t kMaxDisplayBytes = 128;
    if (utf8.size() > kMaxDisplayBytes) {
        size_t cut = kMaxDisplayBytes;
        while (cut > 0 && (static_cast<unsigned char>(utf8[cut]) & 0xC0u) == 0x80u) --cut;
        utf8.resize(cut);
        if (utf8.empty()) utf8 = "archive";
    }
    return utf8;
}

std::string MakeMountId(std::string_view key_utf8, size_t disambiguator) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : key_utf8) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char hex[17]{};
    static constexpr char kHex[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) {
        hex[i] = kHex[hash & 0xFu];
        hash >>= 4;
    }
    std::string id = "arc-";
    id.append(hex, 16);
    if (disambiguator > 0) {
        id.push_back('-');
        id.append(std::to_string(disambiguator));
    }
    return id;
}

int64_t FileTimeToUnixMs(uint64_t filetime) noexcept {
    constexpr uint64_t kWindowsToUnixEpoch100Ns = 116444736000000000ULL;
    if (filetime <= kWindowsToUnixEpoch100Ns) return 0;
    return static_cast<int64_t>((filetime - kWindowsToUnixEpoch100Ns) / 10000ULL);
}

bool ValidSegmentUtf8(std::string_view seg) noexcept {
    if (seg.empty() || seg.size() > 255 || seg == "." || seg == "..") return false;
    for (unsigned char c : seg) {
        if (c < 0x20u) return false;
        switch (c) {
        case '/':
        case '\\':
        case ':':
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|':
            return false;
        default:
            break;
        }
    }
    return true;
}

bool NormalizeVfsRelPath(std::string_view raw, std::vector<std::string>& out_segments, std::string& out_lower) {
    out_segments.clear();
    out_lower.clear();
    std::wstring wide = Utf8ToWide(raw);
    if (wide.empty() && !raw.empty()) return false;
    std::replace(wide.begin(), wide.end(), L'\\', L'/');

    size_t start = 0;
    while (start < wide.size()) {
        const size_t slash = wide.find(L'/', start);
        const std::wstring_view part_w(wide.data() + start,
                                       slash == std::wstring::npos ? wide.size() - start : slash - start);
        if (!part_w.empty()) {
            std::string part_u8 = WideToUtf8(part_w);
            if (!ValidSegmentUtf8(part_u8)) return false;
            out_segments.push_back(std::move(part_u8));
            std::string part_lower_u8 = WideToUtf8(ToLowerInvariant(part_w));
            if (part_lower_u8.empty()) return false;
            if (!out_lower.empty()) out_lower.push_back('/');
            out_lower.append(part_lower_u8);
        }
        if (slash == std::wstring::npos) break;
        start = slash + 1;
    }
    return out_segments.size() <= 64 && out_lower.size() <= 4096;
}

bool SplitArchiveEntrySegments(const std::wstring& archive_entry_path,
                               std::vector<std::wstring>& out_wide_segments,
                               std::vector<std::string>& out_utf8_segments,
                               std::vector<std::string>& out_lower_segments) {
    out_wide_segments.clear();
    out_utf8_segments.clear();
    out_lower_segments.clear();
    size_t start = 0;
    while (start < archive_entry_path.size()) {
        const size_t sep = archive_entry_path.find_first_of(L"\\/", start);
        const std::wstring_view part_w(
            archive_entry_path.data() + start,
            sep == std::wstring::npos ? archive_entry_path.size() - start : sep - start);
        if (!part_w.empty()) {
            std::string part_u8 = WideToUtf8(part_w);
            std::string part_lower = WideToUtf8(ToLowerInvariant(part_w));
            if (!ValidSegmentUtf8(part_u8) || part_lower.empty()) return false;
            out_wide_segments.emplace_back(part_w);
            out_utf8_segments.push_back(std::move(part_u8));
            out_lower_segments.push_back(std::move(part_lower));
        }
        if (sep == std::wstring::npos) break;
        start = sep + 1;
    }
    return !out_utf8_segments.empty() && out_utf8_segments.size() <= 64;
}

// Only formats 7-Zip can rewrite in place get edit support; the engine re-checks the real format.
bool WritableArchivePath(const std::wstring& archive_path) {
    const std::wstring ext = ToLowerInvariant(fs::path(archive_path).extension().wstring());
    return ext == L".7z" || ext == L".zip" || ext == L".tar";
}

fs::path BackupFolder() {
    wchar_t buffer[MAX_PATH + 1]{};
    const DWORD n = GetTempPathW(MAX_PATH + 1, buffer);
    fs::path base = n > 0 && n <= MAX_PATH ? fs::path(buffer) : fs::temp_directory_path();
    return base / L"Pulse" / L"archive_backups";
}

PulseStatus MapEngineStatus(engine::Status status) noexcept {
    switch (status) {
    case engine::Status::Ok:
        return PULSE_OK;
    case engine::Status::NeedPassword:
    case engine::Status::WrongPassword:
        return PULSE_ERROR_PERMISSION;
    case engine::Status::InvalidInput:
        return PULSE_ERROR_ARGUMENT;
    case engine::Status::Unsupported:
        return PULSE_ERROR_UNAVAILABLE;
    default:
        return PULSE_ERROR_STATE;
    }
}
} // namespace

struct ArchiveVfsManager::MountData {
    std::wstring archive_path;
    std::string display_name;
    bool writable = false;
    std::mutex write_mu;      // one modification at a time per archive
    std::wstring last_backup; // guarded by write_mu; the state before the last modification
    // Seen while listing: an item needs a password (or the listing itself does).
    // Changing such an archive is refused; it stays read-only.
    std::atomic<bool> encrypted{false};

    struct Node {
        pulse::sdk::VfsEntryData entry;
        std::wstring archive_entry_path;
        std::string lower_rel_path;
        std::vector<size_t> children;
        std::unordered_map<std::string, size_t> child_by_lower_name;
    };

    struct Tree {
        std::vector<Node> nodes; // index 0 = root directory ("")
        std::unordered_map<std::string, size_t> by_lower_path;
    };

    std::mutex mu;
    std::condition_variable cv;
    bool loaded = false;
    bool loading = false;
    engine::Status load_status = engine::Status::Ok;
    std::shared_ptr<const Tree> tree;

    MountData(std::wstring path, std::string name)
        : archive_path(std::move(path)), display_name(std::move(name)), writable(WritableArchivePath(archive_path)) {}

    uint32_t ReadonlyFlag() const noexcept { return writable ? 0u : static_cast<uint32_t>(PULSE_VFS_ENTRY_READONLY); }

    // Forget the listing so the next call reads the archive again.
    void Reload() {
        std::lock_guard lock(mu);
        if (loading) return;  // the running load reads the new file anyway
        loaded = false;
        tree.reset();
    }

    static bool ArchiveKey(std::string_view rel_path, std::wstring& out) {
        std::vector<std::string> segs;
        std::string lower;
        if (!NormalizeVfsRelPath(rel_path, segs, lower) || segs.empty()) return false;
        std::string joined;
        for (const auto& seg : segs) {
            if (!joined.empty()) joined.push_back('\\');
            joined.append(seg);
        }
        out = Utf8ToWide(joined);
        return !out.empty();
    }

    static engine::Report Forward(const pulse::sdk::VfsOperation& op) {
        if (!op.raw || !op.raw->progress) return {};
        return [&op](const engine::Progress& p) {
            op.Progress(p.completed_bytes, p.total_bytes, p.completed_items, p.total_items, WideToUtf8(p.current));
        };
    }

    PulseStatus ExtractItems(const std::vector<std::string_view>& rel_paths, std::string_view dest_dir_utf8,
                             const pulse::sdk::VfsOperation& op) {
        if (rel_paths.empty() || op.Cancelled()) return rel_paths.empty() ? PULSE_ERROR_ARGUMENT : PULSE_ERROR_STATE;
        std::vector<std::wstring> entries;
        entries.reserve(rel_paths.size());
        std::wstring base;
        for (size_t i = 0; i < rel_paths.size(); ++i) {
            std::wstring key;
            if (!ArchiveKey(rel_paths[i], key)) return PULSE_ERROR_ARGUMENT;
            const size_t slash = key.find_last_of(L'\\');
            const std::wstring parent = slash == std::wstring::npos ? std::wstring() : key.substr(0, slash);
            if (i == 0) base = parent;
            else if (CompareStringOrdinal(parent.c_str(), static_cast<int>(parent.size()), base.c_str(),
                                          static_cast<int>(base.size()), TRUE) != CSTR_EQUAL) {
                return PULSE_ERROR_ARGUMENT;  // one folder at a time
            }
            entries.push_back(std::move(key));
        }
        const std::wstring dest = Utf8ToWide(dest_dir_utf8);
        if (dest.empty()) return PULSE_ERROR_ARGUMENT;
        const auto cancelled = [&op]() { return op.Cancelled(); };
        const engine::ExtractResult res = engine::ExtractSelected(archive_path, entries, base, dest, cancelled, Forward(op), {},
                                                                  engine::Budget::DefaultExtractBudget());
        if (res.status != engine::Status::Ok && !res.error.empty()) op.Message(res.error);
        return MapEngineStatus(res.status);
    }

    PulseStatus Modify(const std::vector<pulse::sdk::VfsChange>& changes, const pulse::sdk::VfsOperation& op) {
        if (changes.empty()) return PULSE_ERROR_ARGUMENT;
        if (!writable) {
            op.Message("This archive format is read-only");
            return PULSE_ERROR_UNAVAILABLE;
        }
        std::lock_guard write_lock(write_mu);
        if (changes.size() == 1 && changes.front().kind == PULSE_VFS_CHANGE_RESTORE) return Restore(op);
        {
            std::shared_ptr<const Tree> listed;  // learn about passwords before the first change
            EnsureLoaded([&op]() { return op.Cancelled(); }, listed);
        }
        if (encrypted.load(std::memory_order_relaxed)) {
            op.Message("The archive is encrypted");
            return PULSE_ERROR_PERMISSION;
        }

        std::vector<engine::Change> list;
        list.reserve(changes.size());
        for (const auto& change : changes) {
            engine::Change item;
            if (!ArchiveKey(change.rel_path, item.path)) return PULSE_ERROR_ARGUMENT;
            switch (change.kind) {
            case PULSE_VFS_CHANGE_PUT:
                item.kind = engine::Change::Kind::Put;
                item.source = Utf8ToWide(change.source_path_utf8);
                if (item.source.empty()) return PULSE_ERROR_ARGUMENT;
                break;
            case PULSE_VFS_CHANGE_DELETE:
                item.kind = engine::Change::Kind::Delete;
                break;
            case PULSE_VFS_CHANGE_RENAME:
                item.kind = engine::Change::Kind::Rename;
                if (!ArchiveKey(change.new_rel_path, item.target)) return PULSE_ERROR_ARGUMENT;
                break;
            default:
                return PULSE_ERROR_ARGUMENT;
            }
            list.push_back(std::move(item));
        }
        const auto cancelled = [&op]() { return op.Cancelled(); };
        const engine::ModifyResult res =
            engine::Modify(archive_path, list, BackupFolder().wstring(), cancelled, Forward(op), {});
        if (res.status != engine::Status::Ok) {
            if (!res.error.empty()) op.Message(res.error);
            return MapEngineStatus(res.status);
        }
        // Keep exactly one step of undo.
        if (!last_backup.empty() && last_backup != res.backup) DeleteFileW(last_backup.c_str());
        last_backup = res.backup;
        Reload();
        return PULSE_OK;
    }

    PulseStatus Restore(const pulse::sdk::VfsOperation& op) {
        std::error_code ec;
        if (last_backup.empty() || !fs::exists(last_backup, ec)) {
            op.Message("Nothing to undo");
            return PULSE_ERROR_STATE;
        }
        if (!ReplaceFileW(archive_path.c_str(), last_backup.c_str(), nullptr,
                          REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr)) {
            // Across volumes ReplaceFile cannot move; copy over the archive instead.
            if (!CopyFileW(last_backup.c_str(), archive_path.c_str(), FALSE)) {
                const DWORD error = GetLastError();
                op.Message(error == ERROR_SHARING_VIOLATION ? "The archive is in use by another program"
                                                            : "Cannot restore the archive (Win32 error " + std::to_string(error) + ")");
                return PULSE_ERROR_STATE;
            }
            DeleteFileW(last_backup.c_str());
        }
        last_backup.clear();
        Reload();
        return PULSE_OK;
    }

    ~MountData() {
        if (!last_backup.empty()) DeleteFileW(last_backup.c_str());
    }

    std::shared_ptr<const Tree> BuildTree(const engine::Listing& listing) const {
        auto built = std::make_shared<Tree>();
        Node root;
        root.entry.name = display_name;
        root.entry.flags = PULSE_VFS_ENTRY_DIRECTORY | ReadonlyFlag();
        built->nodes.push_back(std::move(root));

        std::vector<std::wstring> wide_segs;
        std::vector<std::string> utf8_segs;
        std::vector<std::string> lower_segs;

        for (const auto& item : listing.entries) {
            if (item.link || !engine::SafeRelativePath(item.path)) continue;
            if (!SplitArchiveEntrySegments(item.path, wide_segs, utf8_segs, lower_segs)) continue;

            size_t parent_idx = 0;
            std::string prefix_lower;
            for (size_t depth = 0; depth + 1 < utf8_segs.size(); ++depth) {
                if (!prefix_lower.empty()) prefix_lower.push_back('/');
                prefix_lower.append(lower_segs[depth]);

                auto& parent = built->nodes[parent_idx];
                auto it = parent.child_by_lower_name.find(lower_segs[depth]);
                if (it == parent.child_by_lower_name.end()) {
                    const size_t dir_idx = built->nodes.size();
                    Node folder;
                    folder.entry.name = utf8_segs[depth];
                    folder.entry.flags = PULSE_VFS_ENTRY_DIRECTORY | ReadonlyFlag();
                    folder.lower_rel_path = prefix_lower;
                    built->nodes.push_back(std::move(folder));
                    built->nodes[parent_idx].children.push_back(dir_idx);
                    built->nodes[parent_idx].child_by_lower_name.emplace(lower_segs[depth], dir_idx);
                    built->by_lower_path.emplace(prefix_lower, dir_idx);
                    parent_idx = dir_idx;
                } else {
                    parent_idx = it->second;
                }
            }

            if ((built->nodes[parent_idx].entry.flags & PULSE_VFS_ENTRY_DIRECTORY) == 0u) continue;
            if (!prefix_lower.empty()) prefix_lower.push_back('/');
            prefix_lower.append(lower_segs.back());
            if (prefix_lower.size() > 4096) continue;

            uint32_t flags = ReadonlyFlag();
            if (item.dir) flags |= PULSE_VFS_ENTRY_DIRECTORY;
            if ((item.attributes & FILE_ATTRIBUTE_HIDDEN) != 0u) flags |= PULSE_VFS_ENTRY_HIDDEN;
            const int64_t mtime_ms = FileTimeToUnixMs(item.modified);

            auto existing = built->nodes[parent_idx].child_by_lower_name.find(lower_segs.back());
            if (existing != built->nodes[parent_idx].child_by_lower_name.end()) {
                Node& cur = built->nodes[existing->second];
                if (item.dir && (cur.entry.flags & PULSE_VFS_ENTRY_DIRECTORY) != 0u) {
                    if (cur.entry.modified_unix_ms <= 0 && mtime_ms > 0) cur.entry.modified_unix_ms = mtime_ms;
                    if ((flags & PULSE_VFS_ENTRY_HIDDEN) != 0u) cur.entry.flags |= PULSE_VFS_ENTRY_HIDDEN;
                }
                continue;
            }

            const size_t new_idx = built->nodes.size();
            Node node;
            node.entry.name = utf8_segs.back();
            node.entry.size_bytes = item.dir ? 0u : item.size;
            node.entry.modified_unix_ms = mtime_ms;
            node.entry.flags = flags;
            if (!item.dir) node.archive_entry_path = item.path;
            node.lower_rel_path = prefix_lower;
            built->nodes.push_back(std::move(node));
            built->nodes[parent_idx].children.push_back(new_idx);
            built->nodes[parent_idx].child_by_lower_name.emplace(lower_segs.back(), new_idx);
            built->by_lower_path.emplace(prefix_lower, new_idx);
        }
        return built;
    }

    PulseStatus EnsureLoaded(const std::function<bool()>& cancelled, std::shared_ptr<const Tree>& out_tree) {
        out_tree.reset();
        {
            std::unique_lock lock(mu);
            while (loading) {
                if (cancelled && cancelled()) return PULSE_ERROR_STATE;
                cv.wait(lock);
            }
            if (loaded) {
                if (load_status != engine::Status::Ok || !tree) return MapEngineStatus(load_status);
                out_tree = tree;
                return PULSE_OK;
            }
            loading = true;
        }

        engine::Budget budget = engine::Budget::PreviewBudget();
        budget.max_entries = 50000;
        budget.max_duration_ms = 15000;
        budget.max_total_bytes = 256ull << 20;
        budget.max_single_bytes = 256ull << 20;

        const engine::Listing listing = engine::List(archive_path, cancelled, {}, 50000, budget);
        if (listing.status == engine::Status::NeedPassword || listing.status == engine::Status::WrongPassword ||
            std::any_of(listing.entries.begin(), listing.entries.end(), [](const engine::Entry& e) { return e.encrypted; }))
            encrypted.store(true, std::memory_order_relaxed);
        std::shared_ptr<const Tree> built;
        if (listing.status == engine::Status::Ok) {
            built = BuildTree(listing);
        }

        {
            std::lock_guard lock(mu);
            loading = false;
            if (listing.status != engine::Status::Cancelled && (!cancelled || !cancelled())) {
                loaded = true;
                load_status = listing.status;
                tree = built;
            }
            cv.notify_all();
        }

        if (cancelled && cancelled()) return PULSE_ERROR_STATE;
        if (listing.status != engine::Status::Ok) return MapEngineStatus(listing.status);
        out_tree = std::move(built);
        return out_tree ? PULSE_OK : PULSE_ERROR_STATE;
    }

    PulseStatus ListDir(std::string_view rel_path,
                        const std::function<bool(const pulse::sdk::VfsEntryData&)>& emit,
                        const std::function<bool()>& cancelled) {
        if (cancelled && cancelled()) return PULSE_ERROR_STATE;
        std::vector<std::string> segs;
        std::string lower_rel;
        if (!NormalizeVfsRelPath(rel_path, segs, lower_rel)) return PULSE_ERROR_ARGUMENT;

        std::shared_ptr<const Tree> snapshot;
        const PulseStatus load_st = EnsureLoaded(cancelled, snapshot);
        if (load_st != PULSE_OK) return load_st;

        size_t dir_idx = 0;
        if (!lower_rel.empty()) {
            const auto it = snapshot->by_lower_path.find(lower_rel);
            if (it == snapshot->by_lower_path.end()) return PULSE_ERROR_ARGUMENT;
            dir_idx = it->second;
            if ((snapshot->nodes[dir_idx].entry.flags & PULSE_VFS_ENTRY_DIRECTORY) == 0u) return PULSE_ERROR_ARGUMENT;
        }

        for (const size_t child_idx : snapshot->nodes[dir_idx].children) {
            if (cancelled && cancelled()) return PULSE_ERROR_STATE;
            if (!emit(snapshot->nodes[child_idx].entry)) return PULSE_ERROR_STATE;
        }
        return PULSE_OK;
    }

    PulseStatus Stat(std::string_view rel_path,
                     pulse::sdk::VfsEntryData& out_entry,
                     const std::function<bool()>& cancelled) {
        if (cancelled && cancelled()) return PULSE_ERROR_STATE;
        std::vector<std::string> segs;
        std::string lower_rel;
        if (!NormalizeVfsRelPath(rel_path, segs, lower_rel)) return PULSE_ERROR_ARGUMENT;

        std::shared_ptr<const Tree> snapshot;
        const PulseStatus load_st = EnsureLoaded(cancelled, snapshot);
        if (load_st != PULSE_OK) return load_st;

        if (lower_rel.empty()) {
            out_entry = snapshot->nodes[0].entry;
            return PULSE_OK;
        }
        const auto it = snapshot->by_lower_path.find(lower_rel);
        if (it == snapshot->by_lower_path.end()) return PULSE_ERROR_ARGUMENT;
        out_entry = snapshot->nodes[it->second].entry;
        return PULSE_OK;
    }

    PulseStatus ExtractFile(std::string_view rel_path,
                            std::string_view dest_path_utf8,
                            const std::function<bool()>& cancelled) {
        if (cancelled && cancelled()) return PULSE_ERROR_STATE;
        std::vector<std::string> segs;
        std::string lower_rel;
        if (!NormalizeVfsRelPath(rel_path, segs, lower_rel) || lower_rel.empty()) return PULSE_ERROR_ARGUMENT;

        const std::wstring dest_wide = Utf8ToWide(dest_path_utf8);
        if (dest_wide.empty()) return PULSE_ERROR_ARGUMENT;

        std::shared_ptr<const Tree> snapshot;
        const PulseStatus load_st = EnsureLoaded(cancelled, snapshot);
        if (load_st != PULSE_OK) return load_st;

        const auto it = snapshot->by_lower_path.find(lower_rel);
        if (it == snapshot->by_lower_path.end()) return PULSE_ERROR_ARGUMENT;
        const Node& node = snapshot->nodes[it->second];
        if ((node.entry.flags & PULSE_VFS_ENTRY_DIRECTORY) != 0u || node.archive_entry_path.empty()) {
            return PULSE_ERROR_ARGUMENT;
        }

        const fs::path dest_path(dest_wide);
        const fs::path parent = dest_path.parent_path();
        std::error_code ec;
        if (!parent.empty()) {
            fs::create_directories(parent, ec);
            if (ec) return PULSE_ERROR_STATE;
        }

        // Extract into a fresh non-existent sibling path first, then atomically replace dest_path
        // so ExtractEntry's CREATE_NEW invariant holds even when callers pass a pre-created temp file.
        fs::path staged_path;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const uint64_t seq = ++g_stage_sequence;
            const std::wstring name = L".7zvfs-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                      std::to_wstring(seq) + L".tmp";
            const fs::path candidate = parent.empty() ? fs::path(name) : (parent / name);
            if (!fs::exists(candidate, ec)) {
                staged_path = candidate;
                break;
            }
        }
        if (staged_path.empty()) return PULSE_ERROR_STATE;

        engine::Budget extract_budget = engine::Budget::DefaultExtractBudget();
        extract_budget.max_single_bytes = 512ull << 20;
        extract_budget.max_total_bytes = 512ull << 20;
        extract_budget.max_duration_ms = 60000;

        const engine::ExtractResult res = engine::ExtractEntry(
            archive_path, node.archive_entry_path, staged_path.wstring(),
            cancelled, {}, {}, extract_budget);
        if (res.status != engine::Status::Ok) {
            DeleteFileW(staged_path.c_str());
            return MapEngineStatus(res.status);
        }
        if (cancelled && cancelled()) {
            DeleteFileW(staged_path.c_str());
            return PULSE_ERROR_STATE;
        }
        if (!MoveFileExW(staged_path.c_str(), dest_wide.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(staged_path.c_str());
            return PULSE_ERROR_STATE;
        }
        return PULSE_OK;
    }
};

ArchiveVfsManager::~ArchiveVfsManager() {
    Clear();
}

bool ArchiveVfsManager::Initialize(const pulse::sdk::Host& host) {
    std::lock_guard lock(mutex_);
    mounts_.clear();
    auto client = pulse::sdk::VfsClient::TryQuery(host);
    if (!client) {
        vfs_ = pulse::sdk::VfsClient{};
        return false;
    }
    vfs_ = std::move(*client);
    return vfs_.Available();
}

bool ArchiveVfsManager::Initialize(const PulseVfsV1& table) {
    std::lock_guard lock(mutex_);
    mounts_.clear();
    vfs_ = pulse::sdk::VfsClient(table);
    return vfs_.Available();
}

void ArchiveVfsManager::Clear() noexcept {
    std::vector<MountRecord> old;
    {
        std::lock_guard lock(mutex_);
        old = std::move(mounts_);
        mounts_.clear();
        vfs_ = pulse::sdk::VfsClient{};
    }
    // MountHandle destructors unregister out-of-lock so callbacks drain cleanly.
}

bool ArchiveVfsManager::Available() const noexcept {
    std::lock_guard lock(mutex_);
    return vfs_.Available();
}

size_t ArchiveVfsManager::MountCount() const noexcept {
    std::lock_guard lock(mutex_);
    return mounts_.size();
}

PulseStatus ArchiveVfsManager::OpenArchive(const std::wstring& archive_path, bool new_tab) {
    if (archive_path.empty()) return PULSE_ERROR_ARGUMENT;
    const std::wstring key = CanonicalArchiveKey(archive_path);
    const std::string display_name = MakeDisplayName(archive_path);
    const uint32_t open_mode = new_tab ? PULSE_VFS_OPEN_NEW_TAB : PULSE_VFS_OPEN_CURRENT_TAB;

    std::vector<MountRecord> evicted;
    std::lock_guard lock(mutex_);
    if (!vfs_.Available()) return PULSE_ERROR_UNAVAILABLE;

    for (size_t i = 0; i < mounts_.size(); ++i) {
        if (mounts_[i].canonical_key == key) {
            if (i + 1 < mounts_.size()) {
                MountRecord rec = std::move(mounts_[i]);
                mounts_.erase(mounts_.begin() + static_cast<std::ptrdiff_t>(i));
                mounts_.push_back(std::move(rec));
            }
            return mounts_.back().handle.Open({}, open_mode);
        }
    }

    const std::string key_utf8 = WideToUtf8(key);
    std::string mount_id;
    for (size_t suffix = 0;; ++suffix) {
        mount_id = MakeMountId(key_utf8, suffix);
        const bool collision = std::any_of(mounts_.begin(), mounts_.end(), [&](const MountRecord& m) {
            return m.mount_id == mount_id;
        });
        if (!collision) break;
    }

    if (mounts_.size() >= kMaxMounts) {
        evicted.push_back(std::move(mounts_.front()));
        mounts_.erase(mounts_.begin());
    }

    auto state = std::make_shared<MountData>(archive_path, display_name);
    pulse::sdk::VfsProviderCallbacks cb;
    cb.capabilities = PULSE_VFS_CAP_LIST_DIR | PULSE_VFS_CAP_EXTRACT_FILE | PULSE_VFS_CAP_STAT | PULSE_VFS_CAP_EXTRACT_ITEMS |
                      (state->writable ? PULSE_VFS_CAP_MODIFY : PULSE_VFS_CAP_READONLY);
    cb.icon_glyph = "icon:box";
    cb.list_dir = [state](std::string_view rel_path,
                          const std::function<bool(const pulse::sdk::VfsEntryData&)>& emit,
                          const std::function<bool()>& cancelled) {
        return state->ListDir(rel_path, emit, cancelled);
    };
    cb.extract_file = [state](std::string_view rel_path,
                              std::string_view dest_path_utf8,
                              const std::function<bool()>& cancelled) {
        return state->ExtractFile(rel_path, dest_path_utf8, cancelled);
    };
    cb.stat = [state](std::string_view rel_path,
                      pulse::sdk::VfsEntryData& out_entry,
                      const std::function<bool()>& cancelled) {
        return state->Stat(rel_path, out_entry, cancelled);
    };

    cb.extract_items = [state](const std::vector<std::string_view>& rel_paths, std::string_view dest_dir_utf8,
                               const pulse::sdk::VfsOperation& op) {
        return state->ExtractItems(rel_paths, dest_dir_utf8, op);
    };
    if (state->writable) {
        cb.modify = [state](const std::vector<pulse::sdk::VfsChange>& changes, const pulse::sdk::VfsOperation& op) {
            return state->Modify(changes, op);
        };
    }

    pulse::sdk::VfsClient::MountHandle handle;
    const PulseStatus reg_st = vfs_.RegisterProvider(mount_id, display_name, std::move(cb), handle);
    if (reg_st != PULSE_OK) return reg_st;

    const PulseStatus open_st = handle.Open({}, open_mode);
    if (open_st != PULSE_OK) return open_st;

    MountRecord record;
    record.canonical_key = key;
    record.mount_id = std::move(mount_id);
    record.data = std::move(state);
    record.handle = std::move(handle);
    mounts_.push_back(std::move(record));
    return PULSE_OK;
}