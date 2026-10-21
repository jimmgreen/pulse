#include "preview_host_client.h"
#include "../common/preview_packs.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { puts("Usage: raw_pack_host_test <pack-directory> <fixture-directory>"); return 2; }
    namespace fs = std::filesystem;
    using namespace pulse;
    const fs::path root = fs::current_path() / L"bench_data" / (L"raw-host-" + std::to_wstring(GetCurrentProcessId()));
    wchar_t previous[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", previous, 32768);
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    const fs::path pack = fs::path(packs::PacksRoot()) / L"raw" / L"test";
    const fs::path fixtures = fs::absolute(argv[2]);
    fs::create_directories(pack);
    for (const auto& file : fs::directory_iterator(argv[1])) {
        if (file.is_regular_file() && (file.path().extension() == L".exe" || file.path().extension() == L".dll"))
            fs::copy_file(file.path(), pack / file.path().filename());
    }
    std::ofstream(pack.parent_path() / L"installed.json") << "{\"dir\":\"test\",\"version\":\"test\"}";
    int failed = 0;
    auto check = [&](bool ok, const char* name) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failed += !ok; };
    {
        pulse_test::Host host;
        check(host.Start(), "RAW preview host starts");
        pulse_test::Result result;
        check(host.Request((fixtures / L"synthetic-embedded.dng").wstring(), result, MAXDWORD, 128,
                           ipc::PreviewRequestKind::Content, ipc::kPreviewRequestFlagGrid) &&
              result.response.kind == ipc::PreviewContentKind::Bitmap && !result.pixels.empty(),
              "embedded RAW grid preview through IPC");
        result = {};
        check(host.Request((fixtures / L"synthetic-no-preview.dng").wstring(), result, MAXDWORD, 512,
                           ipc::PreviewRequestKind::Content, ipc::kPreviewRequestFlagRichText) &&
              result.response.kind == ipc::PreviewContentKind::Bitmap && !result.pixels.empty() &&
              result.response.width <= 512 && result.response.height <= 512,
              "explicit RAW preview without embedded image through IPC");
        result = {};
        check(host.Request((fixtures / L"synthetic-embedded.dng").wstring(), result,
                           FILE_ATTRIBUTE_OFFLINE | 0x00400000) && result.error == L"offline-placeholder",
              "RAW recall placeholder remains unread");
    }
    SetEnvironmentVariableW(L"LOCALAPPDATA", previous[0] ? previous : nullptr);
    std::error_code error;
    fs::remove_all(root, error);
    check(!error, "isolated RAW host fixtures cleaned");
    return failed ? 1 : 0;
}
