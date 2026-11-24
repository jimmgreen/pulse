// ai_web_search_test.cpp - Unit + live real-machine tests for Pulse 助手 web search,
// SSRF protection, HTML extraction, provider-native search payloads, and SSE citations.
#include "../app/ai_model_stream.h"
#include "../app/ai_providers.h"
#include "../app/ai_settings.h"
#include "../app/ai_web_search.h"
#include "../common/json_value.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pulse;
using namespace pulse::app::ai;

namespace {

int g_pass = 0, g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    ok ? ++g_pass : ++g_fail;
}

void TestUrlValidation() {
    std::string code, msg;
    Check(ValidatePublicHttpUrl("https://docs.python.org/zh-cn/3.13/whatsnew/3.13.html", &code, &msg),
          "public https url allowed");
    Check(ValidatePublicHttpUrl("http://example.com/path?a=1&b=2"), "public http url allowed");

    // Schemes & malformed
    Check(!ValidatePublicHttpUrl("file:///C:/Windows/win.ini", &code) && code == "unsupported_scheme",
          "file:// scheme blocked");
    Check(!ValidatePublicHttpUrl("javascript:alert(1)", &code) && code == "unsupported_scheme",
          "javascript: scheme blocked");
    Check(!ValidatePublicHttpUrl("ftp://example.com/file.zip", &code) && code == "unsupported_scheme",
          "ftp:// scheme blocked");
    Check(!ValidatePublicHttpUrl("https://user:pass@example.com/", &code), "embedded credentials blocked");
    Check(!ValidatePublicHttpUrl("https://example.com/a b", &code), "whitespace in url blocked");

    // Local / private IPv4 & hostnames (SSRF guard)
    Check(!ValidatePublicHttpUrl("http://localhost:11434/v1/models"), "localhost blocked");
    Check(!ValidatePublicHttpUrl("http://foo.localhost/"), "*.localhost blocked");
    Check(!ValidatePublicHttpUrl("http://printer.local/"), "*.local blocked");
    Check(!ValidatePublicHttpUrl("http://127.0.0.1:8080/"), "127.0.0.1 blocked");
    Check(!ValidatePublicHttpUrl("http://10.0.0.5/"), "10.0.0.0/8 blocked");
    Check(!ValidatePublicHttpUrl("http://172.16.0.1/"), "172.16.0.0/12 blocked");
    Check(!ValidatePublicHttpUrl("http://192.168.1.1/"), "192.168.0.0/16 blocked");
    Check(!ValidatePublicHttpUrl("http://169.254.169.254/latest/meta-data/"), "AWS/cloud metadata IP blocked");
    Check(!ValidatePublicHttpUrl("http://100.64.0.1/"), "CGNAT 100.64.0.0/10 blocked");
    Check(!ValidatePublicHttpUrl("http://0.0.0.0/"), "0.0.0.0 blocked");
    Check(!ValidatePublicHttpUrl("http://2130706433/"), "decimal loopback integer IP blocked");
    Check(!ValidatePublicHttpUrl("http://[::1]/"), "IPv6 ::1 blocked");
    Check(!ValidatePublicHttpUrl("http://[fe80::1]/"), "IPv6 link-local blocked");
    Check(!ValidatePublicHttpUrl("http://[fc00::1]/"), "IPv6 ULA blocked");
    Check(!ValidatePublicHttpUrl("http://[::ffff:127.0.0.1]/"), "IPv4-mapped loopback blocked");
    Check(ValidatePublicHttpUrl("https://8.8.8.8/"), "public IPv4 allowed");
}

