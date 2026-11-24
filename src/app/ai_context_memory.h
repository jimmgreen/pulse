// ai_context_memory.h - Long-conversation context compaction and local habit
// memory (%LOCALAPPDATA%\Pulse\ai\memory.json) for Pulse 助手.
#pragma once

#include "../common/json_value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app::ai {

// Conservative token estimator across ASCII, CJK UTF-8, tool calls, reasoning,
// and multimodal image_url parts (counted as ~765 tokens, not base64 byte count).
size_t EstimateMessageTokens(const json::Value& msg);
size_t EstimateHistoryTokens(const json::Value& history);

// Returns a request-ready copy of `history` compacted to fit within `max_tokens`
// without mutating the caller's full history:
// - Stage 1 (completed turns before the last user message): strips old
//   `reasoning_content` (preserving active-turn reasoning for tool loops),
//   folds large old `role=="tool"` payloads (> 1500 bytes), and replaces old
//   `image_url` data URLs with concise text placeholders.
// - Stage 2 (if still > `max_tokens`): folds oldest completed turns into a
//   single structured summary message while preserving recent turns and the
//   entire active turn tail.
// Sets `compacted = true` whenever any payload was folded or summarized.
json::Value CompactHistoryForRequest(const json::Value& history, size_t max_tokens, bool& compacted);

// Local habit memory stored solely on this PC (%LOCALAPPDATA%\Pulse\ai\memory.json).
struct MemoryItem {
    std::string id;
    std::string text;
    int64_t created = 0;   // Unix epoch seconds
};

std::wstring MemoryFilePath(const std::wstring& dir_override = {});
std::vector<MemoryItem> LoadMemories(const std::wstring& dir_override = {});
bool SaveMemories(const std::vector<MemoryItem>& items, const std::wstring& dir_override = {});
bool AddMemory(const std::string& text, MemoryItem& out_item, const std::wstring& dir_override = {});
size_t RemoveMemory(const std::string& id_or_query, const std::wstring& dir_override = {});
bool ClearMemories(const std::wstring& dir_override = {});
std::string FormatMemoriesForPrompt(const std::vector<MemoryItem>& items);

} // namespace pulse::app::ai
