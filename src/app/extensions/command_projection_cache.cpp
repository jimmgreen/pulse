#include "command_projection_cache.h"

#include "../../common/json_value.h"
#include "../../../sdk/native/include/pulse/commands.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace pulse::extensions {
namespace {

constexpr int64_t kSchemaVersion = 1;
constexpr size_t kMaximumFileBytes = 64 * 1024;
constexpr size_t kMaximumCommands = 32;
constexpr size_t kMaximumExtensions = 32;
constexpr size_t kMaximumSelection = 4096;

std::mutex g_file_mutex;
std::mutex g_cache_mutex;
std::atomic<uint64_t> g_temp_serial{0};

struct CacheSlot {
    bool warmed = false;
    bool reading = false;
    uint64_t generation = 0;
    std::optional<CommandProjectionSnapshot> snapshot;
};

auto& CacheSlots() {
    static std::unordered_map<std::string, CacheSlot> slots;
    return slots;
}

class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE value_;
};

bool ValidIdentifier(std::string_view value, size_t max_len = 64) noexcept {
    if (value.empty() || value.size() > max_len) return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
    });
}

bool ValidUtf8(std::string_view value, size_t max_bytes, bool allow_empty = false) noexcept {
    if (value.empty()) return allow_empty;
    if (value.size() > max_bytes) return false;
    if (std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; })) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                               static_cast<int>(value.size()), nullptr, 0) > 0;
}

bool ValidExtension(std::string_view ext) noexcept {
    if (ext.size() < 2 || ext.size() > 32 || ext.front() != '.') return false;
    if (!ValidIdentifier(ext, 32)) return false;
    return std::none_of(ext.begin(), ext.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
}

bool ValidKey(uint32_t key, uint32_t modifiers) noexcept {
    if ((modifiers & ~7u) != 0) return false;
    if (key == 0) return modifiers == 0;
    const bool valid_key = (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') ||
                           (key >= VK_F1 && key <= VK_F24);
    if (!valid_key) return false;
    if (key < VK_F1 && (modifiers & (PULSE_COMMAND_CTRL | PULSE_COMMAND_ALT)) == 0) return false;
    return true;
}

bool ValidCommand(const ProjectedCommand& cmd) noexcept {
    if (!ValidIdentifier(cmd.id, 64) || !ValidUtf8(cmd.title, 128) || !ValidUtf8(cmd.detail, 160, true) ||
        !ValidUtf8(cmd.group, 32, true) || !ValidUtf8(cmd.icon_name, 96, true) ||
        !ValidUtf8(cmd.group_icon_name, 96, true)) {
        return false;
    }
    if (cmd.kinds == 0 || (cmd.kinds & ~7u) != 0 || cmd.max_selection > kMaximumSelection ||
        cmd.min_selection > cmd.max_selection || (cmd.kinds == PULSE_COMMAND_BACKGROUND && cmd.min_selection > 0)) {
        return false;
    }
    if ((cmd.flags & ~static_cast<uint32_t>(PULSE_COMMAND_ALL_FLAGS)) != 0) return false;
    if (!ValidKey(cmd.virtual_key, cmd.modifiers)) return false;
    if (cmd.extensions.size() > kMaximumExtensions) return false;
    return std::all_of(cmd.extensions.begin(), cmd.extensions.end(),
                       [](const std::string& ext) { return ValidExtension(ext); });
}

bool ValidSnapshot(const CommandProjectionSnapshot& snapshot) noexcept {
    if (!ValidIdentifier(snapshot.package_id, 64) || !ValidUtf8(snapshot.revision, 128) ||
        !ValidUtf8(snapshot.language, 32) || snapshot.commands.size() > kMaximumCommands) {
        return false;
    }
    return std::all_of(snapshot.commands.begin(), snapshot.commands.end(),
                       [](const ProjectedCommand& cmd) { return ValidCommand(cmd); });
}

std::wstring PackageStem(std::string_view id) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring stem = L"pkg-";
    for (unsigned char c : id) {
        stem += digits[c >> 4];
        stem += digits[c & 15];
    }
    return stem;
}

std::wstring Extended(const std::wstring& path) {
    return path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
}

bool Normalize(const std::wstring& input, std::wstring& result) {
    if (input.empty() || input.find(L'\0') != std::wstring::npos) return false;
    std::wstring path = input;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.starts_with(L"\\\\?\\UNC\\")) {
        path = L"\\\\" + path.substr(8);
    } else if (path.starts_with(L"\\\\?\\")) {
        path = path.substr(4);
    }
    if (!((path.size() >= 3 && path[1] == L':' && path[2] == L'\\') ||
          (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\.\\")))) {
        return false;
    }
    const DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!size) return false;
    result.resize(size);
    const DWORD written = GetFullPathNameW(path.c_str(), size, result.data(), nullptr);
    if (!written || written >= size) return false;
    result.resize(written);
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return true;
}

bool SafeFile(HANDLE file) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(file, &info) &&
           !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
           info.nNumberOfLinks == 1;
}

