// ai_web_search.h - Web search and webpage text extraction for Pulse 助手.
//
// Supports configurable search APIs (Tavily, Bocha, Serper, SearXNG) and a
// zero-key fallback chain (DuckDuckGo HTML -> Bing HTML) over WinHTTP, plus
// SSRF-guarded webpage fetching with clean HTML-to-text extraction.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app::ai {

struct WebSearchConfig {
    std::string provider = "auto";   // "auto" | "ddg" | "bing" | "tavily" | "bocha" | "serper" | "searxng"
    std::string endpoint;            // custom endpoint (e.g. SearXNG base URL)
    std::wstring api_key;            // optional API key (wiped after use)
};

struct WebSearchItem {
    std::string title;
    std::string url;
    std::string snippet;
    std::string source;              // host name, e.g. "docs.python.org"
};

struct WebSearchResponse {
    bool ok = false;
    std::string engine;              // engine that answered ("ddg", "bing", "tavily", ...)
    std::string answer;              // optional concise summary from API search engines
    std::vector<WebSearchItem> results;
    std::string error_code;
    std::string error_message;
};

struct WebFetchResponse {
    bool ok = false;
    std::string url;                 // final URL after redirects
    std::string title;
    std::string content_type;
    std::string text;                // extracted readable UTF-8 text
    bool truncated = false;
    int http_status = 0;
    std::string error_code;
    std::string error_message;
};

// Pure validation and parsing helpers (unit-tested without network).
bool IsBlockedIpv4(uint32_t host_order_ip);
bool IsBlockedIpv6(const uint8_t bytes[16]);
bool ValidatePublicHttpUrl(std::string_view url, std::string* error_code = nullptr,
                           std::string* error_message = nullptr);
std::string ExtractUrlHost(std::string_view url);
std::string DecodeHtmlEntities(std::string_view input);
std::string DecodeDuckDuckGoRedirectUrl(std::string_view href);
std::string DecodeBingRedirectUrl(std::string_view href);
std::vector<WebSearchItem> ParseDuckDuckGoHtml(std::string_view html, size_t max_results);
std::vector<WebSearchItem> ParseBingHtml(std::string_view html, size_t max_results);
std::string HtmlToReadableText(std::string_view html, std::string* title_out = nullptr, size_t max_chars = 12000,
                               bool* truncated = nullptr);

// Live network operations (blocking WinHTTP; invoke on a worker thread).
WebSearchResponse WebSearch(std::string_view query, size_t max_results, WebSearchConfig config,
                            const std::atomic<bool>& cancelled);
WebFetchResponse FetchWebUrl(std::string_view url, size_t max_chars, const std::atomic<bool>& cancelled);

} // namespace pulse::app::ai
