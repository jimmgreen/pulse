#include "preview_host_client.h"
#include "../common/preview_packs.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { puts("Usage: archive_pack_host_test <7z.exe> <fixture.wim>"); return 2; }
    namespace fs = std::filesystem;
    using namespace pulse;
    const std::wstring fixture = fs::absolute(argv[2]).wstring();
    const fs::path root = fs::current_path() / L"bench_data" / (L"archive-host-" + std::to_wstring(GetCurrentProcessId()));
    wchar_t previous[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", previous, 32768);
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    const fs::path pack = fs::path(packs::PacksRoot()) / L"archive" / L"test";
    fs::create_directories(pack);
    const fs::path tool(argv[1]);
    fs::copy_file(tool, pack / L"7z.exe");
    fs::copy_file(tool.parent_path() / L"7z.dll", pack / L"7z.dll");
    std::ofstream(pack.parent_path() / L"installed.json") << "{\"dir\":\"test\",\"version\":\"test\"}";
    int failed = 0;
    auto check = [&](bool ok, const char* name) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failed += !ok; };
    {
        pulse_test::Host host;
        check(host.Start(), "preview host starts");
        pulse_test::Result result;
        check(host.Request(fixture, result) && result.response.kind == ipc::PreviewContentKind::Archive &&
              result.text.find(L"hello.txt") != std::wstring::npos &&
              result.text.find(L"7-Zip") != std::wstring::npos, "WIM listing through archive pack IPC");
        std::wprintf(L"status=%lu kind=%u error=%s text=%.400s\n", result.response.status, static_cast<unsigned>(result.response.kind), result.error.c_str(), result.text.c_str());
        result = {};
        const bool offline_read = host.Request(fixture, result, FILE_ATTRIBUTE_OFFLINE | 0x00400000);
        check(offline_read && result.text.find(L"7-Zip") == std::wstring::npos &&
              result.error == L"offline-placeholder", "recall-on-access placeholder never invokes archive pack");
        result = {};
        check(host.Request(fixture, result, MAXDWORD, 128, ipc::PreviewRequestKind::Content,
                           ipc::kPreviewRequestFlagGrid) && result.text.find(L"7-Zip") == std::wstring::npos,
              "grid thumbnails skip archive pack listing");
    }
    packs::PackSettings settings;
    settings.enabled[static_cast<size_t>(packs::PackId::Archives)] = false;
    check(packs::SavePackSettings(settings), "disable isolated archive pack");
    {
        pulse_test::Host host;
        check(host.Start(), "disabled-pack host starts");
        pulse_test::Result result;
        host.Request(fixture, result);
        check(result.text.find(L"7-Zip") == std::wstring::npos, "disabled pack is not selected");
    }
    SetEnvironmentVariableW(L"LOCALAPPDATA", previous[0] ? previous : nullptr);
    std::error_code ec; fs::remove_all(root, ec);
    check(!ec, "isolated host fixtures cleaned");
    return failed ? 1 : 0;
}