struct CachePaths {
    std::vector<Handle> directories;
    std::wstring cache_dir;
    std::wstring target;
    bool missing_directory = false;

    bool Pin(const std::wstring& path) {
        Handle handle(CreateFileW(Extended(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!handle) return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle.Get(), &info)) return false;
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return false;
        }
        directories.push_back(std::move(handle));
        return true;
    }

    bool StepDirectory(const std::wstring& path, bool create) {
        if (create) {
            if (!CreateDirectoryW(Extended(path).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                return false;
            }
            return Pin(path);
        }
        Handle handle(CreateFileW(Extended(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!handle) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                missing_directory = true;
                return true;
            }
            return false;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle.Get(), &info)) return false;
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return false;
        }
        directories.push_back(std::move(handle));
        return true;
    }

    bool Prepare(const std::wstring& root, std::string_view package_id, bool create_directory) {
        if (!ValidIdentifier(package_id)) return false;
        std::wstring absolute;
        if (!Normalize(root, absolute)) return false;
        size_t root_end = 3;
        if (absolute.starts_with(L"\\\\")) {
            const auto server = absolute.find(L'\\', 2);
            if (server == std::wstring::npos) return false;
            const auto share = absolute.find(L'\\', server + 1);
            root_end = share == std::wstring::npos ? absolute.size() : share;
        }
        if (!Pin(absolute.substr(0, root_end))) return false;
        size_t end = root_end;
        while (end < absolute.size()) {
            if (absolute[end] == L'\\') ++end;
            const auto next = absolute.find(L'\\', end);
            const auto limit = next == std::wstring::npos ? absolute.size() : next;
            if (limit > end && !Pin(absolute.substr(0, limit))) return false;
            end = limit;
        }
        const std::wstring ext_dir = absolute + (absolute.back() == L'\\' ? L"" : L"\\") + L"extensions";
        if (!StepDirectory(ext_dir, create_directory)) return false;
        if (missing_directory) return true;
        cache_dir = ext_dir + L"\\command-cache";
        if (!StepDirectory(cache_dir, create_directory)) return false;
        if (missing_directory) return true;
        target = Extended(cache_dir + L"\\" + PackageStem(package_id) + L".json");
        return true;
    }
};

bool ParseUint32(const json::Value& object, std::string_view key, uint32_t& out, bool required = false) noexcept {
    const auto* value = object.find(key);
    if (!value) return !required;
    int64_t parsed = 0;
    if (!value->get_int(parsed, 0, UINT32_MAX)) return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}

