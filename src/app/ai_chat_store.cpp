#include "ai_chat_store.h"

#include "ai_settings.h"
#include "../common/utf8_file.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace pulse::app::ai {
namespace {

constexpr size_t kMaxChatBytes = 32u << 20;

std::wstring PathOf(const std::wstring& dir, std::wstring_view id) {
    std::wstring path = dir;
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    path.append(id);
    path += L".json";
    return path;
}

std::wstring Lower(std::wstring text) {
    if (!text.empty() && text.size() < static_cast<size_t>(INT_MAX))
        CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

int64_t GetInt(const json::Value& doc, std::string_view key) {
    int64_t v = 0;
    if (const json::Value* p = doc.find(key); p && p->get_int(v)) return v;
    return 0;
}

bool ReadDoc(const std::wstring& path, json::Value& doc) {
    std::wstring text;
    if (!ReadUtf8File(path, text)) return false;
    json::ParseLimits limits;
    limits.max_bytes = kMaxChatBytes;
    json::Value parsed;
    if (!json::Parse(json::Utf8(text), parsed, nullptr, limits) || !parsed.is_object()) return false;
    doc = std::move(parsed);
    return true;
}

}  // namespace

std::wstring SearchKey(std::wstring text) { return Lower(std::move(text)); }

std::wstring ChatsDirectory() {
    const std::wstring dir = AiDirectory();
    return dir.empty() ? std::wstring{} : dir + L"\\chats";
}

int64_t NowUnixMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<int64_t>(t / 10000ull) - 11644473600000ll;
}

std::wstring NewChatId(int64_t now_ms, uint32_t salt) {
    const uint64_t t = static_cast<uint64_t>(now_ms + 11644473600000ll) * 10000ull;
    FILETIME utc{static_cast<DWORD>(t & 0xffffffffu), static_cast<DWORD>(t >> 32)}, local{};
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&utc, &local);
    FileTimeToSystemTime(&local, &st);
    wchar_t buf[40] = {};
    swprintf_s(buf, L"%04u%02u%02u-%02u%02u%02u-%04x", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond, salt & 0xffffu);
    return buf;
}

bool ValidChatId(std::wstring_view id) {
    if (id.empty() || id.size() > 40) return false;
    for (wchar_t c : id)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'-'))
            return false;
    return true;
}

