#include "../common/explorer_launch_request.h"
#include <cstdio>
int main() {
    using namespace pulse::explorer_launch;
    unsigned failed = 0, count = 0;
    auto check = [&](bool ok, const char* name) { ++count; std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failed; };
    const auto exe = SystemExplorer();
    auto resolve = [&](std::wstring operand, Request& request, DWORD flags = 0) {
        const auto command = Quote(exe) + L" " + Quote(operand);
        return Resolve(exe.c_str(), command.c_str(), flags, request);
    };
    Request request;
    check(resolve(L"file:///C:/fixture/a%20b%2Cc%3Dd", request) && request.path == L"C:\\fixture\\a b,c=d" && !request.select, "file URL decodes spaces comma equals");
    check(resolve(L"file:///C:/fixture/中文", request) && request.path == L"C:\\fixture\\中文", "DeepSeek decoded Unicode URL");
    check(resolve(L"file:///C:/fixture/%E4%B8%AD%E6%96%87", request) && request.path == L"C:\\fixture\\中文", "UTF-8 percent-encoded Unicode URL");
    check(resolve(L"file:///C:/fixture/100%25", request) && request.path == L"C:\\fixture\\100%", "percent decoded exactly once");
    check(!resolve(L"file:///C:/fixture/%00.txt", request), "encoded NUL remains native");
    check(!resolve(L"file:///C:/fixture/%FF", request), "invalid UTF-8 remains native");
    check(!resolve(L"file:///C:/fixture/%Z0", request), "malformed percent escape remains native");
    check(resolve(L"/select,file:///C:/fixture/a%20b.txt", request) && request.select && request.path == L"C:\\fixture\\a b.txt", "selection intent retained");
    check(resolve(L"C:\\fixture\\folder\\", request) && request.path.back() == L'\\', "trailing backslash quoting");
    check(!resolve(L"shell:Downloads", request), "virtual folder remains native");
    check(!resolve(L"file://server/share/x", request), "UNC URL remains native");
    check(!resolve(L"\\\\server\\share", request), "UNC remains native");
    check(!resolve(L"C:\\x,/root,C:\\", request), "compound switches remain native");
    check(!resolve(L"C:\\x", request, CREATE_SUSPENDED), "caller suspension remains native");
    check(!resolve(L"C:\\x", request, DEBUG_PROCESS), "debug process remains native");
    check(resolve(L"C:\\x", request, CREATE_UNICODE_ENVIRONMENT), "observed Node flags admitted");
    check(!resolve(L"C:\\x", request, EXTENDED_STARTUPINFO_PRESENT), "extended attribute contract remains native");
    check(!resolve(L"C:\\x", request, CREATE_NEW_PROCESS_GROUP), "new process group remains native");
    check(resolve(L"C:\\x", request, CREATE_NO_WINDOW), "GUI bridge accepts Electron no-console flag");
    check(resolve(L"file:///C:/fixture/中文", request, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT), "actual Electron 0x08000400 flags preserve folder intent");
    check(!resolve(L"C:\\x", request, CREATE_NO_WINDOW | CREATE_SUSPENDED), "Electron no-console flag does not admit suspended launch");
    check(!Resolve(L"C:\\fixture\\explorer.exe", L"explorer.exe C:\\x", 0, request), "same basename impostor remains native");
    check(!Resolve(L"explorer.exe", L"explorer.exe C:\\x", 0, request), "unresolved basename remains native");
    check(!Resolve(exe.c_str(), (Quote(exe) + L" C:\\x C:\\y").c_str(), 0, request), "multiple operands remain native");
    check(!resolve(L"file:///C:/x?query", request), "URL query remains native");
    check(!resolve(L"C:\\x:stream", request), "ADS remains native");
    using pulse::browse::Intent;
    using pulse::browse::Match;
    pulse::browse::Request browse_request;
    auto classify = [&](std::wstring operands, const wchar_t* cwd = nullptr, DWORD flags = 0) {
        const auto command = Quote(exe) + (operands.empty() ? L"" : L" " + operands);
        return Classify(exe.c_str(), command.c_str(), flags, cwd, browse_request);
    };
    check(classify(L"/select, \"C:\\fixture\\中文 file.txt\"") == Match::Supported &&
        browse_request.intent == Intent::Reveal && browse_request.path == L"C:\\fixture\\中文 file.txt",
        "DeepSeek separate /select, argument retains reveal intent");
    check(classify(L"/select,\"C:\\fixture\\中文 file.txt\"") == Match::Supported &&
        browse_request.path == L"C:\\fixture\\中文 file.txt", "combined /select, quoted path");
    check(classify(L"/SELECT, file:///C:/fixture/a%20b.txt") == Match::Supported &&
        browse_request.intent == Intent::Reveal && browse_request.path == L"C:\\fixture\\a b.txt",
        "separate case-insensitive select supports local file URL");
    check(classify(L"\"C:\\fixture\\a,b\"") == Match::Supported &&
        browse_request.intent == Intent::OpenFolder && browse_request.path == L"C:\\fixture\\a,b",
        "independently quoted folder operand admits literal comma");
    check(classify(L"/select, \"C:\\fixture\\a,b.txt\"") == Match::Supported &&
        browse_request.path == L"C:\\fixture\\a,b.txt", "separate quoted selection admits literal comma");
    check(classify(L"/select,\"C:\\fixture\\a,b.txt\"") == Match::Supported,
        "quoted operand after combined select prefix admits literal comma");
    check(classify(L"C:\\fixture\\a,b") == Match::Unsupported &&
        classify(L"/select,C:\\fixture\\a,b.txt") == Match::Unsupported &&
        classify(L"\"/select,C:\\fixture\\a,b.txt\"") == Match::Unsupported,
        "unquoted or whole-switch quoted comma grammar stays unsupported");
    check(classify(L".\\child\\..\\folder", L"C:\\fixture") == Match::Supported &&
        browse_request.path == L"C:\\fixture\\folder", "relative folder resolves against explicit cwd only");
    check(classify(L"/select, \"..\\a b.txt\"", L"C:\\fixture\\child") == Match::Supported &&
        browse_request.path == L"C:\\fixture\\a b.txt" && browse_request.intent == Intent::Reveal,
        "relative selected item resolves against explicit cwd");
    check(classify(L"\\folder", L"D:\\fixture") == Match::Supported &&
        browse_request.path == L"D:\\folder", "root-relative path uses explicit cwd drive");
    check(classify(L"relative") == Match::Unsupported &&
        classify(L"relative", L"also-relative") == Match::Unsupported &&
        classify(L"relative", L"\\\\server\\share") == Match::Unsupported &&
        classify(L"C:relative", L"C:\\fixture") == Match::Unsupported,
        "missing UNC or ambiguous drive-relative cwd never uses process cwd");
    check(classify(L"\\\\server\\share") == Match::Unsupported &&
        classify(L"file://server/share") == Match::Unsupported &&
        classify(L"/select, \\\\server\\share\\item") == Match::Unsupported,
        "recognized UNC browsing is unsupported rather than unrelated");
    check(classify(L"shell:Downloads") == Match::Unsupported &&
        classify(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}") == Match::Unsupported,
        "recognized virtual folders are unsupported");
    check(classify(L"/root,C:\\fixture") == Match::Unsupported &&
        classify(L"/select,") == Match::Unsupported &&
        classify(L"/select, C:\\fixture extra") == Match::Unsupported &&
        classify(L"-embedding", L"C:\\fixture") == Match::Unsupported,
        "unknown complex or incomplete Explorer switches are unsupported");
    check(classify(L"C:\\fixture", nullptr, CREATE_SUSPENDED) == Match::Unsupported &&
        classify(L"C:\\fixture", nullptr, DEBUG_PROCESS) == Match::Unsupported,
        "recognized unsupported creation contracts are not unrelated");
    check(classify(L"", nullptr, CREATE_SUSPENDED) == Match::Unrelated &&
        Classify(exe.c_str(), nullptr, 0, nullptr, browse_request) == Match::Unrelated,
        "no-operand Explorer retains desktop startup route");
    const auto full_command = Quote(exe) + L" C:\\fixture";
    check(Classify(nullptr, full_command.c_str(), 0, nullptr, browse_request) == Match::Supported,
        "absolute system image in command line establishes identity");
    check(Classify(L"explorer.exe", full_command.c_str(), 0, nullptr, browse_request) == Match::Unrelated &&
        Classify(L"C:\\fixture\\explorer.exe", full_command.c_str(), 0, nullptr, browse_request) == Match::Unrelated &&
        Classify(nullptr, L"explorer.exe C:\\fixture", 0, L"C:\\Windows", browse_request) == Match::Unrelated,
        "basename and impostor never inherit identity from arguments or cwd");
    browse_request = {Intent::Reveal, L"stale"};
    check(classify(L"\"C:\\unterminated") == Match::Unsupported && browse_request.path.empty() &&
        browse_request.intent == Intent::OpenFolder, "malformed request clears prior parsed result");
    std::printf("RESULT tests=%u failed=%u\n", count, failed);
    return failed ? 1 : 0;
}
