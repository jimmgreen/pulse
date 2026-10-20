#include "../preview_host/raw_pack.h"
#include "../common/image_pack_protocol.h"
#include "../common/preview_packs.h"
#include "../common/ffmpeg_tool.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <fcntl.h>
#include <io.h>

namespace fs = std::filesystem;
using namespace pulse;
int wmain(int argc, wchar_t** argv) {
    if (argc >= 3) {
        const std::wstring mode(argv[1]), file(argv[2]);
        if (mode == L"probe") { puts("format=Camera RAW\nwidth=64\nheight=32"); return 0; }
        if (file == L"hang") { Sleep(10000); return 0; }
        if (mode != (file == L"full" ? L"decode-full" : L"decode")) return 3;
        imgpack::FrameHeader header;
        header.width = 2; header.height = 1; header.stride = 8;
        header.source_width = 64; header.source_height = 32;
        header.flags = mode == L"decode" ? imgpack::kFlagEmbeddedThumbnail : 0;
        if (file == L"oversize") header.width = 9000;
        if (file == L"badmagic") header.magic = 0;
        _setmode(_fileno(stdout), _O_BINARY);
        fwrite(&header, sizeof(header), 1, stdout);
        const unsigned char px[8] = {0,0,255,255,0,255,0,255};
        fwrite(px, 1, file == L"truncated" ? 4 : 8, stdout);
        if (file == L"extra") fwrite(px, 1, 1, stdout);
        return file == L"failure" ? 3 : 0;
    }
    int failed = 0;
    auto check = [&](bool ok, const char* text) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", text); failed += !ok; };
    const fs::path root = fs::current_path() / L"bench_data" / (L"raw-pack-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root);
    wchar_t previous[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", previous, 32768);
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    check(!preview::RawPackAvailable(), "missing pack is unavailable");
    const fs::path pack = fs::path(packs::PacksRoot()) / L"raw" / L"test";
    fs::create_directories(pack);
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
    fs::copy_file(self, pack / L"pulse-rawpack.exe", fs::copy_options::overwrite_existing);
    std::ofstream(pack.parent_path() / L"installed.json") << "{\"dir\":\"test\",\"version\":\"test\"}";
    check(preview::RawPackAvailable(), "offline installed pack available");
    {
        ffmpeg::Process process;
        ffmpeg::LaunchOptions options;
        options.discard_stderr = true;
        const bool started = process.Start((pack / L"pulse-rawpack.exe").wstring(), L"decode embedded 32", options);
        printf("diagnostic start=%d error=%lu\n", started, GetLastError());
        if (started) {
            unsigned char bytes[64]{};
            const DWORD got = process.ReadOut(bytes, sizeof(bytes));
            const bool done = process.Wait(5000);
            printf("diagnostic bytes=%lu exit=%lu done=%d\n", got, process.ExitCode(), done);
            if (!done) process.Terminate();
        }
    }
    preview::ImagePackFrame frame;
    check(preview::RawPackDecode(L"embedded", 32, false, frame) && frame.flags == imgpack::kFlagEmbeddedThumbnail,
          "automatic request uses embedded-only command");
    check(preview::RawPackDecode(L"full", 32, true, frame) && frame.flags == 0,
          "explicit view uses full-decode command");
    for (const wchar_t* bad : {L"oversize",L"badmagic",L"truncated",L"extra",L"failure"})
        check(!preview::RawPackDecode(bad, 32, false, frame) && frame.pixels.empty(), "malformed or failed frame rejected and cleared");
    const ULONGLONG started = GetTickCount64();
    check(!preview::RawPackDecode(L"hang", 32, false, frame) && GetTickCount64() - started >= 2500 && GetTickCount64() - started < 6000, "timeout kills stalled decoder");
    std::vector<preview::PreviewPropertyValue> rows;
    check(preview::RawPackProperties(L"probe", rows, 2) && rows.size() == 2, "bounded properties parsed");
    auto settings = packs::LoadPackSettings(); settings.enabled[static_cast<unsigned>(packs::PackId::Raw)] = false;
    check(packs::SavePackSettings(settings) && !preview::RawPackAvailable(), "disabled pack unavailable");
    check(!preview::RawPackDecode(L"embedded", 32, false, frame), "disabled pack never launches");
    SetEnvironmentVariableW(L"LOCALAPPDATA", *previous ? previous : nullptr);
    std::error_code error; fs::remove_all(root, error);
    check(!error, "isolated fixtures cleaned");
    return failed ? 1 : 0;
}

