#include "preview_host_client.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
bool Write(const std::filesystem::path& path, const std::string& data) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    stream.close();
    return !stream.fail();
}
std::string Zip() {
    const std::string name = "entry.txt";
    const auto put = [](std::string& data, size_t offset, uint32_t value, size_t count) {
        for (size_t i = 0; i < count; ++i) data[offset + i] = static_cast<char>(value >> (8 * i));
    };
    std::string local(30, '\0'), central(46, '\0'), end(22, '\0');
    put(local, 0, 0x04034b50, 4);
    put(local, 4, 20, 2);
    put(local, 26, static_cast<uint32_t>(name.size()), 2);
    put(central, 0, 0x02014b50, 4);
    put(central, 6, 20, 2);
    put(central, 28, static_cast<uint32_t>(name.size()), 2);
    put(end, 0, 0x06054b50, 4);
    put(end, 8, 1, 2);
    put(end, 10, 1, 2);
    put(end, 12, static_cast<uint32_t>(central.size() + name.size()), 4);
    put(end, 16, static_cast<uint32_t>(local.size() + name.size()), 4);
    return local + name + central + name + end;
}
}

int wmain() {
    namespace fs = std::filesystem;
    namespace ipc = pulse::ipc;
    const fs::path parent = fs::current_path() / L"bench_data";
    fs::create_directories(parent);
    const fs::path directory = parent / (L"archive-plugin-required-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!fs::create_directory(directory)) return 1;
    pulse_test::Host host;
    const bool started = host.Start();
    Check(started, "start real preview host");
    if (started) {
        for (const auto* extension : {L".7z", L".rar"}) {
            const auto path = directory / (std::wstring(L"sample") + extension);
            const std::string bytes = extension[1] == L'7'
                ? std::string("7z\xbc\xaf\x27\x1c", 6) : std::string("Rar!\x1a\x07\0", 7);
            Check(Write(path, bytes), "write isolated archive fixture");
            for (const uint32_t flags : {ipc::kPreviewRequestFlagRichText, ipc::kPreviewRequestFlagGrid}) {
                pulse_test::Result result;
                const bool received = host.Request(path.wstring(), result, MAXDWORD, 256,
                    ipc::PreviewRequestKind::Content, flags);
                const bool passed = received && result.error == L"archive-plugin-required" &&
                    result.response.kind != ipc::PreviewContentKind::Hex && result.text.empty();
                Check(passed, "7z/RAR requires plugin without hex fallback (pane/grid)");
                if (!passed) std::wprintf(L"  extension=%ls flags=%u error=%ls kind=%u\n",
                    extension, flags, result.error.c_str(), static_cast<unsigned>(result.response.kind));
            }
        }
        const auto zip = directory / L"native.zip";
        Check(Write(zip, Zip()), "write isolated ZIP fixture");
        pulse_test::Result result;
        Check(host.Request(zip.wstring(), result) && result.response.kind == ipc::PreviewContentKind::Archive &&
            result.text.find(L"entry.txt") != std::wstring::npos, "ZIP retains built-in archive listing");
    }
    host.Stop();
    std::error_code error;
    for (const auto* name : {L"sample.7z", L"sample.rar", L"native.zip"}) fs::remove(directory / name, error);
    Check(fs::remove(directory, error) && !error, "clean isolated fixtures");
    return failures == 0 ? 0 : 1;
}