std::wstring PlainLine(std::wstring_view md, size_t max_chars) {
    std::wstring out;
    size_t i = 0;
    while (i < md.size() && out.size() < max_chars) {
        // A line start: skip heading, quote, list and task markers.
        const bool line_start = i == 0 || md[i - 1] == L'\n';
        if (line_start) {
            while (i < md.size() && (md[i] == L' ' || md[i] == L'\t')) ++i;
            while (i < md.size() && (md[i] == L'#' || md[i] == L'>')) ++i;
            while (i < md.size() && md[i] == L' ') ++i;
            if (i + 1 < md.size() && (md[i] == L'-' || md[i] == L'*' || md[i] == L'+') && md[i + 1] == L' ') i += 2;
            size_t d = i;
            while (d < md.size() && md[d] >= L'0' && md[d] <= L'9') ++d;
            if (d > i && d + 1 < md.size() && (md[d] == L'.' || md[d] == L')') && md[d + 1] == L' ') i = d + 2;
            if (i + 3 < md.size() && md[i] == L'[' && (md[i + 1] == L' ' || md[i + 1] == L'x' || md[i + 1] == L'X') &&
                md[i + 2] == L']' && md[i + 3] == L' ')
                i += 4;
            if (i + 2 < md.size() && md.substr(i, 3) == L"```") {   // a fence line goes
                while (i < md.size() && md[i] != L'\n') ++i;
                continue;
            }
        }
        if (i >= md.size()) break;
        const wchar_t c = md[i++];
        if (c == L'*' || c == L'`' || (c == L'_' && (out.empty() || out.back() == L' '))) continue;
        if (c == L'\r') continue;
        if (c == L'\n' || c == L'\t' || c == L' ') {
            if (!out.empty() && out.back() != L' ') out += L' ';
            continue;
        }
        out += c;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    if (out.size() >= max_chars && max_chars > 1) {
        out.resize(max_chars - 1);
        out += L'\x2026';
    }
    return out;
}

ChatEntry EntryOf(const std::wstring& id, const json::Value& doc) {
    ChatEntry e;
    e.id = id;
    e.title = json::Wide(doc.get_string("title"));
    e.pinned = doc.get_bool("pinned");
    e.created = GetInt(doc, "created");
    e.updated = GetInt(doc, "updated");
    if (e.updated == 0) e.updated = e.created;
    std::wstring hay = e.title;
    std::wstring last_answer, last_note;
    if (const json::Value* blocks = doc.find("blocks"); blocks && blocks->is_array()) {
        for (const json::Value& b : blocks->items()) {
            if (!b.is_object()) continue;
            const std::string k = b.get_string("k");
            if (k != "user" && k != "text" && k != "note") continue;
            const std::wstring text = json::Wide(b.get_string("text"));
            hay += L'\n';
            hay += text;
            if (k == "text" && !text.empty()) last_answer = text;
            if (k == "note" && !text.empty()) last_note = text;
        }
    }
    e.snippet = PlainLine(last_answer.empty() ? last_note : last_answer, 80);
    e.haystack = Lower(std::move(hay));
    return e;
}

std::vector<ChatEntry> ListChats(const std::wstring& dir) {
    std::vector<ChatEntry> out;
    if (dir.empty()) return out;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW((dir + L"\\*.json").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (name.size() <= 5 || _wcsicmp(name.c_str() + name.size() - 5, L".json") != 0) continue;
        name.resize(name.size() - 5);
        if (!ValidChatId(name)) continue;
        json::Value doc;
        if (!ReadDoc(PathOf(dir, name), doc)) continue;
        out.push_back(EntryOf(name, doc));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::stable_sort(out.begin(), out.end(), [](const ChatEntry& a, const ChatEntry& b) {
        if (a.updated != b.updated) return a.updated > b.updated;
        return a.id > b.id;
    });
    return out;
}

bool LoadChat(const std::wstring& dir, std::wstring_view id, json::Value& doc) {
    if (dir.empty() || !ValidChatId(id)) return false;
    return ReadDoc(PathOf(dir, id), doc);
}

bool SaveChat(const std::wstring& dir, std::wstring_view id, const json::Value& doc) {
    if (dir.empty() || !ValidChatId(id)) return false;
    const size_t cut = dir.find_last_of(L'\\');
    if (cut != std::wstring::npos) CreateDirectoryW(dir.substr(0, cut).c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::string utf8 = json::Serialize(doc);
    if (utf8.size() > kMaxChatBytes) return false;
    return WriteUtf8FileAtomic(PathOf(dir, id), json::Wide(utf8));
}

bool DeleteChat(const std::wstring& dir, std::wstring_view id) {
    if (dir.empty() || !ValidChatId(id)) return false;
    return DeleteFileW(PathOf(dir, id).c_str()) != 0;
}

bool UpdateChatMeta(const std::wstring& dir, std::wstring_view id, const std::wstring* title, const bool* pinned) {
    json::Value doc;
    if (!LoadChat(dir, id, doc)) return false;
    if (title) {
        doc.set("title", json::Utf8(*title));
        doc.set("renamed", true);
    }
    if (pinned) doc.set("pinned", *pinned);
    return SaveChat(dir, id, doc);
}

int PurgeChats(const std::wstring& dir, int days, int64_t now_ms) {
    if (days <= 0 || dir.empty()) return 0;
    const int64_t cutoff = now_ms - static_cast<int64_t>(days) * 86400000ll;
    int removed = 0;
    for (const ChatEntry& e : ListChats(dir)) {
        if (e.pinned || e.updated >= cutoff) continue;
        if (DeleteChat(dir, e.id)) ++removed;
    }
    return removed;
}

}  // namespace pulse::app::ai
