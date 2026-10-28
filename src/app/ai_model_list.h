// ai_model_list.h - The connected service's models, as the service names them.
//
// GET /models lists the ids a service accepts; Pulse shows and sends them
// unchanged (another service's dated ids stay whole). The list of the last
// endpoint is cached in %LOCALAPPDATA%\Pulse\ai\models.txt, with the models
// that cannot call tools (仅聊天). UI thread only, except StoreListedModels.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace pulse::app::ai {

// base_url's models in the service's order: the cached list, else for the
// official DeepSeek endpoint the two it serves, else none.
const std::vector<std::string>& ListedModels(const std::wstring& base_url);

// A successful GET /models of base_url (empty lists are ignored); chat_only:
// the ids the list itself marks as unable to call tools. A connect forgets the
// models learned below; a background refresh (keep_learned) keeps them. Any
// thread: a worker's list is taken in by the UI thread's next call here.
void StoreListedModels(const std::wstring& base_url, const std::vector<std::string>& ids,
                       const std::vector<std::string>& chat_only = {}, bool keep_learned = false);

// 仅聊天: the model cannot call tools, so the assistant can only talk with it.
// OpenRouter's list says so; any other service refused tools once (marked).
bool IsChatOnlyModel(const std::wstring& base_url, const std::string& id);
void MarkChatOnlyModel(const std::wstring& base_url, const std::string& id);

// Fetches base_url's list in the background, with the stored key, when it is
// missing or a day old (a failed try waits 10 minutes). hwnd is repainted
// once a new list is in.
void RefreshListedModels(HWND hwnd, const std::wstring& base_url);

} // namespace pulse::app::ai
