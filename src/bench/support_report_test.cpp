#include "../common/diagnostics_exporter.h"
#include "../common/diagnostics_report.h"
#include "../common/runtime_log.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
namespace fs = std::filesystem;
std::string Read(const fs::path& path) { std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
void Write(const fs::path& path, const std::string& s) { std::ofstream f(path, std::ios::binary); f << s; }
int main() {
    using namespace pulse::diagnostics;
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl; failures += !ok; };
    const auto root = fs::absolute("bench_data") / ("support-report-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    fs::create_directories(root / "source" / "Diagnostics" / "Runtime"); fs::create_directories(root / "out"); fs::create_directories(root / "bin");
    uint64_t rejected = 0;
    const auto safe = SanitizeRuntime("{\"event\":\"failure\",\"severity\":2,\"session\":18446744073709551615,\"pid\":42,\"path\":\"PRIVATE\",\"data\":{\"error\":5,\"path\":\"PRIVATE\",\"nested\":{\"name\":\"PRIVATE\"},\"operation\":91}}\ninvalid\n", rejected);
    check(rejected == 1 && safe.find("PRIVATE") == safe.npos && safe.find("\"operation\":91") != safe.npos && safe.find("18446744073709551615") != safe.npos, "sanitize strings and malformed records while preserving exact numeric correlation");
    check(runtime::Initialize((root / "source").wstring(), "test"), "initialize isolated logger");
    runtime::Event("index_write_failed", {{"operation", 7}, {"error", 5}}, runtime::Level::Error);
    Write(root / "source" / "pulse_crash.log", "PRIVATE-LEGACY");
    fs::create_directories(root / "source" / "Diagnostics" / "Crashes");
    Write(root / "source" / "Diagnostics" / "Crashes" / "secret.dmp", "PRIVATE-DUMP");
    Write(root / "bin" / "pulse.exe", "abc");
    Write(root / "config.json", "{\"version\":1,\"generation\":22,\"index_path\":\"D:\\\\PRIVATE\",\"excluded_paths\":[\"PRIVATE\"]}");
    ExportOptions options; options.source_root = (root / "source").wstring(); options.destination = (root / "out").wstring();
    options.support_report = true; options.include_dumps = false; options.install_directory = (root / "bin").wstring(); options.configuration_file = (root / "config.json").wstring();
    std::wstring error;
    check(Export(options, &error), "ordinary feedback export succeeds without system probes");
    const auto report = Read(root / "out" / "support-report.json");
    check(report.find("index_write_failed") != report.npos && report.find("\"flush_completed\":true") != report.npos, "export immediately after failure flushes and includes failure evidence");
    check(report.find("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != report.npos, "report identifies actual executable bytes with SHA256");
    check(report.find("PRIVATE") == report.npos && report.find("\"generation\":22") != report.npos, "configuration summary omits paths while retaining generation");
    check(!fs::exists(root / "out" / "Crashes") && !fs::exists(root / "out" / "pulse_crash.log"), "ordinary feedback excludes dumps and legacy logs");
    check(CreateSupportArchive((root / "out").wstring(), (root / "out" / "Pulse-diagnostics.zip").wstring(), &error), "create feedback ZIP from generated artifacts");
    check(!CreateSupportArchive((root / "out").wstring(), (root / "out" / "Pulse-diagnostics.zip").wstring(), &error), "never overwrite an existing feedback ZIP");
    fs::create_directories(root / "empty-source"); fs::create_directories(root / "empty-out");
    options.source_root = (root / "empty-source").wstring(); options.destination = (root / "empty-out").wstring();
    check(Export(options, &error) && Read(root / "empty-out" / "support-report.json").find("\"evidence_available\":false") != std::string::npos, "missing runtime logs are reported as unavailable despite successful copying");
    runtime::Shutdown();
    // Retain only this isolated fixture for independent ZIP/JSON validation.
    std::cout << "FIXTURE=" << root.string() << '\n';
    return failures ? 1 : 0;
}