void TestHtmlParsing() {
    Check(DecodeHtmlEntities("A &amp; B &lt;3&gt; &quot;ok&quot; &#39;hi&#x21; &#20013;") ==
              "A & B <3> \"ok\" 'hi! \xE4\xB8\xAD",
          "DecodeHtmlEntities named, hex, decimal CJK");

    Check(DecodeDuckDuckGoRedirectUrl("//duckduckgo.com/l/?uddg=https%3A%2F%2Fcmake.org%2Fdoc%2F&amp;rut=abc") ==
              "https://cmake.org/doc/",
          "DecodeDuckDuckGoRedirectUrl decodes uddg");
    Check(DecodeDuckDuckGoRedirectUrl("https://duckduckgo.com/y.js?ad=1").empty(),
          "DecodeDuckDuckGoRedirectUrl filters ad redirect");

    // Base64url("https://www.python.org/") == "aHR0cHM6Ly93d3cucHl0aG9uLm9yZy8"
    Check(DecodeBingRedirectUrl("https://www.bing.com/ck/a?!&&p=123&u=a1aHR0cHM6Ly93d3cucHl0aG9uLm9yZy8&ntb=1") ==
              "https://www.python.org/",
          "DecodeBingRedirectUrl decodes u=a1 base64url");

    const std::string ddg_html = R"HTML(
        <div class="result">
            <a class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fninja-build.org%2F&amp;rut=1">Ninja &mdash; Build</a>
            <a class="result__snippet">Small build system with a focus on <b>speed</b>.</a>
        </div>
        <div class="result">
            <a class="result__a" href="https://duckduckgo.com/y.js?ad=1">Sponsored</a>
        </div>
    )HTML";
    const auto ddg = ParseDuckDuckGoHtml(ddg_html, 5);
    Check(ddg.size() == 1 && ddg[0].url == "https://ninja-build.org/" && ddg[0].source == "ninja-build.org" &&
              ddg[0].snippet == "Small build system with a focus on speed.",
          "ParseDuckDuckGoHtml extracts result and skips ads");

    const std::string bing_html = R"HTML(
        <ol id="b_results">
            <li class="b_algo">
                <h2><a href="https://www.bing.com/ck/a?!&&u=a1aHR0cHM6Ly93d3cucHl0aG9uLm9yZy8&ntb=1">Welcome to <strong>Python</strong></a></h2>
                <div class="b_caption"><p>Official Python homepage &amp; docs.</p></div>
            </li>
        </ol>
    )HTML";
    const auto bing = ParseBingHtml(bing_html, 5);
    Check(bing.size() == 1 && bing[0].url == "https://www.python.org/" && bing[0].title == "Welcome to Python" &&
              bing[0].snippet == "Official Python homepage & docs.",
          "ParseBingHtml extracts title, decoded URL, and snippet");

    const std::string page = R"HTML(
        <html><head><title>Test &amp; Page</title><style>.a{color:red;}</style></head>
        <body>
            <nav>Menu Home About</nav>
            <main>
                <h1>Main Heading</h1>
                <script>alert("secret");</script>
                <!-- hidden comment -->
                <p>First paragraph with enough content to exceed the two hundred byte threshold so main extraction is preferred over navigation boilerplate. Adding more descriptive sentences about Pulse file manager web search integration and readable HTML text extraction.</p>
                <ul><li>Bullet one</li><li>Bullet two</li></ul>
            </main>
        </body></html>
    )HTML";
    std::string title;
    bool trunc = false;
    const std::string text = HtmlToReadableText(page, &title, 5000, &trunc);
    Check(title == "Test & Page", "HtmlToReadableText extracts decoded title");
    Check(text.find("# Main Heading") != std::string::npos && text.find("- Bullet one") != std::string::npos &&
              text.find("secret") == std::string::npos && text.find("Menu Home") == std::string::npos && !trunc,
          "HtmlToReadableText prefers <main>, strips script/style/comments");

    bool trunc2 = false;
    const std::string short_text = HtmlToReadableText("<p>\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95</p>",
                                                      nullptr, 2, &trunc2);
    Check(short_text == "\xE4\xB8\xAD\xE6\x96\x87" && trunc2, "HtmlToReadableText truncates on UTF-8 char boundary");
}

