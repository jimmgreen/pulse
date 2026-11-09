#include "../app/media_props.h"
#include <objbase.h>
#include <cstdio>
#include <chrono>
#include <thread>

using namespace pulse::app::media;

bool WaitIdle() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Pending() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return Pending() == 0;
}

int main() {
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failed += !ok;
    };
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wchar_t cwd[MAX_PATH]{};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    const std::wstring image_path = std::wstring(cwd) + L"\\bench_data\\media-props-" +
        std::to_wstring(GetCurrentProcessId()) + L".bmp";
    HANDLE file = CreateFileW(image_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    bool written = file != INVALID_HANDLE_VALUE;
    if (written) {
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + 24;
        BITMAPINFOHEADER info{};
        info.biSize = sizeof(info);
        info.biWidth = 3;
        info.biHeight = 2;
        info.biPlanes = 1;
        info.biBitCount = 24;
        BYTE pixels[24]{};
        DWORD bytes = 0;
        written = WriteFile(file, &header, sizeof(header), &bytes, nullptr) && bytes == sizeof(header);
        written &= WriteFile(file, &info, sizeof(info), &bytes, nullptr) && bytes == sizeof(info);
        written &= WriteFile(file, pixels, sizeof(pixels), &bytes, nullptr) && bytes == sizeof(pixels);
        CloseHandle(file);
    }
    check(written, "create isolated BMP fixture");
    if (written) {
        Job image_job;
        image_job.path = L"\\\\?\\" + image_path;
        image_job.kind = Kind::Photo;
        const auto start = std::chrono::steady_clock::now();
        const Props props = ReadProps(image_job);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        check(props.width == 3 && props.height == 2, "real BMP dimensions through selected property keys and long-path prefix");
        std::printf("BMP property read: %lld ms\n", static_cast<long long>(elapsed));
    }
    if (file != INVALID_HANDLE_VALUE) check(DeleteFileW(image_path.c_str()) != FALSE, "remove BMP fixture");
    CoUninitialize();
    int owner = 0;
    std::vector<Job> jobs;
    for (unsigned i = 0; i < 96; ++i) {
        Job job;
        job.path = L"\\\\?\\C:\\pulse-media-props-test\\" + std::to_wstring(i);
        job.mtime = 1;
        job.size = i;
        job.attrs = FILE_ATTRIBUTE_OFFLINE;
        job.kind = Kind::Photo;
        jobs.push_back(job);
        jobs.push_back(job); // Multiple tabs can request identical versions.
    }
    const auto before = Revision();
    Request(&owner, jobs);
    check(WaitIdle(), "batch completes, including final partial batch");
    const auto updates = Revision() - before;
    check(updates >= 3 && updates < 96, "revision published per batch rather than per file");
    bool all_cached = true;
    for (const auto& job : jobs) {
        const auto props = Find(job.path, job.mtime, job.size);
        all_cached &= props && props->width == 0 && props->taken == 0;
    }
    check(all_cached, "offline metadata cached without opening files");
    Request(&owner, jobs);
    check(WaitIdle() && Revision() == before + updates, "current versions cause no new publications");
    Job changed = jobs.front();
    changed.mtime = 2;
    Request(&owner, {changed});
    check(WaitIdle() && Find(changed.path, 2, changed.size) && !Find(changed.path, 1, changed.size),
          "single-file tail and changed file version published");
    size_t count = 0;
    ForEachInFolder(L"\\\\?\\C:\\pulse-media-props-test\\", [&](auto, auto, auto, const auto&) { ++count; });
    check(count == 96, "folder cache contains every unique file");
    std::printf("publications for 192 requests / 96 unique files: %llu\n",
                static_cast<unsigned long long>(updates));
    return failed ? 1 : 0;
}
