#include "route.h"
#include <cstdio>
int main() {
    using namespace explorer_probe;
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
    check(!resolve(L"C:\\x", request, CREATE_NO_WINDOW), "unverified no-window contract remains native");
    check(!Resolve(L"C:\\fixture\\explorer.exe", L"explorer.exe C:\\x", 0, request), "same basename impostor remains native");
    check(!Resolve(L"explorer.exe", L"explorer.exe C:\\x", 0, request), "unresolved basename remains native");
    check(!Resolve(exe.c_str(), (Quote(exe) + L" C:\\x C:\\y").c_str(), 0, request), "multiple operands remain native");
    check(!resolve(L"file:///C:/x?query", request), "URL query remains native");
    check(!resolve(L"C:\\x:stream", request), "ADS remains native");
    std::printf("RESULT tests=%u failed=%u\n", count, failed);
    return failed ? 1 : 0;
}
