// batch_rename_content.cpp - see batch_rename_content.h.
#include "batch_rename_content.h"

#include "../common/json_value.h"

#include <algorithm>
#include <string_view>

namespace pulse::app::rename {
namespace {

using json::Value;

constexpr const char* kPrompt =
    "You name files from their contents. For each file you get its current name and text read from it. "
    "Reply with exactly one JSON object and nothing else:\n"
    "{\"files\":[{\"id\":1,\"name\":\"...\",\"keep\":false,\"basis\":\"...\"}]}\n"
    "\n"
    "- name: the new name WITHOUT extension, in the format below. Short (under 60 characters) and specific: "
    "what the document is and whom or what it concerns, with a date when the text gives one. None of "
    "\\ / : * ? \" < > |.\n"
    "- keep: true when the current name already says clearly what the file is, and when the text is empty, "
    "unreadable or says nothing about what the file is. Then leave name empty.\n"
    "- basis: the words in the text the name rests on, quoted briefly (under 60 characters, parts joined "
    "with \" \xC2\xB7 \"), in the text's own language. With keep, a short reason in the reason language "
    "given below.\n"
    "- Never invent facts that are not in the text. One entry per file, same ids.\n";

const char* FormatRule(NameFormat f) {
    switch (f) {
    case NameFormat::DateTypeSubject:
        return "Format: \"<date> <type> \xC2\xB7 <subject>\" with the date as yyyy-MM, e.g. "
               "\"2026-03 \xE5\x8F\x91\xE7\xA5\xA8 \xC2\xB7 \xE6\x98\x9F\xE6\xB2\xB3\xE7\xA7\x91\xE6\x8A\x80\". "
               "Leave the date out when the text has none.";
    case NameFormat::TypeSubjectDate:
        return "Format: \"<type> - <subject> - <date>\" with the date as yyyyMM, e.g. "
               "\"\xE5\x8F\x91\xE7\xA5\xA8 - \xE6\x98\x9F\xE6\xB2\xB3\xE7\xA7\x91\xE6\x8A\x80 - 202603\". "
               "Leave the date out when the text has none.";
    case NameFormat::LikeFolder:
        return "Format: follow the style of the existing names in the folder listed below (order of parts, "
               "separators, date style, language).";
    }
    return "";
}

const char* LanguageRule(NameLanguage l) {
    switch (l) {
    case NameLanguage::Chinese: return "Name language: Simplified Chinese (keep proper names as written).";
    case NameLanguage::English: return "Name language: English (keep proper names as written).";
    case NameLanguage::FollowText: return "Name language: the main language of each file's text.";
    }
    return "";
}

bool IsBad(wchar_t c) {
    return c < 32 || c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' ||
           c == L'>' || c == L'|';
}

} // namespace

std::wstring CompactText(const std::wstring& text, size_t max_chars) {
    std::wstring out;
    out.reserve((std::min)(text.size(), max_chars));
    size_t newlines = 0;
    bool space = false;
    for (wchar_t c : text) {
        if (c == L'\r') { space = true; continue; }   // \r\n is one break
        if (c == L'\n' || c == L'\f' || c == 0x2029) { ++newlines; space = true; continue; }
        if (c == L' ' || c == L'\t' || c == 0x3000 || c == 0xA0 || c < 32) { space = true; continue; }
        if (space && !out.empty()) {
            const std::wstring_view gap = newlines >= 2 ? std::wstring_view(L" / ") : std::wstring_view(L" ");
            if (out.size() + gap.size() >= max_chars) break;
            out += gap;
        }
        space = false;
        newlines = 0;
        if (out.size() + 1 > max_chars) break;
        out += c;
    }
    if (!out.empty() && out.size() >= max_chars && out.back() >= 0xD800 && out.back() <= 0xDBFF) out.pop_back();
    return out;
}

std::wstring CleanName(const std::wstring& name, size_t max_chars) {
    std::wstring out;
    bool space = false;
    for (wchar_t c : name) {
        if (IsBad(c) || c == L' ' || c == L'\t' || c == 0x3000) { space = true; continue; }
        if (space && !out.empty()) out += L' ';
        space = false;
        out += c;
    }
    if (out.size() > max_chars) {
        out.resize(max_chars);
        if (!out.empty() && out.back() >= 0xD800 && out.back() <= 0xDBFF) out.pop_back();
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    return out;
}

std::string ContentRequestBody(const ContentAsk& ask) {
    std::string system = kPrompt;
    system += "\n";
    system += FormatRule(ask.format);
    system += "\n";
    system += LanguageRule(ask.language);
    system += "\nReason language (keep): " + ask.reason_language + ".";
    std::string user;
    if (ask.format == NameFormat::LikeFolder && !ask.folder_names.empty()) {
        user += "Existing names in the folder:\n";
        size_t shown = 0;
        for (const auto& n : ask.folder_names) {
            if (shown++ == 20) break;
            user += "- " + json::Utf8(n) + "\n";
        }
        user += "\n";
    }
    for (const auto& f : ask.files) {
        user += "### id " + std::to_string(f.id) + "\n";
        user += "name: " + json::Utf8(f.name) + "\n";
        user += "text: " + json::Utf8(f.text) + "\n\n";
    }
    Value body = Value::Object();
    body.set("model", json::Utf8(ask.model));
    Value messages = Value::Array();
    Value sys = Value::Object();
    sys.set("role", "system");
    sys.set("content", system);
    messages.push(std::move(sys));
    Value usr = Value::Object();
    usr.set("role", "user");
    usr.set("content", user);
    messages.push(std::move(usr));
    body.set("messages", std::move(messages));
    body.set("stream", true);
    Value format = Value::Object();
    format.set("type", "json_object");
    body.set("response_format", std::move(format));
    // No thinking: the caller adds the off switch in the form its service takes
    // (ai_providers.h WithoutThinking).
    return json::Serialize(body);
}

bool ParseContentReply(const std::string& utf8, std::vector<ContentSuggestion>& out) {
    out.clear();
    const size_t a = utf8.find('{'), b = utf8.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a) return false;
    Value root;
    if (!json::Parse(std::string_view(utf8).substr(a, b - a + 1), root) || !root.is_object()) return false;
    const Value* files = root.find("files");
    if (!files || !files->is_array()) return false;
    for (const Value& v : files->items()) {
        if (!v.is_object()) continue;
        ContentSuggestion s;
        int64_t id = 0;
        const Value* idv = v.find("id");
        if (!idv || !idv->get_int(id, 0, 1 << 20)) continue;
        s.id = static_cast<int>(id);
        s.keep = v.get_bool("keep");
        s.name = s.keep ? std::wstring() : CleanName(json::Wide(v.get_string("name")));
        if (s.name.empty()) s.keep = true;
        s.basis = CompactText(json::Wide(v.get_string("basis")), 120);
        out.push_back(std::move(s));
    }
    return true;
}

} // namespace pulse::app::rename
