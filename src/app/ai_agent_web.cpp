// ai_agent_web.cpp - web_search and fetch_url tools for Pulse 助手.
#include "ai_agent_internal.h"
#include "ai_web_search.h"

namespace pulse::ai_agent {
namespace {

std::wstring ShortArg(std::wstring text, size_t max_chars) {
    if (text.size() <= max_chars) return text;
    return text.substr(0, max_chars) + L"\u2026";
}

size_t Utf8CharCount(std::string_view s) {
    size_t count = 0;
    for (unsigned char c : s)
        if ((c & 0xC0u) != 0x80u) ++count;
    return count;
}

} // namespace

void ToolWebSearch(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    const std::string query = args.get_string("query");
    int64_t max_results = 6;
    if (const json::Value* m = args.find("max_results")) m->get_int(max_results, 1, 10);
    if (query.empty()) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "query is required"));
        ++a.call_index;
        NextTool(s);
        return;
    }

    const std::wstring arg = ShortArg(json::Wide(query), 34);
    AddStep(s, a, ui::AiIcon::Globe, Text(I::AiAsstStepWebSearch), arg, Text(I::AiAsstStepWebSearchRun));
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiAsstChipWebSearch));

    app::ai::WebSearchConfig cfg;
    const app::ai::WebSearchPrefs prefs = app::ai::LoadSettings().assistant.web_search;
    cfg.provider = prefs.provider;
    cfg.endpoint = prefs.endpoint;
    (void)app::ai::LoadSearchKey(cfg.api_key);

    const std::string id = call.id;
    RunJob(
        s,
        [query, max_results, cfg = std::move(cfg)](const std::atomic<bool>& cancelled) mutable -> ToolOutcome {
            const app::ai::WebSearchResponse resp =
                app::ai::WebSearch(query, static_cast<size_t>(max_results), std::move(cfg), cancelled);
            if (!resp.ok) {
                return ToolOutcome::Fail(resp.error_code.empty() ? "search_failed" : resp.error_code,
                                         resp.error_message.empty() ? "Web search failed." : resp.error_message);
            }
            json::Value data = json::Value::Object();
            data.set("ok", true);
            data.set("engine", resp.engine);
            data.set("untrusted_web_content", true);
            if (!resp.answer.empty()) data.set("answer", resp.answer);
            json::Value list = json::Value::Array();
            for (const app::ai::WebSearchItem& item : resp.results) {
                json::Value row = json::Value::Object();
                row.set("title", item.title);
                row.set("url", item.url);
                if (!item.source.empty()) row.set("source", item.source);
                if (!item.snippet.empty()) row.set("snippet", item.snippet);
                list.push(std::move(row));
            }
            data.set("count", static_cast<int64_t>(resp.results.size()));
            data.set("results", std::move(list));
            ToolOutcome out;
            out.data = std::move(data);
            return out;
        },
        [id](AppState& st, const ToolOutcome& out) {
            AiAgent& ag = Agent(st);
            std::wstring sub;
            if (out.ok) {
                int64_t n = 0;
                if (const json::Value* c = out.data.find("count")) c->get_int(n, 0, 100);
                ag.web_sources += static_cast<size_t>(n);
                sub = Fill(Text(I::AiAsstStepWebSearchDone), L"{n}", Num(static_cast<size_t>(n)));
            } else {
                sub = Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message));
            }
            EndStep(st, ag, out.ok, sub);
            AddToolResult(ag, id, out);
            ++ag.call_index;
            AiChatSetChip(Chat(st), ui::StatusAiState::Busy, Text(I::AiAsstThinking));
            AiChatPush(st, 150, [](AppState& s2) {
                NextTool(s2);
                return -1;
            });
        });
}

void ToolFetchUrl(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    const std::string url = args.get_string("url");
    int64_t max_chars = 10000;
    if (const json::Value* m = args.find("max_chars")) m->get_int(max_chars, 500, 24000);
    if (url.empty()) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "url is required"));
        ++a.call_index;
        NextTool(s);
        return;
    }

    const std::string host = app::ai::ExtractUrlHost(url);
    const std::wstring arg = ShortArg(json::Wide(host.empty() ? url : host), 34);
    AddStep(s, a, ui::AiIcon::Globe, Text(I::AiAsstStepWebFetch), arg, Text(I::AiAsstStepWebFetchRun));
    AiChatSetChip(Chat(s), ui::StatusAiState::Busy, Text(I::AiAsstChipWebFetch));

    const std::string id = call.id;
    RunJob(
        s,
        [url, max_chars](const std::atomic<bool>& cancelled) -> ToolOutcome {
            const app::ai::WebFetchResponse resp =
                app::ai::FetchWebUrl(url, static_cast<size_t>(max_chars), cancelled);
            if (!resp.ok) {
                return ToolOutcome::Fail(resp.error_code.empty() ? "fetch_failed" : resp.error_code,
                                         resp.error_message.empty() ? "Failed to read webpage." : resp.error_message);
            }
            json::Value data = json::Value::Object();
            data.set("ok", true);
            data.set("untrusted_web_content", true);
            data.set("url", resp.url);
            if (!resp.title.empty()) data.set("title", resp.title);
            if (!resp.content_type.empty()) data.set("content_type", resp.content_type);
            data.set("truncated", resp.truncated);
            data.set("chars", static_cast<int64_t>(Utf8CharCount(resp.text)));
            data.set("text", resp.text);
            ToolOutcome out;
            out.data = std::move(data);
            return out;
        },
        [id](AppState& st, const ToolOutcome& out) {
            AiAgent& ag = Agent(st);
            std::wstring sub;
            if (out.ok) {
                int64_t chars = 0;
                if (const json::Value* c = out.data.find("chars")) c->get_int(chars, 0, 1000000);
                ++ag.web_sources;
                sub = Fill(Text(I::AiAsstStepWebFetchDone), L"{n}", Num(static_cast<size_t>(chars)));
            } else {
                sub = Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message));
            }
            EndStep(st, ag, out.ok, sub);
            AddToolResult(ag, id, out);
            ++ag.call_index;
            AiChatSetChip(Chat(st), ui::StatusAiState::Busy, Text(I::AiAsstThinking));
            AiChatPush(st, 150, [](AppState& s2) {
                NextTool(s2);
                return -1;
            });
        });
}

} // namespace pulse::ai_agent
