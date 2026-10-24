#include "pe_dependencies.h"
#include <windows.h>
#include <algorithm>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace pulse::preview {
namespace {
constexpr size_t kMaxNodes = 128;
constexpr size_t kMaxDepth = 8;
constexpr size_t kMaxBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxFileBytes = 8u * 1024u * 1024u;
constexpr size_t kTreeChars = 7800;
constexpr ULONGLONG kTimeMs = 500;

std::wstring Lower(std::wstring value) {
    if (!value.empty()) CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}
std::wstring Absolute(const std::wstring& path) {
    const DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!size || size > 32768) return {};
    std::wstring result(size, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), size, result.data(), nullptr);
    if (!written || written >= size) return {};
    result.resize(written);
    return result;
}
std::wstring Parent(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}
bool Local(const std::wstring& path) {
    std::wstring_view p(path);
    if (p.starts_with(L"\\\\?\\")) p.remove_prefix(4);
    if (p.size() < 3 || p[1] != L':' || p[2] != L'\\') return false;
    const std::wstring drive(p.substr(0, 3));
    return GetDriveTypeW(drive.c_str()) == DRIVE_FIXED;
}
bool Compatible(uint16_t target, uint16_t candidate) {
    return target == candidate ||
        (candidate == 0xa64e && (target == 0x8664 || target == 0xaa64 || target == 0xa641));
}
enum class Status { Ok, Missing, Warning, ApiSet, Cycle, Seen, Limit, Unresolved };
struct Node {
    std::wstring name, path, note;
    Status status = Status::Ok;
    bool delay = false, missing = false, incomplete = false;
    std::vector<Node> children;
};
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

