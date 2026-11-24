// ai_model_stream.h - One streamed chat completion from an OpenAI-format
// model service (DeepSeek: POST {base}/chat/completions, stream: true).
//
// The request runs on its own thread with synchronous WinHTTP; the UI thread
// drains events while it runs. Reasoning and content arrive as deltas
// (consecutive deltas of one kind are merged); tool calls are assembled and
// handed over complete with the finished message. Cancel closes the request,
// which ends a blocked read at once. The key travels only in the request's
// Authorization header and never appears in an event or a log.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulse::app::ai {

struct ModelEndpoint {
    std::wstring base_url = L"https://api.deepseek.com";
    std::wstring key;
    bool anthropic = false;   // true: native Anthropic /v1/messages + Prompt Caching
};

struct ModelToolCall {
    std::string id;
    std::string name;
    std::string arguments;   // JSON text as the model wrote it
};

struct ModelCitation {
    std::string title;
    std::string url;
};

struct ModelEvent {
    enum class Kind { Reasoning, Content, Finished, Failed };
    Kind kind = Kind::Content;
    std::string text;                    // Reasoning / Content: UTF-8 delta
    // Finished: the whole assistant message.
    std::string finish_reason;
    std::string content;
    std::string reasoning;
    std::vector<ModelToolCall> tool_calls;
    std::vector<ModelCitation> citations;   // provider-native search citations, deduplicated
    int64_t prompt_tokens = 0;
    int64_t completion_tokens = 0;
    // Failed.
    int http_status = 0;                 // 0: no HTTP answer (network, TLS, cancelled)
    DWORD win32_error = 0;
    std::string error_type;              // the service's error.type / error.code
    std::string error_message;           // the service's error.message (English)
};

class ModelStream {
public:
    static std::shared_ptr<ModelStream> Start(ModelEndpoint endpoint, std::string body);
    ~ModelStream();
    ModelStream(const ModelStream&) = delete;
    ModelStream& operator=(const ModelStream&) = delete;

    void Cancel();
    // Moves the pending events into `out` (appends).
    void Drain(std::vector<ModelEvent>& out);
    // The worker is done and every event has been drained.
    bool Ended() const;

    struct Shared;
private:
    ModelStream() = default;
    std::shared_ptr<Shared> shared_;
};

// The host part of a base URL, for "已连接 api.deepseek.com".
std::wstring ModelHost(const std::wstring& base_url);

// 测试连接: GET {base}/models with the key. Blocking (a few seconds at most);
// run it on a worker thread.
struct ModelProbe {
    int http_status = 0;                 // 200: the key works
    DWORD win32_error = 0;               // no HTTP answer
    uint32_t ms = 0;                     // round trip
    std::vector<std::string> models;     // ids the service lists
    std::vector<std::string> chat_only;  // of those, the ones the list says cannot call tools
    std::string error_type;
    std::string error_message;
};
ModelProbe ProbeModelService(ModelEndpoint endpoint);

// Parses citations/annotations from a single SSE JSON payload (OpenRouter/OpenAI
// annotations, Perplexity citations, Qwen search_info, Zhipu web_search) into
// `out`, deduplicating by URL. Exposed for unit testing as well as live SSE.
void CollectModelCitations(std::string_view sse_json, std::vector<ModelCitation>& out);

// Translates an OpenAI-format chat completion JSON body into an Anthropic
// /v1/messages JSON body with ephemeral Prompt Caching (`cache_control`) on
// system prompt and tool definitions, multimodal base64 image blocks, and
// tool_use/tool_result blocks.
std::string BuildAnthropicRequestBody(std::string_view openai_body);

// Pure incremental parser for Anthropic /v1/messages SSE `data:` payloads.
struct AnthropicSseState {
    std::string content;
    std::string reasoning;
    std::string finish_reason;
    struct BlockAcc {
        std::string kind;   // "text" | "thinking" | "tool_use"
        std::string id;
        std::string name;
        std::string json_args;
    };
    std::vector<BlockAcc> blocks;
    int64_t prompt_tokens = 0;
    int64_t completion_tokens = 0;
    bool message_stopped = false;

    // Feeds one SSE `data:` JSON payload, appending any Reasoning/Content delta
    // events into `out_deltas`. Returns true if parsed as valid JSON object.
    bool Feed(std::string_view data_json, std::vector<ModelEvent>& out_deltas);
    ModelEvent BuildFinished() const;
};

} // namespace pulse::app::ai