std::optional<CommandProjectionSnapshot> DeserializeSnapshot(std::string_view content, std::string_view expected_pkg) {
    json::Value root;
    json::ParseLimits limits{};
    limits.max_bytes = kMaximumFileBytes;
    if (!json::Parse(content, root, nullptr, limits) || !root.is_object()) return std::nullopt;
    const auto* version = root.find("version");
    int64_t parsed_version = 0;
    if (!version || !version->get_int(parsed_version, kSchemaVersion, kSchemaVersion)) return std::nullopt;

    const auto* pkg = root.find("packageId");
    const auto* rev = root.find("revision");
    const auto* lang = root.find("language");
    const auto* cmds = root.find("commands");
    if (!pkg || !pkg->is_string() || pkg->as_string() != expected_pkg ||
        !rev || !rev->is_string() ||
        !lang || !lang->is_string() ||
        !cmds || !cmds->is_array() || cmds->size() > kMaximumCommands) {
        return std::nullopt;
    }

    CommandProjectionSnapshot snap;
    snap.package_id = pkg->as_string();
    snap.revision = rev->as_string();
    snap.language = lang->as_string();
    snap.commands.reserve(cmds->size());

    for (const auto& item : cmds->items()) {
        if (!item.is_object()) return std::nullopt;
        const auto* id = item.find("id");
        const auto* title = item.find("title");
        if (!id || !id->is_string() || !title || !title->is_string()) return std::nullopt;

        ProjectedCommand cmd;
        cmd.id = id->as_string();
        cmd.title = title->as_string();
        if (const auto* detail = item.find("detail")) {
            if (!detail->is_string()) return std::nullopt;
            cmd.detail = detail->as_string();
        }
        if (const auto* group = item.find("group")) {
            if (!group->is_string()) return std::nullopt;
            cmd.group = group->as_string();
        }
        if (const auto* icon_name = item.find("iconName")) {
            if (!icon_name->is_string()) return std::nullopt;
            cmd.icon_name = icon_name->as_string();
        }
        if (const auto* group_icon_name = item.find("groupIconName")) {
            if (!group_icon_name->is_string()) return std::nullopt;
            cmd.group_icon_name = group_icon_name->as_string();
        }
        if (!ParseUint32(item, "icon", cmd.icon) ||
            !ParseUint32(item, "groupIcon", cmd.group_icon) ||
            !ParseUint32(item, "kinds", cmd.kinds, true) ||
            !ParseUint32(item, "minSelection", cmd.min_selection) ||
            !ParseUint32(item, "maxSelection", cmd.max_selection, true) ||
            !ParseUint32(item, "virtualKey", cmd.virtual_key) ||
            !ParseUint32(item, "modifiers", cmd.modifiers) ||
            !ParseUint32(item, "flags", cmd.flags)) {
            return std::nullopt;
        }
        if (const auto* exclude = item.find("excludeExtensions")) {
            if (!exclude->is_bool()) return std::nullopt;
            cmd.exclude_extensions = exclude->as_bool();
        }
        if (const auto* exts = item.find("extensions")) {
            if (!exts->is_array() || exts->size() > kMaximumExtensions) return std::nullopt;
            cmd.extensions.reserve(exts->size());
            for (const auto& ext : exts->items()) {
                if (!ext.is_string()) return std::nullopt;
                cmd.extensions.push_back(ext.as_string());
            }
        }
        snap.commands.push_back(std::move(cmd));
    }

    if (!ValidSnapshot(snap)) return std::nullopt;
    return snap;
}

std::string SerializeSnapshot(const CommandProjectionSnapshot& snap) {
    auto cmds = json::Value::Array();
    for (const auto& cmd : snap.commands) {
        auto item = json::Value::Object();
        item.set("id", cmd.id);
        item.set("title", cmd.title);
        if (!cmd.detail.empty()) item.set("detail", cmd.detail);
        if (!cmd.group.empty()) item.set("group", cmd.group);
        if (cmd.icon != 0) item.set("icon", static_cast<uint64_t>(cmd.icon));
        if (!cmd.icon_name.empty()) item.set("iconName", cmd.icon_name);
        if (cmd.group_icon != 0) item.set("groupIcon", static_cast<uint64_t>(cmd.group_icon));
        if (!cmd.group_icon_name.empty()) item.set("groupIconName", cmd.group_icon_name);
        item.set("kinds", static_cast<uint64_t>(cmd.kinds));
        item.set("minSelection", static_cast<uint64_t>(cmd.min_selection));
        item.set("maxSelection", static_cast<uint64_t>(cmd.max_selection));
        if (!cmd.extensions.empty()) {
            auto exts = json::Value::Array();
            for (const auto& ext : cmd.extensions) exts.push(ext);
            item.set("extensions", std::move(exts));
        }
        if (cmd.exclude_extensions) item.set("excludeExtensions", true);
        if (cmd.virtual_key != 0) item.set("virtualKey", static_cast<uint64_t>(cmd.virtual_key));
        if (cmd.modifiers != 0) item.set("modifiers", static_cast<uint64_t>(cmd.modifiers));
        if (cmd.flags != 0) item.set("flags", static_cast<uint64_t>(cmd.flags));
        cmds.push(std::move(item));
    }
    auto root = json::Value::Object();
    root.set("version", kSchemaVersion);
    root.set("packageId", snap.package_id);
    root.set("revision", snap.revision);
    root.set("language", snap.language);
    root.set("commands", std::move(cmds));
    return json::Serialize(root);
}