class DependencyTree {
public:
    DependencyTree(const std::wstring& root, uint16_t machine)
        : machine_(machine), deadline_(GetTickCount64() + kTimeMs) {
        const auto absolute = Absolute(root);
        active_.insert(Lower(absolute));
        AddDirectory(Parent(absolute));
        wchar_t system[MAX_PATH]{}, windows[MAX_PATH]{};
        GetWindowsDirectoryW(windows, MAX_PATH);
        // GetSystemWow64Directory2 chooses the correct system directory on
        // ARM64 as well as x64, and is optional on older supported Windows.
        using SystemDirectoryForMachine = UINT(WINAPI*)(LPWSTR, UINT, WORD);
        const auto get_system = reinterpret_cast<SystemDirectoryForMachine>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemWow64Directory2W"));
        if (!get_system || !get_system(system, MAX_PATH, machine)) {
            if (machine == 0x14c) GetSystemWow64DirectoryW(system, MAX_PATH);
            if (!system[0]) GetSystemDirectoryW(system, MAX_PATH);
        }
        system_ = system;
        AddDirectory(system_);
        AddDirectory(windows);
        const DWORD count = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (count && count <= 32768) {
            std::wstring value(count, L'\0');
            const DWORD length = GetEnvironmentVariableW(L"PATH", value.data(), count);
            if (length && length < count) {
                value.resize(length);
                size_t start = 0;
                while (start < value.size() && directories_.size() < 64) {
                    const auto end = value.find(L';', start);
                    auto dir = value.substr(start, end == std::wstring::npos ? end : end - start);
                    if (dir.size() > 1 && dir.front() == L'"' && dir.back() == L'"')
                        dir = dir.substr(1, dir.size() - 2);
                    // Relative PATH entries depend on runtime working directory.
                    if (dir.size() > 2 && dir[1] == L':') AddDirectory(dir);
                    else if (!dir.empty()) skipped_paths_ = true;
                    if (end == std::wstring::npos) break;
                    start = end + 1;
                }
                if (start < value.size() && directories_.size() >= 64) skipped_paths_ = true;
            }
        }
        ReadKnownDlls(L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\KnownDLLs");
        if (machine == 0x14c)
            ReadKnownDlls(L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\KnownDLLs32");
    }

    std::wstring Build(const PeImports& imports, bool& truncated) {
        std::vector<Node> roots;
        for (const auto& module : imports.modules) {
            if (nodes_ >= kMaxNodes || Expired()) {
                roots.push_back({L"Remaining dependencies", {}, L"analysis budget reached", Status::Limit});
                limited_ = true;
                break;
            }
            roots.push_back(Resolve(module, 0));
        }
        std::wstring text = L"\nDLL Dependencies\n"
            L"  Static candidate paths; SxS, runtime DLL directories and API-set mapping may differ.\n"
            L"  Search: KnownDLLs, application folder, architecture system folder, Windows, PATH.\n";
        if (roots.empty()) text += L"  None recorded\n";
        for (const auto& node : roots) Render(node, 1, text);
        if (imports.incomplete) {
            text += L"  [UNRESOLVED] Import metadata is incomplete.\n";
            limited_ = true;
        }
        if (limited_) text += L"  [LIMIT] 128 nodes / depth 8 / 32 MiB; 500 ms soft budget between file operations.\n";
        truncated = truncated || limited_;
        return text;
    }

private:
    bool Expired() const { return GetTickCount64() >= deadline_; }
    void AddDirectory(const std::wstring& path) {
        if (path.empty()) return;
        const auto full = Absolute(path);
        if (full.empty() || !Local(full)) { skipped_paths_ = true; return; }
        const auto key = Lower(full);
        for (const auto& old : directories_) if (Lower(old) == key) return;
        directories_.push_back(full);
    }
    void ReadKnownDlls(const wchar_t* path) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
            return;
        for (DWORD i = 0; i < 256 && !Expired(); ++i) {
            wchar_t name[256]{}, value[512]{};
            DWORD name_size = 256, value_size = sizeof(value), type = 0;
            const LSTATUS status = RegEnumValueW(key, i, name, &name_size, nullptr, &type,
                reinterpret_cast<BYTE*>(value), &value_size);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status != ERROR_SUCCESS || type != REG_SZ || value_size < 2 || value_size > sizeof(value)) continue;
            value[511] = 0;
            std::wstring entry(value);
            if (entry.find_first_of(L"\\/:") == std::wstring::npos && Lower(entry).ends_with(L".dll"))
                known_dlls_.insert(Lower(entry));
        }
        RegCloseKey(key);
    }
    bool Read(const std::wstring& path, PeImports& imports, std::wstring& note) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            note = L"not a readable file"; return false;
        }
        if (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) {
            note = L"offline/cloud content not read"; return false;
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { note = L"reparse target not followed"; return false; }
        Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) { note = L"cannot read candidate"; return false; }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.value, &size) || size.QuadPart <= 0) { note = L"invalid file size"; return false; }
        if (static_cast<uint64_t>(size.QuadPart) > kMaxFileBytes ||
            static_cast<uint64_t>(size.QuadPart) > kMaxBytes - bytes_ || Expired()) {
            note = L"file/analysis byte or time limit"; limited_ = true; return false;
        }
        std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file.value, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size()) {
            note = L"incomplete file read"; return false;
        }
        bytes_ += data.size();
        if (!ReadPeImports(data, imports)) { note = L"invalid PE image"; return false; }
        if (!Compatible(machine_, imports.machine)) { note = L"architecture mismatch"; return false; }
        return true;
    }
    Node Resolve(const PeImport& module, size_t depth) {
        Node node;
        node.name = module.name;
        node.delay = module.delay && !module.normal;
        ++nodes_;
        const auto key = Lower(module.name);
        if (key.starts_with(L"api-ms-") || key.starts_with(L"ext-ms-")) {
            node.status = Status::ApiSet; node.incomplete = true;
            node.note = L"virtual contract; runtime mapping not evaluated"; return node;
        }
        if (depth >= kMaxDepth || Expired()) {
            node.status = Status::Limit; node.incomplete = true;
            node.note = L"depth/time limit"; limited_ = true; return node;
        }
        if (key.empty() || key == L"." || key == L".." || key.find_first_of(L"\\/:") != std::wstring::npos ||
            key.front() == L'<') {
            node.status = Status::Unresolved; node.incomplete = true;
            node.note = L"unsupported module name"; return node;
        }
        std::vector<std::wstring> search;
        if (known_dlls_.contains(key)) search.push_back(system_);
        else search = directories_;
        PeImports imports;
        bool unavailable = skipped_paths_;
        for (const auto& directory : search) {
            if (++probes_ > 2048 || Expired()) {
                node.status = Status::Limit; node.incomplete = true;
                node.note = L"search/time limit"; limited_ = true; return node;
            }
            const auto candidate = directory + L"\\" + module.name;
            const DWORD attributes = GetFileAttributesW(candidate.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) unavailable = true;
                continue;
            }
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            node.path = candidate;
            const auto path_key = Lower(candidate);
            if (active_.contains(path_key)) {
                node.status = Status::Cycle; node.incomplete = true; node.note = L"cycle"; return node;
            }
            if (const auto found = completed_.find(path_key); found != completed_.end()) {
                node.missing = found->second.first;
                node.incomplete = found->second.second;
                node.status = node.missing ? Status::Warning : node.incomplete ? Status::Unresolved : Status::Seen;
                node.note = node.missing ? L"missing descendant; already shown" :
                    node.incomplete ? L"dependency checks incomplete; already shown" : L"already shown";
                return node;
            }
            if (!Read(candidate, imports, node.note)) {
                node.status = Status::Unresolved;
                node.incomplete = true;
                return node;
            }
            active_.insert(path_key);
            for (const auto& child : imports.modules) {
                if (nodes_ >= kMaxNodes || Expired()) {
                    node.children.push_back({L"Remaining dependencies", {}, L"analysis budget reached", Status::Limit});
                    node.incomplete = true;
                    limited_ = true;
                    break;
                }
                node.children.push_back(Resolve(child, depth + 1));
                node.missing = node.missing || node.children.back().missing;
                node.incomplete = node.incomplete || node.children.back().incomplete;
            }
            active_.erase(path_key);
            node.incomplete = node.incomplete || imports.incomplete;
            completed_[path_key] = {node.missing, node.incomplete};
            node.status = node.missing ? Status::Warning : node.incomplete ? Status::Unresolved : Status::Ok;
            node.note = node.missing ? L"a dependency is missing" : node.incomplete ? L"dependency checks incomplete" : L"";
            if (imports.incomplete) {
                node.note += L"; import metadata incomplete";
                if (!node.missing) node.status = Status::Unresolved;
                limited_ = true;
            }
            return node;
        }
        node.status = unavailable ? Status::Unresolved : Status::Missing;
        node.note = unavailable ? L"not found; some search locations unavailable" : L"not found in static search locations";
        node.missing = !unavailable;
        node.incomplete = unavailable;
        return node;
    }
    void Render(const Node& node, size_t depth, std::wstring& text) {
        const wchar_t* status = L"OK";
        switch (node.status) {
        case Status::Missing: status = L"MISSING"; break;
        case Status::Warning: status = L"WARNING"; break;
        case Status::ApiSet: status = L"API SET"; break;
        case Status::Cycle: status = L"CYCLE"; break;
        case Status::Seen: status = L"SEEN"; break;
        case Status::Limit: status = L"LIMIT"; break;
        case Status::Unresolved: status = L"UNRESOLVED"; break;
        default: break;
        }
        std::wstring line(depth * 2, L' ');
        line += L"[" + std::wstring(status) + L"] " + node.name;
        if (node.delay) line += L" (delay-loaded)";
        if (!node.path.empty()) line += L" — " + node.path;
        if (!node.note.empty()) line += L" (" + node.note + L")";
        line += L"\n";
        if (text.size() + line.size() > kTreeChars) { limited_ = true; return; }
        text += line;
        for (const auto& child : node.children) Render(child, depth + 1, text);
    }

    uint16_t machine_;
    ULONGLONG deadline_;
    size_t nodes_ = 0, probes_ = 0, bytes_ = 0;
    bool limited_ = false, skipped_paths_ = false;
    std::wstring system_;
    std::vector<std::wstring> directories_;
    std::set<std::wstring> known_dlls_, active_;
    std::map<std::wstring, std::pair<bool, bool>> completed_;
};
} // namespace

bool MakePeDocument(const std::wstring& path, std::span<const uint8_t> bytes,
                    std::wstring& text, bool& truncated) {
    // Reserve space for the tree so the Imports/Exports tabs retain their data.
    if (!MakePeDocument(bytes, text, truncated, 24000)) return false;
    PeImports imports;
    if (!ReadPeImports(bytes, imports)) return true;
    const auto start = text.find(L"\nDLL Dependencies\n");
    const auto end = text.find(L"\nVersion information", start);
    if (start == std::wstring::npos || end == std::wstring::npos) return true;
    auto tree = DependencyTree(path, imports.machine).Build(imports, truncated);
    text.replace(start, end - start, tree);
    return true;
}
} // namespace pulse::preview
