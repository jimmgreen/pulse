#include "../app/update_checker.h"

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

} // namespace

int main(int argc, char** argv) {
    using namespace pulse::app;
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
