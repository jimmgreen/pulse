#include "../common/picker_pulse_open.h"
#include "../app/external_open.h"
#include <cstdio>
#include <fstream>

int wmain(int argc, wchar_t** argv) {
    namespace open = pulse::picker_pulse_open;
    const auto directory = std::filesystem::path(argv[0]).parent_path();
    const auto captured = directory / L"captured-intent.txt";
    if (argc == 2) {
        const auto request = open::Parse(argv[1]);
        if (!request) return 2;
        std::ofstream out(captured, std::ios::binary);
        const std::wstring intent(argv[1]);
        out.write(reinterpret_cast<const char*>(intent.data()), static_cast<std::streamsize>(intent.size() * sizeof(wchar_t)));
        return out ? 0 : 3;
    }
    int failures = 0;
    const auto check = [&](bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    for (const auto* path : {L"C:\\", L"C:\\space name\\file.txt", L"\\\\server\\share\\folder", L"\\\\?\\C:\\long\\folder", L"\\\\?\\UNC\\server\\share\\file.txt"}) {
        for (const bool file : {false, true}) {
            const auto intent = open::Intent(path, file);
            const auto parsed = open::Parse(intent);
            check(parsed && parsed->file == file && !parsed->path.empty(), "filesystem intent roundtrip");
        }
    }
    for (const auto* path : {L"relative", L"C:\\..\\file", L"C:\\bad\"name", L"pulse:extract:C:\\x.zip", L"\\\\server", L"C:\\file:stream"})
        check(open::Intent(path, false).empty(), "invalid/non-filesystem target rejected");
    check(!open::Parse(L"pulse:picker-file:"), "empty reveal rejected");
    check(open::Intent(std::wstring(32768, L'x'), false).empty(), "oversize intent rejected");
    check(open::Executable(L"C:\\Pulse\\PulsePickerUiProof.dll") == L"C:\\Pulse\\pulse.exe", "module sibling executable");
    check(open::Executable(L"C:\\Pulse\\DialogBridge\\hash\\PulsePickerUiProof.dll") == L"C:\\Pulse\\pulse.exe", "thin bundle stable executable");
    check(open::Executable(L"C:\\Pulse\\AppRuntime\\hash\\pulse.exe") == L"C:\\Pulse\\pulse.exe", "versioned app stable executable");
    check(open::Executable(L"relative.dll").empty(), "relative module rejected");
    pulse::app::ExternalOpenRequest request;
    request.can_dedupe = false;
    pulse::app::ExternalOpenWindow window;
    window.shows_folder = true;
    const auto decision = pulse::app::DecideOpenTarget(request, {window}, {}, pulse::app::ExternalOpenPolicy::AlwaysTab);
    check(decision.action == pulse::app::ExternalOpenAction::Tab, "explicit picker intent creates tab even when folder already open");
    window.busy = true;
    check(pulse::app::DecideOpenTarget(request, {window}, {}, pulse::app::ExternalOpenPolicy::AlwaysTab).action == pulse::app::ExternalOpenAction::Deferred,
        "busy window stays deferred");
    // The sibling pulse.exe here is a copy of THIS probe, not the user's app.
    for (const bool file : {false, true}) {
        std::error_code error;
        std::filesystem::remove(captured, error);
        check(open::Launch(L"C:\\picker fixture\\sample.txt", file), "production worker started");
        std::wstring received;
        for (int i = 0; i < 100 && received.empty(); ++i) {
            Sleep(20);
            std::ifstream in(captured, std::ios::binary | std::ios::ate);
            if (!in || in.tellg() <= 0) continue;
            const size_t bytes = static_cast<size_t>(in.tellg());
            received.resize(bytes / sizeof(wchar_t)); in.seekg(0);
            in.read(reinterpret_cast<char*>(received.data()), static_cast<std::streamsize>(bytes));
        }
        check(received == open::Intent(L"C:\\picker fixture\\sample.txt", file), "production CreateProcess preserves exact quoted intent");
    }
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}
