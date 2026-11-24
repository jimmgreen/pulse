// ai_mcp_client.h - External MCP (Model Context Protocol) JSON-RPC 2.0 client
// over stdio pipes or HTTP for Pulse 助手.
#pragma once

#include "ai_settings.h"
#include "../common/json_value.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app::ai {

struct McpToolDef {
    std::string server_name;
    std::string tool_name;
    std::string full_id;        // "mcp__<server>__<tool>"
    std::string description;
    json::Value input_schema;   // JSON Schema object
};

struct McpListResult {
    bool ok = false;
    std::vector<McpToolDef> tools;
    std::string error_message;
};

struct McpCallResult {
    bool ok = false;
    bool is_error = false;
    std::string text;
    json::Value raw;
    std::string error_message;
};

// Produces a function-calling-safe identifier `mcp__<server>__<tool>`.
std::string MakeMcpToolId(std::string_view server_name, std::string_view tool_name);

// Connects to an external MCP server (stdio or HTTP), performs `initialize` +
// `tools/list`, and returns its advertised tools. Blocking — run on worker thread.
McpListResult ListMcpServerTools(const ExternalMcpServer& server, uint32_t timeout_ms,
                                 const std::atomic<bool>& cancelled);

// Connects to an external MCP server and invokes `tools/call`. Blocking — run on
// worker thread ONLY after user confirmation in Pulse.
McpCallResult CallMcpServerTool(const ExternalMcpServer& server, const std::string& tool_name,
                                const json::Value& arguments, uint32_t timeout_ms,
                                const std::atomic<bool>& cancelled);

} // namespace pulse::app::ai
