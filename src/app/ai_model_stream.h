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
};

struct ModelToolCall {
    std::string id;
    std::string name;
    std::string arguments;   // JSON text as the model wrote it
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

} // namespace pulse::app::ai
