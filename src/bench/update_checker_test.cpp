#include "../app/update_checker.h"
#include "../app/update_transport.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

namespace {

bool Report(const char* name, bool passed) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}

constexpr char kPublicKey[] =
    "045997e7ba58e53b5a508a21c9b684a58891fc87d73cb9d44a183751f0935b2ba9"
    "e5fcfdc15ce19210655359be709204ef0eed2c097b57d037fc5f3997a621d71c";

const std::string kManifest =
    "{\n"
    "  \"schema\": 1,\n"
    "  \"version\": \"9.8.7\",\n"
    "  \"minimum_windows_build\": 19045,\n"
    "  \"download_page\": \"https://updates.example.test/pulse\",\n"
    "  \"installer_sha256\": "
        "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\n"
    "  \"signature\": "
        "\"SNdGtlX32+/cB9d812GBxuW6kemSIB68TmXjumgUDw8QMLTRcYXjFwBFcMmRa/"
        "EdiuecedQ2TAXB8CUt5lRzxA==\"\n"
    "}\n";

bool TestFallback() {
    using namespace pulse::app;
    const std::wstring url = L"https://github.com/jimmgreen/pulse/releases/download/v1.0/PulseSetup.exe";
    const auto proxy = AcceleratedUpdateUrl(url);
    bool passed = Report("release accelerator preserves original URL", proxy == L"https://ghproxy.net/" + url);
    passed &= Report("latest manifest supports acceleration", !AcceleratedUpdateUrl(
        L"https://github.com/jimmgreen/pulse/releases/latest/download/update-manifest.json").empty());
    for (const auto invalid : {L"https://example.test/a", L"http://github.com/a/b/releases/download/v1/a",
        L"https://github.com.evil/a/b/releases/download/v1/a", L"https://github.com@evil/a/b/releases/download/v1/a",
        L"https://github.com/a/b/blob/main/a", L"https://github.com/a/b/releases/latest/download/",
        L"https://github.com/a/b/releases/download/v1/a?token=secret"})
        passed &= Report("unrelated or credential-bearing URL is not proxied", AcceleratedUpdateUrl(invalid).empty());
    std::atomic<bool> cancelled{false};
    UpdateError category = UpdateError::None;
    DWORD error = 0;
    int calls = 0, resets = 0;
    std::string output;
    const auto reset = [&] { ++resets; output.clear(); return true; };
    const auto consume = [&](const void* data, DWORD size) { output.append(static_cast<const char*>(data), size); return true; };
    UpdateResponseReader reader = [&](std::wstring_view source, uint64_t maximum,
        const std::atomic<bool>&, const std::function<bool(const void*, DWORD)>& sink,
        UpdateError& failure, DWORD& code) {
        ++calls;
        passed &= Report("attempt preserves response size limit", maximum == 1024);
        if (calls == 1) {
            passed &= Report("automatic mode tries ghproxy.net first", source == proxy);
            sink("partial", 7);
            failure = UpdateError::Network;
            code = ERROR_TIMEOUT;
            return false;
        }
        passed &= Report("network failure switches to gh-proxy.com", source == L"https://gh-proxy.com/" + url);
        return sink("complete", 8);
    };
    passed &= Report("partial response is discarded on fallback", ReadUpdateWithFallback(url, 1024,
        cancelled, reset, consume, category, error, reader) && output == "complete" && resets == 2 &&
        calls == 2 && category == UpdateError::None && error == 0);
    calls = resets = 0;
    reader = [&](std::wstring_view source, uint64_t, const std::atomic<bool>&,
        const std::function<bool(const void*, DWORD)>& sink, UpdateError&, DWORD&) {
        ++calls;
        passed &= Report("default acceleration skips initial GitHub attempt", source == proxy);
        return sink("ok", 2);
    };
    passed &= Report("successful preferred source avoids extra requests", ReadUpdateWithFallback(url, 1024,
        cancelled, reset, consume, category, error, reader) && calls == 1 && resets == 1);
    calls = 0;
    reader = [&](std::wstring_view source, uint64_t, const std::atomic<bool>&,
        const std::function<bool(const void*, DWORD)>& sink, UpdateError& result, DWORD& code) {
        ++calls;
        if (calls == 1) {
            passed &= Report("preferred accelerator is attempted first", source == proxy);
            result = UpdateError::HttpStatus; code = 503; return false;
        }
        if (calls == 2) {
            passed &= Report("second accelerator precedes GitHub", source == L"https://gh-proxy.com/" + url);
            sink("partial", 7);
            result = UpdateError::HttpStatus; code = 502; return false;
        }
        passed &= Report("unavailable accelerators fall back to GitHub", source == url);
        return sink("ok", 2);
    };
    passed &= Report("accelerator HTTP failure recovers", ReadUpdateWithFallback(url, 1024,
        cancelled, reset, consume, category, error, reader) && calls == 3 && output == "ok");
    calls = 0;
    reader = [&](std::wstring_view, uint64_t, const std::atomic<bool>&,
        const std::function<bool(const void*, DWORD)>&, UpdateError& result, DWORD& code) {
        ++calls; result = UpdateError::Network; code = ERROR_TIMEOUT; return false;
    };
    passed &= Report("all sources failing stops after three attempts", !ReadUpdateWithFallback(url, 1024,
        cancelled, reset, consume, category, error, reader) && calls == 3 && error == ERROR_TIMEOUT);
    calls = 0;
    passed &= Report("custom update hosts never use public accelerator", !ReadUpdateWithFallback(
        L"https://updates.example.test/manifest.json", 1024, cancelled, reset, consume, category, error,
        reader) && calls == 1);
    for (const auto failure : {UpdateError::LocalIo, UpdateError::ResponseTooLarge, UpdateError::InsecureUrl,
                              UpdateError::InvalidSignature}) {
        calls = 0;
        reader = [&](std::wstring_view, uint64_t, const std::atomic<bool>&,
            const std::function<bool(const void*, DWORD)>&, UpdateError& result, DWORD& code) {
            ++calls; result = failure; code = ERROR_INVALID_DATA; return false;
        };
        passed &= Report("non-network failure never retries", !ReadUpdateWithFallback(url, 1024,
            cancelled, reset, consume, category, error, reader) && calls == 1);
    }
    calls = 0;
    reader = [&](std::wstring_view, uint64_t, const std::atomic<bool>&,
        const std::function<bool(const void*, DWORD)>&, UpdateError& result, DWORD& code) {
        ++calls; cancelled = true; result = UpdateError::Network; code = ERROR_CANCELLED; return false;
    };
    passed &= Report("cancellation prevents fallback", !ReadUpdateWithFallback(url, 1024,
        cancelled, reset, consume, category, error, reader) && calls == 1);
    calls = resets = 0;
    passed &= Report("pre-cancelled download never resets or requests", !ReadUpdateWithFallback(url, 1024,
        cancelled, reset, consume, category, error, reader) && calls == 0 && resets == 0);
    cancelled = false;
    passed &= Report("destination reset failure never requests", !ReadUpdateWithFallback(url, 1024,
        cancelled, [] { SetLastError(ERROR_DISK_FULL); return false; }, consume, category, error, reader) &&
        calls == 0 && category == UpdateError::LocalIo && error == ERROR_DISK_FULL);
    return passed;
}

} // namespace