void TestProviderNativeSearchAndCitations() {
    Check(SupportsNativeWebSearch(ProviderById("qwen"), "qwen-plus"), "qwen-plus supports native search");
    Check(!SupportsNativeWebSearch(ProviderById("qwen"), "qwen3-coder-plus"), "qwen3-coder does not use native search");
    Check(SupportsNativeWebSearch(ProviderById("openrouter"), "deepseek/deepseek-chat"), "openrouter supports native search");
    Check(SupportsNativeWebSearch(ProviderById("zhipu"), "glm-4.7"), "zhipu glm-4.7 supports native search");
    Check(!SupportsNativeWebSearch(ProviderById("deepseek"), "deepseek-flash"), "deepseek has no native search field");

    {
        json::Value body = json::Value::Object();
        Check(AddNativeWebSearch(ProviderById("qwen"), "qwen-plus", body), "qwen AddNativeWebSearch returns true");
        const std::string s = json::Serialize(body);
        Check(s.find("\"enable_search\":true") != std::string::npos &&
                  s.find("\"enable_source\":true") != std::string::npos,
              "qwen native search payload fields");
    }
    {
        json::Value body = json::Value::Object();
        Check(AddNativeWebSearch(ProviderById("openrouter"), "openai/gpt-5", body), "openrouter AddNativeWebSearch");
        Check(json::Serialize(body).find("\"id\":\"web\"") != std::string::npos, "openrouter plugins web");
    }
    {
        json::Value body = json::Value::Object();
        body.set("tools", json::Value::Array());
        Check(AddNativeWebSearch(ProviderById("zhipu"), "glm-5", body), "zhipu AddNativeWebSearch");
        Check(json::Serialize(body).find("\"type\":\"web_search\"") != std::string::npos, "zhipu web_search tool");
    }

    // SSE citation parsing across formats
    std::vector<ModelCitation> cits;
    CollectModelCitations(
        R"JSON({"choices":[{"delta":{"annotations":[{"type":"url_citation","url_citation":{"url":"https://docs.python.org/3/","title":"Python Docs"}}]}}]})JSON",
        cits);
    CollectModelCitations(
        R"JSON({"citations":["https://docs.python.org/3/","https://cmake.org/","file:///etc/passwd"]})JSON", cits);
    CollectModelCitations(
        R"JSON({"search_info":{"search_results":[{"title":"CMake Home","url":"https://cmake.org/"}]}})JSON", cits);
    CollectModelCitations(
        R"JSON({"web_search":[{"title":"Zhipu Result","link":"https://example.org/news"}]})JSON", cits);
    Check(cits.size() == 3, "CollectModelCitations deduplicates and ignores non-http URLs");
    if (cits.size() == 3) {
        Check(cits[0].url == "https://docs.python.org/3/" && cits[0].title == "Python Docs",
              "url_citation parsed");
        Check(cits[1].url == "https://cmake.org/" && cits[1].title == "CMake Home",
              "title backfilled when duplicate URL later provides title");
        Check(cits[2].url == "https://example.org/news" && cits[2].title == "Zhipu Result",
              "zhipu web_search citation parsed");
    }

    // ai_settings web_search round-trip
    Settings s;
    s.assistant.web_search.enabled = false;
    s.assistant.web_search.provider = "bocha";
    s.assistant.web_search.endpoint = "https://search.example.com";
    s.assistant.web_search.native = true;
    const std::string json_text = SerializeSettings(s);
    bool malformed = true;
    const Settings parsed = ParseSettings(json_text, &malformed);
    Check(!malformed && !parsed.assistant.web_search.enabled && parsed.assistant.web_search.provider == "bocha" &&
              parsed.assistant.web_search.endpoint == "https://search.example.com" &&
              parsed.assistant.web_search.native,
          "ai_settings web_search round-trip");

    const Settings bool_compat = ParseSettings(R"JSON({"assistant":{"web_search":false}})JSON", &malformed);
    Check(!malformed && !bool_compat.assistant.web_search.enabled && bool_compat.assistant.web_search.provider == "auto",
          "ai_settings web_search boolean compat");
}

void TestLiveNetwork() {
    std::atomic<bool> cancelled{false};

    // SSRF guard enforced at FetchWebUrl entry too
    const WebFetchResponse blocked = FetchWebUrl("http://127.0.0.1:80/", 4000, cancelled);
    Check(!blocked.ok && (blocked.error_code == "blocked_host" || blocked.error_code == "blocked_ip"),
          "live FetchWebUrl rejects 127.0.0.1");

    // Live English search
    WebSearchConfig cfg;
    cfg.provider = "auto";
    const WebSearchResponse en = WebSearch("CMake Ninja generator", 5, cfg, cancelled);
    std::printf("  [live en] ok=%d engine=%s count=%zu err=%s\n", en.ok ? 1 : 0, en.engine.c_str(), en.results.size(),
                en.error_message.c_str());
    for (size_t i = 0; i < en.results.size() && i < 3; ++i)
        std::printf("    #%zu [%s] %s -> %s\n", i + 1, en.results[i].source.c_str(), en.results[i].title.c_str(),
                    en.results[i].url.c_str());
    Check(en.ok && !en.results.empty() && !en.results[0].url.empty() && !en.results[0].title.empty(),
          "live WebSearch English query returns real results");

    // Live Chinese search
    const WebSearchResponse zh = WebSearch("Python 3.13 \xE6\x96\xB0\xE7\x89\xB9\xE6\x80\xA7", 5, cfg, cancelled);
    std::printf("  [live zh] ok=%d engine=%s count=%zu err=%s\n", zh.ok ? 1 : 0, zh.engine.c_str(), zh.results.size(),
                zh.error_message.c_str());
    for (size_t i = 0; i < zh.results.size() && i < 3; ++i)
        std::printf("    #%zu [%s] %s -> %s\n", i + 1, zh.results[i].source.c_str(), zh.results[i].title.c_str(),
                    zh.results[i].url.c_str());
    Check(zh.ok && !zh.results.empty() && !zh.results[0].url.empty(),
          "live WebSearch Chinese query returns real results");

    // Live webpage fetch
    const std::string fetch_target = !en.results.empty() ? en.results[0].url : "https://example.com/";
    const WebFetchResponse page = FetchWebUrl(fetch_target, 3000, cancelled);
    std::printf("  [live fetch] ok=%d status=%d title=\"%s\" len=%zu url=%s err=%s\n", page.ok ? 1 : 0, page.http_status,
                page.title.c_str(), page.text.size(), page.url.c_str(), page.error_message.c_str());
    Check(page.ok && page.http_status == 200 && page.text.size() >= 100,
          "live FetchWebUrl extracts readable text from real webpage");
}

} // namespace

int main(int argc, char** argv) {
    bool run_live = true;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--offline") == 0) run_live = false;

    TestUrlValidation();
    TestHtmlParsing();
    TestProviderNativeSearchAndCitations();
    if (run_live) TestLiveNetwork();

    std::printf("Summary: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
