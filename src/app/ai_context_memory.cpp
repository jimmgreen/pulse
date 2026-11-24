// ai_context_memory.cpp - Context compaction and local habit memory store.
#include "ai_context_memory.h"

#include "../common/utf8_file.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace pulse::app::ai {
namespace {

constexpr size_t kMaxMemories = 32;
constexpr size_t kMaxMemoryChars = 240;
constexpr size_t kOldToolCompactThreshold = 1500;
constexpr size_t kOldToolKeepBytes = 900;
constexpr size_t kImageTokenEstimate = 765;

size_t EstimateStringTokens(const std::string& s) {
    size_t ascii = 0, non_ascii = 0;
    for (unsigned char c : s) {
        if (c < 0x80u) ++ascii;
        else ++non_ascii;
    }
    return (ascii + 3) / 4 + (non_ascii * 2 + 2) / 3;
}

std::string TruncateUtf8Bytes(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0u) == 0x80u) --cut;
    return s.substr(0, cut);
}

std::string TruncateUtf8Chars(const std::string& s, size_t max_chars) {
    size_t chars = 0, i = 0;
    while (i < s.size()) {
        if (chars >= max_chars) return s.substr(0, i);
        const unsigned char b = static_cast<unsigned char>(s[i]);
        size_t step = 1;
        if ((b & 0xE0u) == 0xC0u) step = 2;
        else if ((b & 0xF0u) == 0xE0u) step = 3;
        else if ((b & 0xF8u) == 0xF0u) step = 4;
        if (i + step > s.size()) break;
        i += step;
        ++chars;
    }
    return s;
}

