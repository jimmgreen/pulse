#include "app/hang_watch.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
std::string Read(const std::wstring& path) {
    std::ifstream file{std::filesystem::path(path)};
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0, pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) { ++count; pos += needle.size(); }
    return count;
}
}

int wmain() {
    using namespace pulse::app::hang;
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path = std::wstring(temp) + L"pulse_frame_watch_" +
        std::to_wstring(GetCurrentProcessId()) + L".log";
    DeleteFileW(path.c_str());
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    check(!StartFrameWatch(std::wstring(temp)) && BeginFrame() == 0, "invalid log target reports unavailable");
    check(StartFrameWatch(path), "monitor starts with writable log");
    for (int i = 0; i < 20; ++i) {
        const auto id = BeginFrame();
        Sleep(2);
        EndFrame(id);
    }
    Sleep(30);
    check(Read(path).empty(), "short frames do not sample");
    const auto slow = BeginFrame();
    check(slow != 0 && BeginFrame() == 0, "nested frame preserves outer identity");
    Sleep(120);
    EndFrame(slow);
    Sleep(30);
    const auto text = Read(path);
    check(text.find("frame_id=" + std::to_string(slow) + " ") != std::string::npos &&
        text.find("status=captured") != std::string::npos && text.find("+0x") != std::string::npos,
        "120ms stall captures correlated module RVA stack");
    check(Count(text, "frame_id=") >= 1 && Count(text, "frame_id=") <= 3,
        "per frame sampling bounded at three");
    const auto second = BeginFrame();
    Sleep(120);
    EndFrame(second);
    Sleep(30);
    check(Count(Read(path), "frame_id=") <= 3, "rolling second sampling bounded at three");
    const auto before_stop = GetTickCount64();
    StopFrameWatch();
    check(GetTickCount64() - before_stop < 500 && BeginFrame() == 0, "stop joins promptly and disables sampling");
    StopFrameWatch();
    StartFrameWatch(path);
    const auto restart = BeginFrame();
    StopFrameWatch();
    EndFrame(restart);
    check(restart > second, "restart preserves unique frame IDs");
    DeleteFileW(path.c_str());
    return failures ? 1 : 0;
}
