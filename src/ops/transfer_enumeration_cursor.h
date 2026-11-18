#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <string_view>
#include <utility>
namespace pulse::ops {
// Resumable pre-order enumeration: 64 entries initially, then up to 1024 per refill.
// Refills may overlap copies; worker jobs own paths and retain only journal indices.
// Only the unexecuted discovery batch is bounded;
// the caller's operation journal still grows with input. Cursor mappings retain
// only open ancestors and the one pending descent (O(maximum directory depth)).
class TransferEnumerationCursor {
    struct Folder {
        HANDLE find = INVALID_HANDLE_VALUE;
        std::wstring source, destination;
        WIN32_FIND_DATAW first{};
        bool pending = true;
        Folder(HANDLE handle, std::wstring path, std::wstring target, const WIN32_FIND_DATAW& data)
            : find(handle), source(std::move(path)), destination(std::move(target)), first(data) {}
        Folder(Folder&& other) noexcept : find(std::exchange(other.find, INVALID_HANDLE_VALUE)),
            source(std::move(other.source)), destination(std::move(other.destination)),
            first(other.first), pending(other.pending) {}
        Folder(const Folder&) = delete;
        ~Folder() { if (find != INVALID_HANDLE_VALUE) FindClose(find); }
    };
public:
    static constexpr size_t InitialBatchLimit = 64;
    static constexpr size_t RefillBatchLimit = 1024;
    struct Item { std::wstring source, destination; WIN32_FIND_DATAW metadata{}; };
    bool Start(const std::wstring& source, const std::wstring& destination) {
        return Open(source, destination);
    }
    void RemapDirectory(const std::wstring& source, const std::wstring& destination) {
        // Remapping happens at refill boundaries, not on the per-file hot path.
        if (pending_descent_ == source) pending_destination_ = destination;
        for (auto& folder : folders_)
            if (folder.source == source) { folder.destination = destination; break; }
    }
    size_t PendingDirectories() const noexcept { return folders_.size() + (pending_descent_.empty() ? 0 : 1); }
    bool Complete() const noexcept { return folders_.empty() && pending_descent_.empty(); }
    DWORD Error() const noexcept { return error_; }
    const std::wstring& ErrorPath() const noexcept { return error_path_; }
    void Close() { folders_.clear(); pending_descent_.clear(); pending_destination_.clear(); }
    // Returns false on EOF or error. Check Error; cancellation is owned by caller.
    bool Next(Item& item) {
#ifdef PULSE_COPY_STREAM_TESTING
        wchar_t inject[2]{};
        if (++test_next_calls_ == 1100 && GetEnvironmentVariableW(L"PULSE_TEST_LATE_ENUM_FAILURE", inject, 2) == 1 && inject[0] == L'1') {
            error_ = ERROR_ACCESS_DENIED;
            error_path_ = folders_.empty() ? std::wstring{} : folders_.back().source;
            Close();
            return false;
        }
#endif
        if (!pending_descent_.empty()) {
            auto next = std::exchange(pending_descent_, {});
            auto destination = std::exchange(pending_destination_, {});
            if (!Open(next, destination)) return false;
        }
        while (!folders_.empty()) {
            auto& folder = folders_.back();
            auto& data = item.metadata;
            if (folder.pending) { data = folder.first; folder.pending = false; }
            else if (!FindNextFileW(folder.find, &data)) {
                const DWORD code = GetLastError();
                if (code != ERROR_NO_MORE_FILES) { error_ = code; error_path_ = folder.source; Close(); return false; }
                folders_.pop_back(); continue;
            }
            const wchar_t* name = data.cFileName;
            if (name[0] == L'.' && (!name[1] || (name[1] == L'.' && !name[2]))) continue;
            item.source = Join(folder.source, name);
            item.destination = Join(folder.destination, name);
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                pending_descent_ = item.source;
                pending_destination_ = item.destination;
            }
            return true;
        }
        return false;
    }
private:
    static std::wstring Join(const std::wstring& folder, std::wstring_view name) {
        std::wstring path;
        path.reserve(folder.size() + 1 + name.size());
        path.append(folder);
        if (!folder.empty() && folder.back() != L'\\') path.push_back(L'\\');
        path.append(name);
        return path;
    }
    bool Open(const std::wstring& source, const std::wstring& destination) {
        std::wstring path = source;
        if (path.rfind(L"\\\\?\\", 0) != 0) {
            if (path.rfind(L"\\\\", 0) == 0) path = L"\\\\?\\UNC\\" + path.substr(2);
            else if (path.size() >= 3 && path[1] == L':') path = L"\\\\?\\" + path;
        }
        WIN32_FIND_DATAW data{};
        const HANDLE find = FindFirstFileExW(Join(path, L"*").c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_NO_MORE_FILES) {
                return true;
            }
            error_ = code; error_path_ = source; Close(); return false;
        }
        folders_.emplace_back(find, source, destination, data);
        return true;
    }
    std::vector<Folder> folders_;
    std::wstring pending_descent_, pending_destination_, error_path_;
#ifdef PULSE_COPY_STREAM_TESTING
    size_t test_next_calls_ = 0;
#endif
    DWORD error_ = ERROR_SUCCESS;
};
}