int main(int argc, char** argv) {
    using namespace pulse::app;
    if (!TestFallback()) return 1;
    if (argc == 2 && std::string_view(argv[1]) == "--check-live") {
        HWND window = CreateWindowExW(0, L"STATIC", L"Update check test", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        UpdateChecker checker;
        const bool started = checker.CheckAsync(window, WM_APP + 1);
        UpdateResult result;
        bool received = false;
        const auto deadline = GetTickCount64() + 60000;
        while (started && GetTickCount64() < deadline && !(received = checker.TakeResult(result))) Sleep(20);
        checker.Stop();
        DestroyWindow(window);
        const bool passed = started && received && result.error == UpdateError::None && !result.version.empty();
        std::printf("error=%u diagnostic=%lu\n", static_cast<unsigned>(result.error), result.diagnostic_code);
        Report("live configured update manifest downloads and verifies", passed);
        return passed ? 0 : 1;
    }
    if (argc == 3) {
        std::ifstream input(argv[1], std::ios::binary);
        const std::string document((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
        const auto result = ValidateUpdateManifest(
            document, argv[2], "0.0.0", std::numeric_limits<uint32_t>::max());
        const bool valid = input.good() || input.eof();
        const bool passed = valid && result.error == UpdateError::None &&
                            result.update_available;
        Report("external signed manifest validates", passed);
        return passed ? 0 : 1;
    }
    bool passed = true;

    UpdateManifest parsed;
    passed &= Report("signed manifest schema parses",
        ParseUpdateManifest(kManifest, parsed) && parsed.schema == 1 &&
        parsed.version == "9.8.7" && parsed.minimum_windows_build == 19045);
    passed &= Report("canonical payload is stable",
        CanonicalUpdatePayload(parsed) ==
            "schema=1\nversion=9.8.7\nminimum_windows_build=19045\n"
            "download_page=https://updates.example.test/pulse\n"
            "installer_sha256="
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n");

    auto result = ValidateUpdateManifest(kManifest, kPublicKey, "1.0.0", 22631);
    passed &= Report("valid newer signed release is offered",
        result.error == UpdateError::None && result.update_available &&
        result.version == L"9.8.7" &&
        result.download_page == L"https://updates.example.test/pulse");

    result = ValidateUpdateManifest(kManifest, kPublicKey, "9.8.7", 22631);
    passed &= Report("equal signed version is up to date",
        result.error == UpdateError::None && !result.update_available &&
        result.download_page.empty());

    result = ValidateUpdateManifest(kManifest, kPublicKey, "10.0.0", 22631);
    passed &= Report("signed downgrade is not offered",
        result.error == UpdateError::None && !result.update_available);

    result = ValidateUpdateManifest(kManifest, kPublicKey, "1.0.0", 17763);
    passed &= Report("unsupported Windows build has no download link",
        result.error == UpdateError::UnsupportedWindows && !result.update_available &&
        result.download_page.empty());

    std::string tampered = kManifest;
    tampered.replace(tampered.find("9.8.7"), 5, "9.8.8");
    result = ValidateUpdateManifest(tampered, kPublicKey, "1.0.0", 22631);
    passed &= Report("tampered manifest signature is rejected",
        result.error == UpdateError::InvalidSignature && !result.update_available);

    std::string insecure = kManifest;
    insecure.replace(insecure.find("https://"), 8, "http://");
    result = ValidateUpdateManifest(insecure, kPublicKey, "1.0.0", 22631);
    passed &= Report("HTTP download page is rejected before display",
        result.error == UpdateError::InsecureUrl && !result.update_available);

    std::string user_info = kManifest;
    user_info.replace(user_info.find("updates.example.test"), 20,
                      "trusted.example@evil");
    result = ValidateUpdateManifest(user_info, kPublicKey, "1.0.0", 22631);
    passed &= Report("download URL user info is rejected before display",
        result.error == UpdateError::InsecureUrl && !result.update_available);

    std::string duplicate = kManifest;
    duplicate.insert(duplicate.find("\n  \"version\""), "\n  \"schema\": 1,");
    passed &= Report("duplicate signed fields are rejected",
        !ParseUpdateManifest(duplicate, parsed));

    std::string unknown = kManifest;
    unknown.insert(unknown.find("\n  \"version\""), "\n  \"channel\": \"stable\",");
    passed &= Report("unknown schema fields are rejected",
        !ParseUpdateManifest(unknown, parsed));

    std::printf("\n== update checker tests: %s ==\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
