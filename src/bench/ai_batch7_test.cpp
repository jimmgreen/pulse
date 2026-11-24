// ai_batch7_test.cpp - Tests for Batch 7: Anthropic native /v1/messages + Prompt Caching,
// Anthropic SSE assembler, reusable workflows store, ai.json mcp_servers, and live stdio MCP client.
#include "../app/ai_mcp_client.h"
#include "../app/ai_model_stream.h"
#include "../app/ai_settings.h"
#include "../app/ai_workflow_store.h"
#include "../common/json_value.h"
#include "../common/utf8_file.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* name) {
    if (ok) ++g_pass;
    else {
        ++g_fail;
        std::fprintf(stderr, "[FAIL] %s\n", name);
    }
}

} // namespace

int main() {
    using namespace pulse::app::ai;
    namespace json = pulse::json;
    namespace fs = std::filesystem;

    // 1. Anthropic /v1/messages translation + Prompt Caching + multimodal + tool_use/tool_result coalescing
    {
        static constexpr char kOpenAiReq[] = R"JSON({
            "model": "claude-sonnet-4-5",
            "stream": true,
            "messages": [
                {"role": "system", "content": "You are Pulse Assistant."},
                {"role": "user", "content": [
                    {"type": "text", "text": "Inspect this"},
                    {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,/9j/4AAQ"}}
                ]},
                {"role": "assistant", "content": "Checking both.", "tool_calls": [
                    {"id": "call_a", "type": "function", "function": {"name": "list_folder", "arguments": "{\"path\":\"C:\\\\Docs\"}"}},
                    {"id": "call_b", "type": "function", "function": {"name": "web_search", "arguments": "{\"query\":\"pulse\"}"}}
                ]},
                {"role": "tool", "tool_call_id": "call_a", "content": "{\"ok\":true}"},
                {"role": "tool", "tool_call_id": "call_b", "content": "{\"hits\":3}"}
            ],
            "tools": [
                {"type": "function", "function": {"name": "list_folder", "description": "Lists dir", "parameters": {"type": "object", "properties": {"path": {"type": "string"}}}}},
                {"type": "function", "function": {"name": "web_search", "description": "Search web", "parameters": {"type": "object", "properties": {"query": {"type": "string"}}}}}
            ]
        })JSON";

        const std::string out_str = BuildAnthropicRequestBody(kOpenAiReq);
        json::Value out;
        Check(json::Parse(out_str, out) && out.is_object(), "BuildAnthropicRequestBody valid JSON");
        Check(out.get_string("model") == "claude-sonnet-4-5", "Anthropic model preserved");
        const json::Value* sys = out.find("system");
        Check(sys && sys->is_array() && sys->size() == 1 &&
                  sys->at(0).get_string("text") == "You are Pulse Assistant." &&
                  sys->at(0).find("cache_control") != nullptr &&
                  sys->at(0).find("cache_control")->get_string("type") == "ephemeral",
              "Anthropic system block carries ephemeral cache_control");
        const json::Value* tools = out.find("tools");
        Check(tools && tools->is_array() && tools->size() == 2 &&
                  tools->at(0).find("input_schema") != nullptr &&
                  tools->at(1).find("cache_control") != nullptr &&
                  tools->at(1).find("cache_control")->get_string("type") == "ephemeral",
              "Anthropic tools use input_schema + cache_control on last tool");
        const json::Value* msgs = out.find("messages");
        Check(msgs && msgs->is_array() && msgs->size() == 3,
              "Anthropic messages coalesced (user+img, assistant+2 tool_use, user+2 tool_result)");
        if (msgs && msgs->size() == 3) {
            const json::Value* u0 = msgs->at(0).find("content");
            Check(u0 && u0->is_array() && u0->size() == 2 && u0->at(1).get_string("type") == "image" &&
                      u0->at(1).find("source")->get_string("media_type") == "image/jpeg" &&
                      u0->at(1).find("source")->get_string("data") == "/9j/4AAQ",
                  "Anthropic translates data URL image_url to base64 image block");
            const json::Value* a1 = msgs->at(1).find("content");
            Check(a1 && a1->is_array() && a1->size() == 3 && a1->at(1).get_string("type") == "tool_use" &&
                      a1->at(2).get_string("name") == "web_search",
                  "Anthropic translates assistant tool_calls to tool_use");
            const json::Value* u2 = msgs->at(2).find("content");
            Check(u2 && u2->is_array() && u2->size() == 2 &&
                      u2->at(0).get_string("type") == "tool_result" &&
                      u2->at(0).get_string("tool_use_id") == "call_a" &&
                      u2->at(1).get_string("tool_use_id") == "call_b",
                  "Anthropic coalesces consecutive tool results into one user turn");
        }
    }

    // 2. Anthropic SSE streaming parser (thinking + text + tool_use + cache usage)
    {
        AnthropicSseState st;
        std::vector<ModelEvent> deltas;
        Check(st.Feed(R"({"type":"message_start","message":{"usage":{"input_tokens":120,"cache_read_input_tokens":800}}})", deltas),
              "Anthropic SSE message_start");
        Check(st.Feed(R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})", deltas),
              "Anthropic SSE thinking start");
        Check(st.Feed(R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Step 1. "}})", deltas),
              "Anthropic SSE thinking delta");
        Check(st.Feed(R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})", deltas),
              "Anthropic SSE text start");
        Check(st.Feed(R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"Done!"}})", deltas),
              "Anthropic SSE text delta");
        Check(st.Feed(R"({"type":"content_block_start","index":2,"content_block":{"type":"tool_use","id":"toolu_1","name":"list_folder","input":{}}})", deltas),
              "Anthropic SSE tool_use start");
        Check(st.Feed(R"({"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"{\"path\":"}})", deltas),
              "Anthropic SSE input_json part 1");
        Check(st.Feed(R"({"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"\"D:\\\\Work\"}"}})", deltas),
              "Anthropic SSE input_json part 2");
        Check(st.Feed(R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":45}})", deltas),
              "Anthropic SSE message_delta");
        Check(st.Feed(R"({"type":"message_stop"})", deltas), "Anthropic SSE message_stop");

        Check(st.message_stopped, "Anthropic SSE stopped flag");
        Check(deltas.size() == 2 && deltas[0].kind == ModelEvent::Kind::Reasoning &&
                  deltas[1].kind == ModelEvent::Kind::Content,
              "Anthropic SSE emitted Reasoning + Content deltas");
        const ModelEvent fin = st.BuildFinished();
        Check(fin.reasoning == "Step 1. " && fin.content == "Done!", "Anthropic SSE assembled text & reasoning");
        Check(fin.prompt_tokens == 920 && fin.completion_tokens == 45, "Anthropic SSE usage includes cache tokens");
        Check(fin.tool_calls.size() == 1 && fin.tool_calls[0].id == "toolu_1" &&
                  fin.tool_calls[0].name == "list_folder" &&
                  fin.tool_calls[0].arguments == "{\"path\":\"D:\\\\Work\"}",
              "Anthropic SSE assembled chunked tool_use");
    }

    // Create isolated temp directory
    wchar_t tmp_buf[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp_buf);
    const fs::path dir = fs::path(tmp_buf) / (L"pulse_batch7_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // 3. Reusable workflow store CRUD
    {
        WorkflowRule r1, r2, upd;
        Check(UpsertWorkflow("\xE6\x9C\x88\xE5\xBA\xA6\xE5\x8F\x91\xE7\xA5\xA8\xE5\xBD\x92\xE6\xA1\xA3",
                             "Invoices to Finance", "Search PDF invoices and move by month into D:\\Finance", r1,
                             dir.wstring()),
              "UpsertWorkflow Chinese rule");
        Check(UpsertWorkflow("Photo Sort", "By date", "Rename with {taken}_{n} and group by year", r2, dir.wstring()),
              "UpsertWorkflow English rule");
        Check(UpsertWorkflow("photo sort", "By date v2", "Rename with {taken:yyyyMMdd}_{n}", upd, dir.wstring()),
              "UpsertWorkflow case-insensitive update");
        Check(upd.id == r2.id, "UpsertWorkflow keeps id on update");

        const auto loaded = LoadWorkflows(dir.wstring());
        Check(loaded.size() == 2, "LoadWorkflows returns 2 rules");
        const std::string prompt = FormatWorkflowsForPrompt(loaded);
        Check(prompt.find("D:\\Finance") != std::string::npos && prompt.find("yyyyMMdd") != std::string::npos,
              "FormatWorkflowsForPrompt renders updated rules");
        Check(RemoveWorkflow("photo sort", dir.wstring()) == 1 && LoadWorkflows(dir.wstring()).size() == 1,
              "RemoveWorkflow deletes by name");
    }

    // 4. Settings external mcp_servers round-trip
    {
        Settings s;
        ExternalMcpServer srv;
        srv.name = "git_helper";
        srv.transport = "stdio";
        srv.command = "python";
        srv.args = {"-u", "server.py"};
        s.assistant.mcp_servers.push_back(srv);
        ExternalMcpServer srv2;
        srv2.name = "remote_docs";
        srv2.transport = "http";
        srv2.url = "https://mcp.example.com/rpc";
        srv2.enabled = false;
        s.assistant.mcp_servers.push_back(srv2);

        const std::string serialized = SerializeSettings(s);
        bool malformed = false;
        const Settings parsed = ParseSettings(serialized, &malformed);
        Check(!malformed && parsed.assistant.mcp_servers.size() == 2, "ai.json mcp_servers round-trip count");
        Check(parsed.assistant.mcp_servers[0].name == "git_helper" &&
                  parsed.assistant.mcp_servers[0].args.size() == 2 &&
                  parsed.assistant.mcp_servers[1].transport == "http" && !parsed.assistant.mcp_servers[1].enabled,
              "ai.json mcp_servers preserves stdio/http fields");
    }

    // 5. LIVE real-process stdio MCP JSON-RPC 2.0 round-trip (Python test MCP server)
    {
        const fs::path script = dir / L"mock_mcp_server.py";
        static constexpr wchar_t kPyServer[] =
            L"import json, sys\n"
            L"for line in sys.stdin:\n"
            L"    line = line.strip()\n"
            L"    if not line: continue\n"
            L"    msg = json.loads(line)\n"
            L"    mid = msg.get('id')\n"
            L"    method = msg.get('method')\n"
            L"    if method == 'initialize':\n"
            L"        resp = {'jsonrpc':'2.0','id':mid,'result':{'protocolVersion':'2024-11-05','capabilities':{'tools':{}},'serverInfo':{'name':'demo','version':'1.0'}}}\n"
            L"        sys.stdout.write(json.dumps(resp) + '\\n'); sys.stdout.flush()\n"
            L"    elif method == 'tools/list':\n"
            L"        resp = {'jsonrpc':'2.0','id':mid,'result':{'tools':[{'name':'add_numbers','description':'Adds a and b','inputSchema':{'type':'object','properties':{'a':{'type':'integer'},'b':{'type':'integer'}}}}]}}\n"
            L"        sys.stdout.write(json.dumps(resp) + '\\n'); sys.stdout.flush()\n"
            L"    elif method == 'tools/call':\n"
            L"        args = msg.get('params',{}).get('arguments',{})\n"
            L"        total = int(args.get('a',0)) + int(args.get('b',0))\n"
            L"        resp = {'jsonrpc':'2.0','id':mid,'result':{'content':[{'type':'text','text':f'sum={total}'}]}}\n"
            L"        sys.stdout.write(json.dumps(resp) + '\\n'); sys.stdout.flush()\n";
        Check(pulse::WriteUtf8FileAtomic(script.wstring(), kPyServer), "Write mock Python MCP server");

        ExternalMcpServer srv;
        srv.name = "demo";
        srv.transport = "stdio";
        srv.command = "python";
        srv.args = {"-u", json::Utf8(script.wstring())};

        std::atomic<bool> cancelled{false};
        const McpListResult listed = ListMcpServerTools(srv, 8000, cancelled);
        Check(listed.ok && listed.tools.size() == 1, "LIVE stdio MCP tools/list succeeds");
        if (listed.tools.size() == 1) {
            Check(listed.tools[0].full_id == "mcp__demo__add_numbers" &&
                      listed.tools[0].tool_name == "add_numbers",
                  "LIVE stdio MCP tool id & name match");
        }

        json::Value call_args = json::Value::Object();
        call_args.set("a", 17);
        call_args.set("b", 25);
        const McpCallResult called = CallMcpServerTool(srv, "add_numbers", call_args, 8000, cancelled);
        Check(called.ok && !called.is_error && called.text == "sum=42",
              "LIVE stdio MCP tools/call returns sum=42");
    }

    fs::remove_all(dir, ec);
    std::printf("[SUMMARY] ai_batch7_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
