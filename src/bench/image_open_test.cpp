#include "../ops/image_open_dispatch.h"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace pulse::ops;
namespace {
unsigned neighbor_calls = 0, shell_calls = 0;
bool success = true;
HRESULT neighbor_error = E_FAIL;
std::wstring shell_file;
ImageNeighborLaunchResult Neighbor(const std::wstring&, HWND) noexcept {
    ++neighbor_calls;
    return {success, success ? S_OK : neighbor_error, ImageNeighborLaunchStage::Complete};
}
BOOL WINAPI Shell(SHELLEXECUTEINFOW* info) {
    ++shell_calls; shell_file = info->lpFile;
    return TRUE;
}
}
int main() {
    unsigned failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; failures += !ok;
    };
    ShellCommandApi api; api.shell_execute = Shell;
    auto run = [&](const wchar_t* file, const wchar_t* verb = L"", const wchar_t* args = L"", bool explicit_app = false) {
        neighbor_calls = shell_calls = 0;
        return LaunchImageOrShellItem(file, verb, args, L"C:\\fixture", nullptr, explicit_app, Neighbor, api);
    };
    auto result = run(L"C:\\fixture\\photo.JPG");
    check(result.neighbor_launched && neighbor_calls == 1 && shell_calls == 0, "image default open uses neighbor launch exactly once");
    success = false;
    result = run(L"C:\\fixture\\photo.png");
    check(neighbor_calls == 1 && shell_calls == 1 && shell_file == L"C:\\fixture\\photo.png" && !result.shell.error,
        "failed neighbor launch falls back once for only the selected file");
    neighbor_error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    result = run(L"C:\\fixture\\photo.png");
    check(shell_calls == 0 && result.shell.error == ERROR_CANCELLED, "user cancellation never relaunches file");
    success = true;
    for (const auto* verb : {L"edit", L"print", L"openas", L"runas", L"open"}) {
        run(L"C:\\fixture\\photo.png", verb);
        check(neighbor_calls == 0 && shell_calls == 1, "explicit shell verb remains unchanged");
    }
    run(L"C:\\fixture\\photo.png", L"", L"", true);
    check(neighbor_calls == 0 && shell_calls == 1, "explicit application bypasses neighbor launch");
    run(L"C:\\fixture\\photo.png", L"", L"custom");
    check(neighbor_calls == 0 && shell_calls == 1, "custom arguments remain on shell path");
    for (const auto* file : {L"C:\\fixture\\document.txt", L"C:\\fixture\\photo.lnk", L"C:\\folder.jpg\\file", L"C:\\a.png:stream"}) {
        run(file); check(neighbor_calls == 0 && shell_calls == 1, "non-image and alternate streams bypass neighbor launch");
    }
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"image-open-" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root);
    const auto file = root / L"image.png";
    { std::ofstream out(file); out << "query fixture, no image decoding"; }
    HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse image query test", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const auto prepared = PrepareImageNeighborLaunch(file.wstring(), owner);
    std::cout << "prepare_hr=" << std::hex << static_cast<unsigned>(prepared.error) << std::dec
        << " stage=" << static_cast<unsigned>(prepared.stage) << '\n';
    check(SUCCEEDED(prepared.error) && prepared.query_prepared && !prepared.launched,
        "real WinRT preparation passes neighbor query without launching an app");
    check(prepared.shallow && prepared.ordered_by_name && prepared.filter_count > 0,
        "neighbor query is shallow, image-filtered and ordered by name");
    const auto missing = PrepareImageNeighborLaunch((root / L"missing.png").wstring(), owner);
    check(FAILED(missing.error) && !missing.launched, "missing file fails before launch");
    const auto no_owner = PrepareImageNeighborLaunch(file.wstring(), nullptr);
    check(FAILED(no_owner.error) && !no_owner.launched, "unavailable owner safely rejects WinRT launch");
    if (owner) DestroyWindow(owner);
    std::filesystem::remove(file); std::filesystem::remove(root);
    return failures ? 1 : 0;
}
