// direct_reveal_targets.cpp — see direct_reveal_targets.h.
#include "direct_reveal_targets.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <mutex>
#include <set>

namespace pulse::app::direct_reveal_targets {
namespace {

constexpr size_t kMaxLearned = 64;
constexpr std::wstring_view kBuiltIn[]{L"Weixin.exe", L"WeChat.exe", L"Alma.exe"};

std::wstring Fold(std::wstring_view text) {
    std::wstring folded(text);
    if (!folded.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, folded.data(), static_cast<int>(folded.size()),
                      folded.data(), static_cast<int>(folded.size()), nullptr, nullptr, 0);
    return folded;
}

bool SameText(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring_view FileName(std::wstring_view path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

std::wstring DefaultStorePath() {
    PWSTR local = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local)) && local)
        path = std::wstring(local) + L"\\Pulse\\direct-reveal-apps.txt";
    CoTaskMemFree(local);
    return path;
}

struct Store {
    std::mutex mutex;
    bool loaded = false;
    std::wstring path_override;
    std::vector<std::wstring> paths;
    std::set<std::wstring> names;   // folded file names

    std::wstring Path() const { return path_override.empty() ? DefaultStorePath() : path_override; }

    void AddLocked(const std::wstring& image_path) {
        paths.push_back(image_path);
        names.insert(Fold(FileName(image_path)));
    }

    void LoadLocked() {
        if (loaded) return;
        loaded = true;
        const std::wstring file = Path();
        if (file.empty()) return;
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size{};
        std::string bytes;
        if (GetFileSizeEx(handle, &size) && size.QuadPart > 0 && size.QuadPart <= 256 * 1024) {
            bytes.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            if (!ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) read = 0;
            bytes.resize(read);
        }
        CloseHandle(handle);
        const int chars = bytes.empty() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                                                   static_cast<int>(bytes.size()), nullptr, 0);
        if (chars <= 0) return;
        std::wstring text(static_cast<size_t>(chars), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()),
                            text.data(), chars);
        size_t start = 0;
        while (start < text.size() && paths.size() < kMaxLearned) {
            size_t end = text.find(L'\n', start);
            if (end == std::wstring::npos) end = text.size();
            std::wstring line = text.substr(start, end - start);
            while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
            if (Learnable(line) && !names.contains(Fold(FileName(line)))) AddLocked(line);
            start = end + 1;
        }
    }

    bool SaveLocked() const {
        const std::wstring file = Path();
        if (file.empty()) return false;
        const size_t slash = file.find_last_of(L'\\');
        if (slash != std::wstring::npos) SHCreateDirectoryExW(nullptr, file.substr(0, slash).c_str(), nullptr);
        std::wstring text;
        for (const auto& path : paths) text += path + L"\r\n";
        std::string bytes;
        const int length = text.empty() ? 0 : WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                                                  nullptr, 0, nullptr, nullptr);
        if (length > 0) {
            bytes.resize(static_cast<size_t>(length));
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), length,
                                nullptr, nullptr);
        }
        const std::wstring temp = file + L".tmp";
        HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                        written == bytes.size();
        CloseHandle(handle);
        if (!ok || !MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temp.c_str());
            return false;
        }
        return true;
    }
};

Store& State() {
    static Store store;
    return store;
}

} // namespace

bool IsBuiltIn(std::wstring_view image_name) noexcept {
    for (auto name : kBuiltIn) if (SameText(image_name, name)) return true;
    return false;
}

bool IsTarget(std::wstring_view image_name) noexcept {
    if (IsBuiltIn(image_name)) return true;
    try {
        auto& store = State();
        std::lock_guard lock(store.mutex);
        store.LoadLocked();
        return store.names.contains(Fold(image_name));
    } catch (...) {
        return false;
    }
}

bool Learnable(std::wstring_view path) noexcept {
    try {
        const bool drive = path.size() > 7 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
                           path[1] == L':' && path[2] == L'\\';
        if (!drive || path.size() >= MAX_PATH * 4 || path.find(L"\\..\\") != path.npos ||
            path.find(L"\\.\\") != path.npos || path.find(L'/') != path.npos || path.find(L'\0') != path.npos)
            return false;
        const auto name = FileName(path);
        if (name.size() <= 4 || !SameText(name.substr(name.size() - 4), L".exe")) return false;
        if (SameText(name, L"explorer.exe") || (name.size() >= 5 && SameText(name.substr(0, 5), L"pulse"))) return false;
        wchar_t windows[MAX_PATH]{};
        const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
        if (!length || length >= MAX_PATH) return false;
        const std::wstring_view system(windows, length);
        if (path.size() > system.size() && path[system.size()] == L'\\' && SameText(path.substr(0, system.size()), system))
            return false;
        return true;
    } catch (...) {
        return false;
    }
}

bool Learn(const std::wstring& image_path) noexcept {
    if (!Learnable(image_path) || IsBuiltIn(FileName(image_path))) return false;
    try {
        auto& store = State();
        std::lock_guard lock(store.mutex);
        store.LoadLocked();
        if (store.paths.size() >= kMaxLearned || store.names.contains(Fold(FileName(image_path)))) return false;
        store.AddLocked(image_path);
        store.SaveLocked();   // in-memory learning stands even if the write fails
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<std::wstring> LearnedPaths() {
    auto& store = State();
    std::lock_guard lock(store.mutex);
    store.LoadLocked();
    return store.paths;
}

void SetStorePathForTest(std::wstring path) {
    auto& store = State();
    std::lock_guard lock(store.mutex);
    store.path_override = std::move(path);
    store.loaded = false;
    store.paths.clear();
    store.names.clear();
}

} // namespace pulse::app::direct_reveal_targets
