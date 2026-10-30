// ai_chat_store.h - Pulse 助手's saved conversations (55 P3 历史对话).
//
// One file per conversation under %LOCALAPPDATA%\Pulse\ai\chats\<id>.json:
//   {"v":1,"id","title","renamed","pinned","created","updated","model",
//    "blocks":[...], "history":[model messages]}
// The blocks are what the panel shows (ai_chat_history.cpp builds and reads
// them); this file only stores, lists and removes. Nothing here holds an API
// key: the model messages never carry one.
#pragma once

#include "../common/json_value.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app::ai {

// One saved conversation as the history list shows it.
struct ChatEntry {
    std::wstring id;          // the file name without .json
    std::wstring title;
    std::wstring snippet;     // the last answer's first line (plain)
    std::wstring haystack;    // the title and every message, lower case (search)
    bool pinned = false;
    int64_t created = 0;      // unix ms
    int64_t updated = 0;
};

std::wstring ChatsDirectory();   // %LOCALAPPDATA%\Pulse\ai\chats, empty without it
int64_t NowUnixMs();

// yyyyMMdd-HHmmss-xxxx from local time and four hex digits of `salt`.
std::wstring NewChatId(int64_t now_ms, uint32_t salt);
// Letters, digits and '-' only, 1..40 long: a file name that stays in `dir`.
bool ValidChatId(std::wstring_view id);

// Newest first by `updated`. Files that do not read or parse are skipped.
std::vector<ChatEntry> ListChats(const std::wstring& dir);
// The entry for one document (as ListChats builds it).
ChatEntry EntryOf(const std::wstring& id, const json::Value& doc);

bool LoadChat(const std::wstring& dir, std::wstring_view id, json::Value& doc);
// Writes through a temporary file; creates `dir` (and its parent) when missing.
bool SaveChat(const std::wstring& dir, std::wstring_view id, const json::Value& doc);
bool DeleteChat(const std::wstring& dir, std::wstring_view id);
// Sets the title (renamed) and/or pinned in the file; the rest stays.
bool UpdateChatMeta(const std::wstring& dir, std::wstring_view id, const std::wstring* title, const bool* pinned);
// Removes conversations not updated for `days` days (pinned ones stay);
// days <= 0 keeps everything. Returns how many went.
int PurgeChats(const std::wstring& dir, int days, int64_t now_ms);

// A one-line plain version of Markdown: no heading, list or quote markers,
// no emphasis or code ticks, whitespace collapsed; at most `max_chars`.
std::wstring PlainLine(std::wstring_view markdown, size_t max_chars);
// Lower case the way ChatEntry::haystack is (a search query goes through it).
std::wstring SearchKey(std::wstring text);

}  // namespace pulse::app::ai
