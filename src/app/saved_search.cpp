#include "saved_search.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>

namespace pulse::app {
namespace {

constexpr size_t kMaximumSavedSearches = 100;

std::wstring Trim(std::wstring value) {
    const size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

const wchar_t* ModeName(SavedSearchMode mode) {
    if (mode == SavedSearchMode::Content) return L"content";
    if (mode == SavedSearchMode::Duplicates) return L"duplicates";
    return L"name";
}

bool ParseMode(const std::wstring& value, SavedSearchMode& mode) {
    if (value == L"name") mode = SavedSearchMode::Name;
    else if (value == L"content") mode = SavedSearchMode::Content;
    else if (value == L"duplicates") mode = SavedSearchMode::Duplicates;
    else return false;
    return true;
}

bool Normalize(SavedSearch& search) {
    search.name = Trim(std::move(search.name));
    search.root = Trim(std::move(search.root));
    if (search.name.empty() || search.name.size() > 128 || search.root.empty() ||
        search.root.size() > 32768 || search.query.size() > 4096) return false;
    if (search.mode != SavedSearchMode::Duplicates && search.query.empty()) return false;
    return true;
}

std::vector<std::wstring> ExtractObjects(const std::wstring& json) {
    std::vector<std::wstring> output;
    size_t pos = pulse::json::ValuePosition(json, L"searches");
    if (pos == std::wstring::npos || pos >= json.size() || json[pos] != L'[') return output;
    bool in_string = false;
    int depth = 0;
    size_t start = std::wstring::npos;
    for (size_t i = pos + 1; i < json.size(); ++i) {
        const wchar_t value = json[i];
        if (in_string) {
            if (value == L'\\') ++i;
            else if (value == L'"') in_string = false;
            continue;
        }
        if (value == L'"') in_string = true;
        else if (value == L'{') {
            if (depth++ == 0) start = i;
        } else if (value == L'}' && depth > 0) {
            if (--depth == 0 && start != std::wstring::npos) {
                output.push_back(json.substr(start, i - start + 1));
                start = std::wstring::npos;
            }
        } else if (value == L']' && depth == 0) {
            break;
        }
    }
    return output;
}

} // namespace

std::wstring SavedSearchStore::DefaultPath() {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                    nullptr, &local)) || !local) return {};
    const std::wstring root = std::wstring(local) + L"\\Pulse";
    CoTaskMemFree(local);
    if (!CreateDirectoryW(root.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return {};
    return root + L"\\saved_searches.json";
}

bool SavedSearchStore::Load() {
    const std::wstring path = DefaultPath();
    return !path.empty() && LoadFrom(path);
}

bool SavedSearchStore::Save() const {
    const std::wstring path = DefaultPath();
    return !path.empty() && SaveTo(path);
}

bool SavedSearchStore::LoadFrom(const std::wstring& path) {
    std::wstring document;
    if (!ReadUtf8File(path, document)) {
        items_.clear();
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    if (pulse::json::ExtractInt(document, L"schema", 0) != 1) return false;
    std::vector<SavedSearch> loaded;
    for (const std::wstring& object : ExtractObjects(document)) {
        SavedSearch search;
        search.name = pulse::json::ExtractString(object, L"name");
        search.root = pulse::json::ExtractString(object, L"root");
        search.query = pulse::json::ExtractString(object, L"query");
        search.recursive = pulse::json::ExtractBool(object, L"recursive", true);
        if (!ParseMode(pulse::json::ExtractString(object, L"mode"), search.mode) ||
            !Normalize(search)) continue;
        loaded.push_back(std::move(search));
        if (loaded.size() >= kMaximumSavedSearches) break;
    }
    items_ = std::move(loaded);
    return true;
}

bool SavedSearchStore::SaveTo(const std::wstring& path) const {
    std::wstring document = L"{\n  \"schema\":1,\n  \"searches\":[";
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& search = items_[i];
        std::wstring name, root, query;
        pulse::json::Escape(search.name, name);
        pulse::json::Escape(search.root, root);
        pulse::json::Escape(search.query, query);
        document += i == 0 ? L"\n" : L",\n";
        document += L"    {\"name\":\"" + name + L"\",\"mode\":\"" +
            ModeName(search.mode) + L"\",\"root\":\"" + root +
            L"\",\"query\":\"" + query + L"\",\"recursive\":" +
            (search.recursive ? L"true" : L"false") + L"}";
    }
    document += items_.empty() ? L"]\n}\n" : L"\n  ]\n}\n";
    return WriteUtf8FileAtomic(path, document);
}

bool SavedSearchStore::Add(SavedSearch search) {
    if (!Normalize(search) || items_.size() >= kMaximumSavedSearches) return false;
    const auto duplicate = std::find_if(items_.begin(), items_.end(), [&](const SavedSearch& item) {
        return _wcsicmp(item.name.c_str(), search.name.c_str()) == 0;
    });
    if (duplicate != items_.end()) return false;
    items_.push_back(std::move(search));
    return true;
}

bool SavedSearchStore::Remove(size_t index) {
    if (index >= items_.size()) return false;
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

} // namespace pulse::app