bool WriteAll(HANDLE file, const std::string& content) noexcept {
    size_t offset = 0;
    while (offset < content.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(content.size() - offset, 65536u));
        DWORD written = 0;
        if (!WriteFile(file, content.data() + offset, chunk, &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}

} // namespace

std::optional<CommandProjectionSnapshot> ReadCommandProjectionFile(const std::wstring& root_directory,
                                                                   std::string_view package_id) {
    try {
        std::lock_guard lock(g_file_mutex);
        CachePaths paths;
        if (!paths.Prepare(root_directory, package_id, false) || paths.missing_directory) return std::nullopt;
        Handle file(CreateFileW(paths.target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file || !SafeFile(file.Get())) return std::nullopt;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart <= 0 ||
            size.QuadPart > static_cast<LONGLONG>(kMaximumFileBytes)) {
            return std::nullopt;
        }
        std::string content(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        if (!ReadFile(file.Get(), content.data(), static_cast<DWORD>(content.size()), &read, nullptr) ||
            read != content.size()) {
            return std::nullopt;
        }
        return DeserializeSnapshot(content, package_id);
    } catch (...) {
        return std::nullopt;
    }
}

bool WriteCommandProjectionFile(const std::wstring& root_directory, const CommandProjectionSnapshot& snapshot) {
    try {
        if (!ValidSnapshot(snapshot)) return false;
        const std::string content = SerializeSnapshot(snapshot);
        if (content.empty() || content.size() > kMaximumFileBytes) return false;

        std::lock_guard lock(g_file_mutex);
        CachePaths paths;
        if (!paths.Prepare(root_directory, snapshot.package_id, true)) return false;

        const uint64_t serial = g_temp_serial.fetch_add(1, std::memory_order_relaxed);
        const std::wstring temp = Extended(
            paths.cache_dir + L"\\tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetCurrentThreadId()) + L"-" + std::to_wstring(serial) + L".tmp");
        {
            Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!file) return false;
            if (!WriteAll(file.Get(), content)) {
                CloseHandle(file.Get());
                DeleteFileW(temp.c_str());
                return false;
            }
        }
        if (!MoveFileExW(temp.c_str(), paths.target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(temp.c_str());
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool PurgeCommandProjectionFile(const std::wstring& root_directory, std::string_view package_id) {
    try {
        std::lock_guard lock(g_file_mutex);
        CachePaths paths;
        if (!paths.Prepare(root_directory, package_id, false)) return false;
        if (paths.missing_directory) return true;
        Handle file(CreateFileW(paths.target.c_str(), FILE_READ_ATTRIBUTES | DELETE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file) return GetLastError() == ERROR_FILE_NOT_FOUND;
        if (!SafeFile(file.Get())) return false;
        FILE_DISPOSITION_INFO disposition{TRUE};
        return SetFileInformationByHandle(file.Get(), FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
    } catch (...) {
        return false;
    }
}

namespace command_projection {

std::optional<CommandProjectionSnapshot> Lookup(std::string_view package_id,
                                                std::string_view revision,
                                                std::string_view language) {
    std::lock_guard lock(g_cache_mutex);
    const auto it = CacheSlots().find(std::string(package_id));
    if (it == CacheSlots().end() || !it->second.snapshot) return std::nullopt;
    const auto& snap = *it->second.snapshot;
    if (snap.revision != revision || snap.language != language) return std::nullopt;
    return snap;
}

bool BeginWarmRead(std::string_view package_id, uint64_t& out_generation) {
    std::lock_guard lock(g_cache_mutex);
    auto& slot = CacheSlots()[std::string(package_id)];
    if (slot.warmed || slot.reading) {
        out_generation = slot.generation;
        return false;
    }
    slot.reading = true;
    out_generation = slot.generation;
    return true;
}

void PublishWarmRead(std::string_view package_id, uint64_t generation,
                     std::optional<CommandProjectionSnapshot> snapshot) {
    std::lock_guard lock(g_cache_mutex);
    auto& slot = CacheSlots()[std::string(package_id)];
    slot.reading = false;
    slot.warmed = true;
    if (slot.generation != generation) return;
    slot.snapshot = std::move(snapshot);
}

void PublishLive(CommandProjectionSnapshot snapshot) {
    if (!ValidSnapshot(snapshot)) return;
    std::lock_guard lock(g_cache_mutex);
    auto& slot = CacheSlots()[snapshot.package_id];
    ++slot.generation;
    slot.warmed = true;
    slot.reading = false;
    slot.snapshot = std::move(snapshot);
}

void Invalidate(std::string_view package_id) {
    std::lock_guard lock(g_cache_mutex);
    auto& slot = CacheSlots()[std::string(package_id)];
    ++slot.generation;
    slot.warmed = false;
    slot.reading = false;
    slot.snapshot.reset();
}

void ResetForTest() {
    std::lock_guard lock(g_cache_mutex);
    CacheSlots().clear();
}

} // namespace command_projection

} // namespace pulse::extensions