std::string TrimUtf8(std::string s) {
    auto ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t start = 0;
    while (start < s.size() && ws(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && ws(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string LowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string MessagePlainExcerpt(const json::Value& msg, size_t max_chars) {
    std::string text;
    if (const json::Value* cnt = msg.find("content")) {
        if (cnt->is_string()) {
            text = cnt->as_string();
        } else if (cnt->is_array()) {
            for (const json::Value& part : cnt->items()) {
                if (part.get_string("type") == "text") {
                    if (!text.empty()) text += " ";
                    text += part.get_string("text");
                } else if (part.get_string("type") == "image_url") {
                    if (!text.empty()) text += " ";
                    text += "[image]";
                }
            }
        }
    }
    // Collapse newlines into spaces for one-line digest bullets.
    for (char& c : text)
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    text = TrimUtf8(std::move(text));
    if (text.size() > max_chars) {
        text = TruncateUtf8Chars(text, max_chars) + "...";
    }
    return text;
}

std::wstring DefaultAiDir() {
    wchar_t buf[32768]{};
    DWORD n = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        std::wstring d(buf, n);
        while (!d.empty() && d.back() == L'\\') d.pop_back();
        return d + L"\\ai";
    }
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    std::wstring d(buf, n);
    while (!d.empty() && d.back() == L'\\') d.pop_back();
    return d + L"\\Pulse\\ai";
}

void EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return;
    size_t pos = 0;
    if (dir.size() >= 2 && dir[1] == L':') pos = 3;
    while (pos <= dir.size()) {
        const size_t next = dir.find(L'\\', pos);
        const std::wstring sub = next == std::wstring::npos ? dir : dir.substr(0, next);
        if (!sub.empty()) CreateDirectoryW(sub.c_str(), nullptr);
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
}

std::string MakeMemoryId(const std::vector<MemoryItem>& existing, int64_t now) {
    char base[32]{};
    sprintf_s(base, "m%lld", static_cast<long long>(now));
    std::string id = base;
    for (int seq = 1; seq < 1000; ++seq) {
        bool clash = false;
        for (const MemoryItem& it : existing)
            if (it.id == id) { clash = true; break; }
        if (!clash) return id;
        id = std::string(base) + "_" + std::to_string(seq);
    }
    return id;
}

} // namespace

size_t EstimateMessageTokens(const json::Value& msg) {
    if (!msg.is_object()) return 0;
    size_t tokens = 8;
    if (const json::Value* cnt = msg.find("content")) {
        if (cnt->is_string()) {
            tokens += EstimateStringTokens(cnt->as_string());
        } else if (cnt->is_array()) {
            for (const json::Value& part : cnt->items()) {
                const std::string t = part.get_string("type");
                if (t == "text") tokens += EstimateStringTokens(part.get_string("text"));
                else if (t == "image_url") tokens += kImageTokenEstimate;
            }
        }
    }
    if (const json::Value* rc = msg.find("reasoning_content"); rc && rc->is_string()) {
        tokens += EstimateStringTokens(rc->as_string());
    }
    if (const json::Value* tcs = msg.find("tool_calls"); tcs && tcs->is_array()) {
        for (const json::Value& tc : tcs->items()) {
            tokens += 12;
            if (const json::Value* fn = tc.find("function"); fn && fn->is_object()) {
                tokens += EstimateStringTokens(fn->get_string("name"));
                tokens += EstimateStringTokens(fn->get_string("arguments"));
            }
        }
    }
    return tokens;
}

size_t EstimateHistoryTokens(const json::Value& history) {
    if (!history.is_array()) return 0;
    size_t total = 0;
    for (const json::Value& m : history.items()) total += EstimateMessageTokens(m);
    return total;
}

json::Value CompactHistoryForRequest(const json::Value& history, size_t max_tokens, bool& compacted) {
    compacted = false;
    if (!history.is_array()) return json::Value::Array();
    const size_t n = history.size();
    if (n == 0) return json::Value::Array();

    // Locate the start of the active turn (last user message). Anything at or
    // after `last_user` belongs to the live tool-call loop and keeps its full
    // reasoning_content and images untouched.
    size_t last_user = 0;
    for (size_t i = n; i > 0; --i) {
        if (history.at(i - 1).get_string("role") == "user") {
            last_user = i - 1;
            break;
        }
    }

    // Stage 1: Clean completed turns strictly before `last_user`.
    json::Value stage1 = json::Value::Array();
    for (size_t i = 0; i < n; ++i) {
        const json::Value& orig = history.at(i);
        if (i >= last_user) {
            stage1.push(orig);
            continue;
        }
        const std::string role = orig.get_string("role");
        json::Value m = json::Value::Object();
        m.set("role", role);
        if (role == "assistant") {
            if (const json::Value* c = orig.find("content")) m.set("content", *c);
            if (const json::Value* tc = orig.find("tool_calls")) m.set("tool_calls", *tc);
            // Strip reasoning_content on already-completed turns.
            if (const json::Value* rc = orig.find("reasoning_content");
                rc && rc->is_string() && !rc->as_string().empty()) {
                compacted = true;
            }
        } else if (role == "tool") {
            if (const json::Value* id = orig.find("tool_call_id")) m.set("tool_call_id", *id);
            const std::string txt = orig.get_string("content");
            if (txt.size() > kOldToolCompactThreshold) {
                std::string shrunk = TruncateUtf8Bytes(txt, kOldToolKeepBytes);
                shrunk += "\n... [earlier tool output compacted, original " + std::to_string(txt.size()) + " bytes]";
                m.set("content", std::move(shrunk));
                compacted = true;
            } else {
                m.set("content", txt);
            }
        } else if (role == "user") {
            if (const json::Value* cnt = orig.find("content"); cnt && cnt->is_array()) {
                std::string flattened;
                bool had_image = false;
                for (const json::Value& part : cnt->items()) {
                    const std::string t = part.get_string("type");
                    if (t == "text") {
                        if (!flattened.empty()) flattened += "\n";
                        flattened += part.get_string("text");
                    } else if (t == "image_url") {
                        had_image = true;
                        if (!flattened.empty()) flattened += "\n";
                        flattened += "[Earlier image attachment omitted from context]";
                    }
                }
                if (had_image) {
                    m.set("content", std::move(flattened));
                    compacted = true;
                } else {
                    m.set("content", *cnt);
                }
            } else if (const json::Value* c = orig.find("content")) {
                m.set("content", *c);
            }
        } else {
            m = orig;
        }
        stage1.push(std::move(m));
    }

    if (max_tokens == 0 || EstimateHistoryTokens(stage1) <= max_tokens) {
        return stage1;
    }

    // Stage 2: Fold oldest completed user/assistant turns into one structured
    // summary message while keeping the active turn and recent turns intact.
    // Find valid user-message boundaries strictly before `last_user`.
    std::vector<size_t> user_starts;
    for (size_t i = 0; i <= last_user && i < stage1.size(); ++i) {
        if (stage1.at(i).get_string("role") == "user") user_starts.push_back(i);
    }
    if (user_starts.size() <= 1) {
        // Only a single turn exists — cannot drop the active turn.
        return stage1;
    }

    // Walk forward through completed turn boundaries until remaining tail fits
    // (always keeping at least the active turn starting at `last_user`).
    size_t cut = 0;
    for (size_t b = 1; b < user_starts.size(); ++b) {
        const size_t candidate = user_starts[b];
        size_t tail_tokens = 256; // reserve for summary block
        for (size_t i = candidate; i < stage1.size(); ++i) tail_tokens += EstimateMessageTokens(stage1.at(i));
        cut = candidate;
        if (tail_tokens <= max_tokens) break;
    }
    if (cut == 0) return stage1;

    std::string digest = "[Pulse earlier conversation summary (" + std::to_string(cut) + " messages compacted):]\n";
    for (size_t i = 0; i < cut; ++i) {
        const json::Value& m = stage1.at(i);
        const std::string role = m.get_string("role");
        if (role == "user") {
            const std::string ex = MessagePlainExcerpt(m, 160);
            if (!ex.empty()) digest += "- User: " + ex + "\n";
        } else if (role == "assistant") {
            const std::string ex = MessagePlainExcerpt(m, 180);
            if (!ex.empty()) digest += "- Assistant: " + ex + "\n";
            else if (const json::Value* tc = m.find("tool_calls"); tc && tc->is_array() && tc->size() > 0) {
                std::string names;
                for (const json::Value& c : tc->items()) {
                    if (const json::Value* fn = c.find("function")) {
                        if (!names.empty()) names += ", ";
                        names += fn->get_string("name");
                    }
                }
                if (!names.empty()) digest += "- Assistant called tools: " + names + "\n";
            }
        }
    }
    if (digest.size() > 6000) {
        digest = TruncateUtf8Bytes(digest, 5900) + "\n... [summary truncated]";
    }

    json::Value out = json::Value::Array();
    json::Value sum_msg = json::Value::Object();
    sum_msg.set("role", "user");
    sum_msg.set("content", std::move(digest));
    out.push(std::move(sum_msg));
    for (size_t i = cut; i < stage1.size(); ++i) out.push(stage1.at(i));
    compacted = true;
    return out;
}

std::wstring MemoryFilePath(const std::wstring& dir_override) {
    std::wstring dir = dir_override.empty() ? DefaultAiDir() : dir_override;
    if (dir.empty()) return {};
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    return dir + L"\\memory.json";
}

std::vector<MemoryItem> LoadMemories(const std::wstring& dir_override) {
    std::vector<MemoryItem> out;
    const std::wstring path = MemoryFilePath(dir_override);
    if (path.empty()) return out;
    std::wstring wide;
    if (!ReadUtf8File(path, wide)) return out;
    json::Value root;
    if (!json::Parse(json::Utf8(wide), root) || !root.is_object()) return out;
    const json::Value* items = root.find("memories");
    if (!items || !items->is_array()) return out;
    for (const json::Value& it : items->items()) {
        if (!it.is_object()) continue;
        MemoryItem m;
        m.id = it.get_string("id");
        m.text = TrimUtf8(it.get_string("text"));
        if (const json::Value* c = it.find("created")) c->get_int(m.created, 0, INT64_MAX);
        if (!m.id.empty() && !m.text.empty()) {
            out.push_back(std::move(m));
            if (out.size() >= kMaxMemories) break;
        }
    }
    return out;
}

bool SaveMemories(const std::vector<MemoryItem>& items, const std::wstring& dir_override) {
    const std::wstring path = MemoryFilePath(dir_override);
    if (path.empty()) return false;
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) EnsureDir(path.substr(0, slash));
    json::Value root = json::Value::Object();
    root.set("version", 1);
    json::Value arr = json::Value::Array();
    for (size_t i = 0; i < items.size() && i < kMaxMemories; ++i) {
        json::Value o = json::Value::Object();
        o.set("id", items[i].id);
        o.set("text", items[i].text);
        o.set("created", items[i].created);
        arr.push(std::move(o));
    }
    root.set("memories", std::move(arr));
    return WriteUtf8FileAtomic(path, json::Wide(json::Serialize(root)));
}

bool AddMemory(const std::string& raw_text, MemoryItem& out_item, const std::wstring& dir_override) {
    std::string clean = TrimUtf8(raw_text);
    for (char& c : clean)
        if (c == '\r' || c == '\n') c = ' ';
    clean = TruncateUtf8Chars(clean, kMaxMemoryChars);
    if (clean.empty()) return false;

    std::vector<MemoryItem> items = LoadMemories(dir_override);
    const std::string key = LowerAscii(clean);
    for (const MemoryItem& existing : items) {
        if (LowerAscii(existing.text) == key) {
            out_item = existing;
            return true;
        }
    }
    if (items.size() >= kMaxMemories) {
        items.erase(items.begin());
    }
    MemoryItem added;
    added.created = static_cast<int64_t>(std::time(nullptr));
    added.id = MakeMemoryId(items, added.created);
    added.text = std::move(clean);
    items.push_back(added);
    if (!SaveMemories(items, dir_override)) return false;
    out_item = std::move(added);
    return true;
}

size_t RemoveMemory(const std::string& id_or_query, const std::wstring& dir_override) {
    const std::string q = LowerAscii(TrimUtf8(id_or_query));
    if (q.empty()) return 0;
    std::vector<MemoryItem> items = LoadMemories(dir_override);
    const size_t before = items.size();
    items.erase(std::remove_if(items.begin(), items.end(), [&](const MemoryItem& m) {
                    return LowerAscii(m.id) == q || LowerAscii(m.text).find(q) != std::string::npos;
                }),
                items.end());
    const size_t removed = before - items.size();
    if (removed > 0) SaveMemories(items, dir_override);
    return removed;
}

bool ClearMemories(const std::wstring& dir_override) {
    return SaveMemories({}, dir_override);
}

std::string FormatMemoriesForPrompt(const std::vector<MemoryItem>& items) {
    if (items.empty()) return {};
    std::string out = "\nUser preferences remembered locally on this PC (apply when relevant; user can ask to change or forget any):\n";
    for (const MemoryItem& m : items) {
        out += "- [" + m.id + "] " + m.text + "\n";
    }
    return out;
}

} // namespace pulse::app::ai
