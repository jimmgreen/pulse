#include "search_history.h"
#include "session.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <cwctype>
#include <string_view>

namespace pulse::app {
namespace {
constexpr size_t kCapacity = 50;
constexpr size_t kMaxQuery = 4096;
constexpr size_t kMaxPath = 32768;
constexpr size_t kMaxFileBytes = 8 * 1024 * 1024;
constexpr std::wstring_view kSearchPrefix = L"pulse:search:";

std::wstring Trim(const std::wstring& text) {
    const auto first = std::find_if_not(text.begin(), text.end(), iswspace);
    const auto last = std::find_if_not(text.rbegin(), text.rend(), iswspace).base();
    return first < last ? std::wstring(first, last) : std::wstring{};
}

bool Valid(const std::wstring& query, const std::wstring& path) {
    if (query.empty() || query.size() > kMaxQuery || path.size() > kMaxPath) return false;
    if (path.compare(0, kSearchPrefix.size(), kSearchPrefix) != 0 ||
        Trim(path.substr(kSearchPrefix.size())).empty())
        return false;
    const auto control = [](wchar_t c) { return c < L' ' || c == 0x7f; };
    return std::none_of(query.begin(), query.end(), control) &&
           std::none_of(path.begin(), path.end(), control);
}

bool Same(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// Parse only this file's bounded schema, rejecting truncated strings and invalid escapes.
struct Reader {
    const std::wstring& text;
    size_t pos = 0;
    bool Token(const wchar_t* value) {
        json::SkipWhitespace(text, pos);
        const size_t count = wcslen(value);
        if (text.compare(pos, count, value) != 0) return false;
        pos += count;
        return true;
    }
    bool String(std::wstring& out) {
        if (!Token(L"\"")) return false;
        while (pos < text.size()) {
            wchar_t c = text[pos++];
            if (c == L'"') return true;
            if (c < L' ') return false;
            if (c == L'\\') {
                if (pos == text.size()) return false;
                c = text[pos++];
                if (c == L'n') c = L'\n';
                else if (c == L'r') c = L'\r';
                else if (c == L't') c = L'\t';
                else if (c != L'"' && c != L'\\' && c != L'/') return false;
            }
            out += c;
            if (out.size() > kMaxPath) return false;
        }
        return false;
    }
};
} // namespace

bool SearchHistory::Record(const std::wstring& query, const std::wstring& path) {
    const auto trimmed = Trim(query);
    if (!Valid(trimmed, path)) return false;
    if (!entries.empty() && entries.front().query == trimmed && entries.front().path == path)
        return false;
    Remove(path);
    entries.insert(entries.begin(), {trimmed, path});
    if (entries.size() > kCapacity) entries.resize(kCapacity);
    return true;
}

bool SearchHistory::Remove(const std::wstring& path) {
    const auto count = entries.size();
    std::erase_if(entries, [&](const auto& entry) { return Same(entry.path, path); });
    return count != entries.size();
}

bool SearchHistory::Clear() {
    const bool changed = !entries.empty();
    entries.clear();
    return changed;
}

std::wstring SearchHistory::ToJson() const {
    std::wstring out = L"{\"version\":1,\"entries\":[";
    size_t count = 0;
    for (const auto& entry : entries) {
        if (!Valid(entry.query, entry.path)) continue;
        if (count++) out += L",";
        out += L"{\"query\":\"";
        json::Escape(entry.query, out);
        out += L"\",\"path\":\"";
        json::Escape(entry.path, out);
        out += L"\"}";
        if (count == kCapacity) break;
    }
    return out + L"]}\n";
}

bool SearchHistory::FromJson(const std::wstring& text) {
    if (text.size() > kMaxFileBytes) return false;
    Reader reader{text};
    if (!reader.Token(L"{") || !reader.Token(L"\"version\"") || !reader.Token(L":") ||
        !reader.Token(L"1") || !reader.Token(L",") || !reader.Token(L"\"entries\"") ||
        !reader.Token(L":") || !reader.Token(L"[")) return false;
    SearchHistory parsed;
    if (!reader.Token(L"]")) {
        do {
            SearchHistoryEntry entry;
            if (!reader.Token(L"{") || !reader.Token(L"\"query\"") || !reader.Token(L":") ||
                !reader.String(entry.query) || !reader.Token(L",") ||
                !reader.Token(L"\"path\"") || !reader.Token(L":") ||
                !reader.String(entry.path) || !reader.Token(L"}")) return false;
            entry.query = Trim(entry.query);
            if (!Valid(entry.query, entry.path)) return false;
            const bool duplicate = std::any_of(parsed.entries.begin(), parsed.entries.end(),
                [&](const auto& old) { return Same(old.path, entry.path); });
            if (!duplicate && parsed.entries.size() < kCapacity)
                parsed.entries.push_back(std::move(entry));
        } while (reader.Token(L","));
        if (!reader.Token(L"]")) return false;
    }
    if (!reader.Token(L"}")) return false;
    json::SkipWhitespace(text, reader.pos);
    if (reader.pos != text.size()) return false;
    entries = std::move(parsed.entries);
    return true;
}

bool SearchHistory::LoadFromFile(const std::wstring& file) {
    if (!persist) return false;
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(handle, &size) && size.QuadPart >= 0 && size.QuadPart <= kMaxFileBytes;
    std::vector<uint8_t> bytes(ok ? static_cast<size_t>(size.QuadPart) : 0);
    DWORD count = 0;
    if (ok) ok = ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)
        && count == bytes.size();
    CloseHandle(handle);
    std::wstring text;
    return ok && DecodeUtf8Bytes(bytes, text) && FromJson(text);
}

bool SearchHistory::SaveToFile(const std::wstring& file) const {
    return persist && WriteUtf8FileAtomic(file, ToJson());
}

bool SearchHistory::Load() {
    if (!persist) return false;
    const auto dir = GetPulseDataDir();
    return !dir.empty() && LoadFromFile(dir + L"\\search_history.json");
}

bool SearchHistory::Save() const {
    if (!persist) return false;
    const auto dir = GetPulseDataDir();
    return !dir.empty() && SaveToFile(dir + L"\\search_history.json");
}

SearchHistoryWriter::SearchHistoryWriter(std::wstring file) : file_(std::move(file)),
    thread_([this] { Run(); }) {}

SearchHistoryWriter::~SearchHistoryWriter() { Stop(); }

void SearchHistoryWriter::Submit(SearchHistory snapshot) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        pending_ = std::move(snapshot);
        ++submitted_;
    }
    wake_.notify_all();
}

void SearchHistoryWriter::Flush() {
    std::unique_lock lock(mutex_);
    const auto target = submitted_;
    wake_.wait(lock, [&] { return completed_ >= target; });
}

void SearchHistoryWriter::Stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void SearchHistoryWriter::Run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
        if (!pending_) return;
        auto snapshot = std::move(*pending_);
        pending_.reset();
        const auto revision = submitted_;
        lock.unlock();
        // Disk failures leave the previous atomic file intact. Never let allocation or
        // filesystem failures terminate the process from the background thread.
        try {
            if (file_.empty()) snapshot.Save();
            else snapshot.SaveToFile(file_);
        } catch (...) {}
        lock.lock();
        completed_ = revision;
        wake_.notify_all();
    }
}
} // namespace pulse::app
