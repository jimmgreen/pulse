#include "../app/media_props.h"
#include "../app/media_matroska.h"
#include <objbase.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failed += !ok;
    };
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    using namespace pulse::app::media;
    Job job;
    job.path = std::filesystem::absolute(argv[1]).wstring();
    job.kind = Kind::Video;
    const auto p = ReadProps(job);
    std::printf("width=%u height=%u fps_milli=%u\n", p.width, p.height, p.fps_milli);
    check(p.width == 320 && p.height == 180 && p.fps_milli == 23976, "real MKV video columns");
    Props direct;
    ReadMatroskaProps(job.path, direct);
    check(direct.width == 320 && direct.height == 180 && direct.fps_milli == 23976, "container without Shell handler");
    Props existing;
    existing.width = 640; existing.height = 360; existing.fps_milli = 30000;
    ReadMatroskaProps(job.path, existing);
    check(existing.width == 640 && existing.height == 360 && existing.fps_milli == 30000, "preserve populated values");
    job.attrs = FILE_ATTRIBUTE_OFFLINE;
    check(ReadProps(job).width == 0, "offline file stays unopened");
    job.attrs = 0;
    job.path = L"\\\\?\\" + job.path;
    check(ReadProps(job).fps_milli == 23976, "extended path");
    std::ifstream input(std::filesystem::path(argv[1]), std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(input)), {});
    const auto truncated = std::filesystem::path(argv[1]).parent_path() /
        (L"truncated-" + std::to_wstring(GetCurrentProcessId()) + L".mkv");
    bool clean = true;
    for (const size_t length : {size_t(0), size_t(1), size_t(4), size_t(40), size_t(100)}) {
        if (length > data.size()) { clean = false; break; }
        {
            std::ofstream output(truncated, std::ios::binary);
            output.write(data.data(), static_cast<std::streamsize>(length));
        }
        Props short_props;
        ReadMatroskaProps(truncated.wstring(), short_props);
        clean &= !short_props.width && !short_props.height && !short_props.fps_milli;
    }
    check(clean, "truncated headers produce unknown metadata");
    std::filesystem::remove(truncated);
    CoUninitialize();
    return failed ? 1 : 0;
}
