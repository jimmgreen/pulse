#include "resource_settings.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <utility>
#include <vector>
#include <windows.h>
namespace pulse::extensions {
namespace {
constexpr size_t kMaximumFileBytes = 16 * 1024;
constexpr uint64_t kMaximumVersion = (uint64_t{1} << 53) - 1;
std::mutex settings_mutex;
std::atomic<uint64_t> temporary_serial{0};
class Handle {
  public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    Handle(Handle &&other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle &operator=(Handle &&other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_HANDLE_VALUE) {
                CloseHandle(value_);
            }
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE Get() const { return value_; }
    explicit operator bool() const { return value_ != INVALID_HANDLE_VALUE; }

  private:
    HANDLE value_;
};
json::Value Error(const char *code) {
    auto error = json::Value::Object();
    error.set("code", code);
    auto result = json::Value::Object();
    result.set("ok", false);
    result.set("error", std::move(error));
    return result;
}
json::Value Success(json::Value data) {
    auto result = json::Value::Object();
    result.set("ok", true);
    result.set("data", std::move(data));
    return result;
}
bool Identifier(std::string_view value) {
    return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
               c == '_' || c == '-';
    });
}
bool Utf8Value(const std::string &value) {
    if (value.size() > 1024) {
        return false;
    }
    json::Value parsed;
    return json::Parse(json::Serialize(json::Value(value)), parsed);
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
std::wstring Extended(const std::wstring &path) {
    return path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
}
bool Normalize(const std::wstring &input, std::wstring &result) {
    if (input.empty() || input.find(L'\0') != std::wstring::npos) {
        return false;
    }
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
    if (!size) {
        return false;
    }
    result.resize(size);
    const DWORD written = GetFullPathNameW(path.c_str(), size, result.data(), nullptr);
    if (!written || written >= size) {
        return false;
    }
    result.resize(written);
    while (result.size() > 3 && result.back() == L'\\') {
        result.pop_back();
    }
    return true;
}
bool SafeFile(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(file, &info) &&
           !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
           info.nNumberOfLinks == 1;
}
struct Paths {
    std::vector<Handle> directories;
    std::wstring directory, target, lock;
    const char *error = "io_error";
    bool Pin(const std::wstring &path) {
        // Deny directory writes/deletion while traversing and committing: a checked
        // ancestor cannot be renamed or changed into a junction underneath us.
        Handle handle(CreateFileW(Extended(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr));
        if (!handle) {
            return false;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle.Get(), &info)) {
            return false;
        }
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            error = "unsafe_path";
            return false;
        }
        directories.push_back(std::move(handle));
        return true;
    }
    bool Prepare(const std::wstring &root, std::string_view package) {
        std::wstring absolute;
        if (!Normalize(root, absolute)) {
            error = "invalid_argument";
            return false;
        }
        size_t root_end = 3;
        if (absolute.starts_with(L"\\\\")) {
            const auto server = absolute.find(L'\\', 2);
            if (server == std::wstring::npos) {
                error = "invalid_argument";
                return false;
            }
            const auto share = absolute.find(L'\\', server + 1);
            root_end = share == std::wstring::npos ? absolute.size() : share;
        }
        if (!Pin(absolute.substr(0, root_end))) {
            return false;
        }
        size_t end = root_end;
        while (end < absolute.size()) {
            if (absolute[end] == L'\\') {
                ++end;
            }
            const auto next = absolute.find(L'\\', end);
            const auto limit = next == std::wstring::npos ? absolute.size() : next;
            if (limit > end && !Pin(absolute.substr(0, limit))) {
                return false;
            }
            end = limit;
        }
        directory = absolute + (absolute.back() == L'\\' ? L"" : L"\\") + L"extension-settings";
        if (!CreateDirectoryW(Extended(directory).c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
        if (!Pin(directory)) {
            return false;
        }
        const auto stem = PackageStem(package);
        target = Extended(directory + L"\\" + stem + L".json");
        lock = Extended(directory + L"\\" + stem + L".lock");
        return true;
    }
};
class FileLock {
  public:
    bool Acquire(const std::wstring &path) {
        file_ =
            Handle(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file_) {
            return false;
        }
        if (!SafeFile(file_.Get())) {
            unsafe = true;
            return false;
        }
        locked_ = LockFileEx(file_.Get(), LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &position_) != FALSE;
        return locked_;
    }
    ~FileLock() {
        if (locked_) {
            UnlockFileEx(file_.Get(), 0, 1, 0, &position_);
        }
    }
    bool unsafe = false;

  private:
    Handle file_;
    OVERLAPPED position_{};
    bool locked_ = false;
};
struct Stored {
    uint64_t version = 0;
    json::Value values = json::Value::Object();
};
const char *Load(const std::wstring &path, Stored &stored) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? nullptr : "io_error";
    }
    if (!SafeFile(file.Get())) {
        return "unsafe_path";
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.Get(), &size)) {
        return "io_error";
    }
    if (size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(kMaximumFileBytes)) {
        return "corrupt_data";
    }
    std::string content(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file.Get(), content.data(), static_cast<DWORD>(content.size()), &read, nullptr) ||
        read != content.size()) {
        return "io_error";
    }
    json::Value document;
    int64_t version = 0;
    if (!json::Parse(content, document, nullptr, {kMaximumFileBytes, 3}) || !document.is_object() ||
        document.size() != 2 || !document.find("version") ||
        !document.find("version")->get_int(version, 0, static_cast<int64_t>(kMaximumVersion))) {
        return "corrupt_data";
    }
    const auto *values = document.find("values");
    if (!values || !values->is_object() || values->size() > 64) {
        return "corrupt_data";
    }
    for (const auto &key : values->keys()) {
        const auto *value = values->find(key);
        if (!Identifier(key) || !value->is_string() || !Utf8Value(value->as_string())) {
            return "corrupt_data";
        }
    }
    stored.version = static_cast<uint64_t>(version);
    stored.values = *values;
    return nullptr;
}
json::Value Data(const Stored &stored) {
    auto data = json::Value::Object();
    data.set("version", stored.version);
    data.set("values", stored.values);
    return data;
}
class Temporary {
  public:
    Handle file;
    bool committed = false;
    ~Temporary() {
        if (file && !committed) {
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;
            SetFileInformationByHandle(file.Get(), FileDispositionInfo, &disposition, sizeof(disposition));
        }
    }
};
bool Authorized(const std::function<bool()> &authorized) {
    try {
        return authorized && authorized();
    } catch (...) {
        return false;
    }
}
const char *Commit(const Paths &paths, const std::string &content, const std::function<bool()> &authorized) {
    if (!Authorized(authorized)) {
        return "not_authorized";
    }
    Temporary temp;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto name = Extended(paths.directory + L"\\write-" + std::to_wstring(GetCurrentProcessId()) +
                                   L"-" + std::to_wstring(GetTickCount64()) + L"-" +
                                   std::to_wstring(++temporary_serial) + L".tmp");
        temp.file = Handle(CreateFileW(
            name.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (temp.file) {
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            return "io_error";
        }
    }
    if (!temp.file) {
        return "io_error";
    }
    if (!SafeFile(temp.file.Get())) {
        return "unsafe_path";
    }
    DWORD written = 0;
    if (!WriteFile(temp.file.Get(), content.data(), static_cast<DWORD>(content.size()), &written, nullptr) ||
        written != content.size() || !FlushFileBuffers(temp.file.Get())) {
        return "io_error";
    }
    // Recheck the destination without following a link. The handle-based rename
    // moves exactly our flushed temporary object; it never reopens a temp pathname.
    {
        Handle target(CreateFileW(paths.target.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (target) {
            if (!SafeFile(target.Get())) {
                return "unsafe_path";
            }
        } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return "io_error";
        }
    }
    if (!Authorized(authorized)) {
        return "not_authorized";
    }
    const size_t bytes = sizeof(FILE_RENAME_INFO) + paths.target.size() * sizeof(wchar_t);
    std::vector<unsigned char> buffer(bytes, 0);
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(buffer.data());
    rename->ReplaceIfExists = TRUE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(paths.target.size() * sizeof(wchar_t));
    std::copy(paths.target.begin(), paths.target.end(), rename->FileName);
    if (!SetFileInformationByHandle(temp.file.Get(), FileRenameInfo, rename,
                                    static_cast<DWORD>(buffer.size()))) {
        return "io_error";
    }
    temp.committed = true;
    // Do not check authorization again and claim rollback after this commit point.
    return nullptr;
}
} // namespace
json::Value ReadOwnSettings(const std::wstring &root_directory, std::string_view package_id) {
    if (!Identifier(package_id)) {
        return Error("invalid_argument");
    }
    std::lock_guard guard(settings_mutex);
    Paths paths;
    if (!paths.Prepare(root_directory, package_id)) {
        return Error(paths.error);
    }
    FileLock lock;
    if (!lock.Acquire(paths.lock)) {
        return Error(lock.unsafe ? "unsafe_path" : "io_error");
    }
    Stored stored;
    if (const auto *error = Load(paths.target, stored)) {
        return Error(error);
    }
    return Success(Data(stored));
}
json::Value WriteOwnSetting(const std::wstring &root_directory, std::string_view package_id,
                            std::string_view key, const std::optional<std::string> &value,
                            uint64_t expected_version, const std::function<bool()> &authorized) {
    if (!Identifier(package_id) || !Identifier(key) || expected_version > kMaximumVersion ||
        (value && !Utf8Value(*value))) {
        return Error("invalid_argument");
    }
    if (!Authorized(authorized)) {
        return Error("not_authorized");
    }
    std::lock_guard guard(settings_mutex);
    Paths paths;
    if (!paths.Prepare(root_directory, package_id)) {
        return Error(paths.error);
    }
    FileLock lock;
    if (!lock.Acquire(paths.lock)) {
        return Error(lock.unsafe ? "unsafe_path" : "io_error");
    }
    Stored stored;
    if (const auto *error = Load(paths.target, stored)) {
        return Error(error);
    }
    if (stored.version != expected_version) {
        return Error("version_conflict");
    }
    if (stored.version == kMaximumVersion) {
        return Error("version_exhausted");
    }
    auto changed = json::Value::Object();
    for (const auto &existing : stored.values.keys()) {
        if (existing != key) {
            changed.set(existing, *stored.values.find(existing));
        }
    }
    if (value) {
        changed.set(key, *value);
    }
    if (changed.size() > 64) {
        return Error("limit_exceeded");
    }
    stored.values = std::move(changed);
    ++stored.version;
    auto data = Data(stored);
    const auto content = json::Serialize(data);
    if (content.size() > kMaximumFileBytes) {
        return Error("limit_exceeded");
    }
    if (const auto *error = Commit(paths, content, authorized)) {
        return Error(error);
    }
    return Success(std::move(data));
}
} // namespace pulse::extensions
