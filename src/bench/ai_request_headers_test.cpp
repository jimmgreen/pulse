// ai_request_headers_test.cpp - The headers an OpenAI-format call carries
// (ai_model_stream.h RequestHeaders): Pulse names itself, services that route
// by conversation (OpenCode) get the conversation's id, and a header value
// can never end the header it is in. No network: the builder is pure.
#include "../app/ai_model_stream.h"

#include <cstdio>
#include <string>

using namespace pulse;
namespace ai = pulse::app::ai;

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", what);
    if (!ok) ++g_failed;
}

std::wstring Lower(std::wstring text) {
    for (wchar_t& c : text)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return text;
}

// Where `needle` starts in `hay`; -1 when it is not there.
long Pos(const std::wstring& hay, const std::wstring& needle) {
    const long hn = static_cast<long>(hay.size());
    const long nn = static_cast<long>(needle.size());
    if (nn == 0 || hn < nn) return -1;
    for (long i = 0; i + nn <= hn; ++i)
        if (hay.compare(static_cast<size_t>(i), static_cast<size_t>(nn), needle) == 0) return i;
    return -1;
}

long Pos(const std::wstring& hay, const wchar_t* needle) { return Pos(hay, std::wstring(needle)); }

bool Has(const std::wstring& headers, const wchar_t* text) { return Pos(Lower(headers), Lower(std::wstring(text))) >= 0; }

// A header line by its name, whatever the value (the version moves).
bool HasField(const std::wstring& headers, const wchar_t* name) {
    const std::wstring lower = Lower(headers);
    return Pos(lower, Lower(std::wstring(name)) + L": ") == 0 ||
           Pos(lower, L"\r" + Lower(std::wstring(name)) + L": ") >= 0;
}

// A line a value carrying CR LF would have added (injection).
bool Injected(const std::wstring& headers) {
    const std::wstring lower = Lower(headers);
    return Pos(lower, L"\rauthorization") >= 0 || Pos(lower, L"\r\n\r\n") >= 0;
}

}  // namespace

int main() {
    const std::wstring session = L"20260101-120000-beef";

    // Every call names itself: OpenCode asks a client to send its own
    // User-Agent rather than a generic one.
    const std::wstring deepseek = ai::RequestHeaders(L"https://api.deepseek.com", L"");
    Check(HasField(deepseek, L"user-agent"), "a call names Pulse as its User-Agent");
    Check(Pos(deepseek, L"x-opencode-session") < 0, "a service that does not route by conversation gets no session header");
    Check(deepseek.size() >= 2 && deepseek.compare(deepseek.size() - 2, 2, L"\r\n") == 0,
          "the headers end with CR LF");

    // OpenCode: the conversation's id, or the call is answered
    // "Request is missing x-opencode-session and cannot be routed efficiently".
    const std::wstring go = ai::RequestHeaders(L"https://opencode.ai/inference/go/openai/v1", session);
    Check(Has(go, L"x-opencode-session: 20260101-120000-beef"), "OpenCode gets the conversation's id");
    Check(HasField(go, L"user-agent"), "OpenCode gets Pulse's name too");
    // The same conversation, the same id: this is what the service caches by.
    Check(go == ai::RequestHeaders(L"https://opencode.ai/inference/go/openai/v1", session),
          "one conversation keeps one id");

    // The paid host, a subdomain and an upper-case host all route the same.
    Check(Has(ai::RequestHeaders(L"https://opencode.ai/inference", session), L"x-opencode-session"),
          "the paid host takes the id");
    Check(Has(ai::RequestHeaders(L"https://api.opencode.ai/v1", session), L"x-opencode-session"),
          "a subdomain takes the id");
    Check(Has(ai::RequestHeaders(L"https://OpenCode.AI/v1", session), L"x-opencode-session"),
          "the host is not case-sensitive");
    Check(Pos(ai::RequestHeaders(L"https://notopencode.ai/v1", session), L"x-opencode-session") < 0,
          "a host that merely ends the same way is not OpenCode");
    Check(Pos(ai::RequestHeaders(L"https://opencode.ai.evil.test/v1", session), L"x-opencode-session") < 0,
          "a host OpenCode is a prefix of is not OpenCode");

    // An empty id leaves the header out rather than sending it blank.
    Check(Pos(ai::RequestHeaders(L"https://opencode.ai/v1", L""), L"x-opencode-session") < 0,
          "no id, no session header");

    // A header ends at CR or LF: a value carrying one would add a header of
    // its own (here, an Authorization line). Spaces stay, they are no break.
    const std::wstring injected =
        ai::RequestHeaders(L"https://opencode.ai/v1", L"good\r\nAuthorization: Bearer stolen");
    Check(Has(injected, L"x-opencode-session: goodAuthorization: Bearer stolen"),
          "a value's CR LF stay inside its line");
    Check(!Injected(injected), "a value cannot inject a header");
    Check(Pos(ai::RequestHeaders(L"https://opencode.ai/v1", L"\r\n\r\n"), L"x-opencode-session") < 0,
          "a value of CR LFs alone leaves the header out");

    // The user's own address, or one that is no URL at all: Pulse still names
    // itself, and nothing is guessed from a host that did not parse.
    Check(HasField(ai::RequestHeaders(L"not a url", session), L"user-agent"),
          "an address that is not a URL still names Pulse");
    Check(Pos(ai::RequestHeaders(L"not a url", session), L"x-opencode-session") < 0,
          "and gets no session header");

    std::printf("%s ai_request_headers_test (%d failed)\n", g_failed ? "[FAIL]" : "[PASS]", g_failed);
    return g_failed ? 1 : 0;
}
