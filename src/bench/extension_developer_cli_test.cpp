#include "../app/extensions/developer_cli.h"
#include "../app/extensions/developer_contract.h"
#include "../app/extensions/manifest.h"
#include "../app/extensions/runtime.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pulse::extensions;
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
struct Arguments {
    std::vector<std::wstring> values{L"pulse.exe"};
    std::vector<wchar_t*> pointers;
    Arguments(std::initializer_list<std::wstring> args) {
        values.insert(values.end(), args);
        for (auto& value : values) pointers.push_back(value.data());
    }
    int Count() const { return static_cast<int>(pointers.size()); }
};
auto Evaluate(std::initializer_list<std::wstring> args) {
    Arguments arguments(args);
    return EvaluateDeveloperCli(arguments.Count(), arguments.pointers.data());
}
struct Fixture {
    std::filesystem::path root;
    std::vector<std::filesystem::path> files;
    Fixture() {
        root = std::filesystem::absolute(std::filesystem::path("bench_data") /
            ("extension-cli-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("Cannot create isolated fixture directory");
    }
    std::filesystem::path File(const std::wstring& name, const std::string& contents) {
        const auto path = root / name;
        files.push_back(path);
        std::ofstream output(path, std::ios::binary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!output) throw std::runtime_error("Cannot write fixture");
        return path;
    }
    ~Fixture() {
        std::error_code error;
        for (const auto& file : files) std::filesystem::remove(file, error);
        std::filesystem::remove(root, error);
    }
};
struct StdoutOverride {
    HANDLE original = GetStdHandle(STD_OUTPUT_HANDLE);
    explicit StdoutOverride(HANDLE output) { SetStdHandle(STD_OUTPUT_HANDLE, output); }
    ~StdoutOverride() { SetStdHandle(STD_OUTPUT_HANDLE, original); }
};
void Reject(std::initializer_list<std::wstring> args, const char* code, const char* label) {
    const auto result = Evaluate(args);
    Check(result && result->exit_code == 2 && !result->output.get_bool("valid", true) &&
        result->output.get_string("code") == code && !result->output.get_string("error").empty() &&
        result->output.find("path") && result->output.find("path")->is_string(), label);
}
}

int main() {
    try {
        Fixture fixture;
        const std::string manifest = R"({"apiVersion":1,"id":"cli-test","name":"CLI test"})";
        const auto valid = fixture.File(L"valid.json", manifest);
        const auto broken = fixture.File(L"broken.json", "{");
        const auto unicode = fixture.File(L"\u6d4b\u8bd5-\u589e\u5f3a\u5305.json", manifest);
        std::string boundary = manifest;
        boundary.resize(kMaxManifestBytes, ' ');
        const auto exact = fixture.File(L"exact.json", boundary);
        boundary.push_back(' ');
        const auto oversized = fixture.File(L"oversized.json", boundary);
        Check(!Evaluate({}) && !Evaluate({L"--view", L"details"}) &&
            !Evaluate({L"--extension-check", valid.wstring()}), "old startup arguments remain unhandled");
        const auto api = Evaluate({L"--extension-api"});
        Check(api && api->exit_code == 0 && pulse::json::Serialize(api->output) == pulse::json::Serialize(DescribeExtensionApi()),
            "API discovery exactly matches shared contract");
        const auto guarded_api = Evaluate({L"--extension-api", L"--extension-check"});
        Check(guarded_api && guarded_api->exit_code == 0 &&
            pulse::json::Serialize(guarded_api->output) == pulse::json::Serialize(DescribeExtensionApi()),
            "final pathless compatibility guard preserves API discovery");
        const auto guarded_validation = Evaluate({L"--extension-validate-json", valid.wstring(), L"--extension-check"});
        Check(guarded_validation && guarded_validation->exit_code == 0 && guarded_validation->output.get_bool("valid"),
            "final pathless compatibility guard permits validation");
        for (const auto& path : {valid, unicode, exact}) {
            const auto result = Evaluate({L"--extension-validate-json", path.wstring()});
            Check(result && result->exit_code == 0 && result->output.get_bool("valid") &&
                result->output.get_string("code").empty() && result->output.get_string("error").empty() &&
                result->output.get_string("path") == pulse::json::Utf8(path.wstring()),
                "valid, Unicode and exact 64 KiB files validate with original path");
        }
        Reject({L"--extension-validate-json", oversized.wstring()}, "too_large", "65537-byte file rejects");
        Reject({L"--extension-validate-json", broken.wstring()}, "invalid_manifest", "malformed JSON rejects");
        const auto invalid = Evaluate({L"--extension-validate-json", broken.wstring()});
        Check(invalid && invalid->output.get_string("error") == ParseManifest("{").error,
            "parser diagnostic and position are preserved verbatim");
        Reject({L"--extension-validate-json", L"relative.json"}, "invalid_path", "relative path rejects");
        Reject({L"--extension-validate-json", L"C:relative.json"}, "invalid_path", "drive-relative path rejects");
        Reject({L"--extension-validate-json", L""}, "invalid_path", "empty path rejects");
        Reject({L"--extension-validate-json", (fixture.root / "missing.json").wstring()}, "read_failed", "missing file reports read failure");
        Reject({L"--extension-validate-json", fixture.root.wstring()}, "read_failed", "directory cannot be validated as file");
        Reject({L"--extension-validate-json"}, "invalid_args", "missing path argument rejects");
        Reject({L"--extension-api", L"--extension-api"}, "invalid_args", "duplicate discovery flag rejects");
        Reject({L"--extension-api", L"--extension-validate-json", valid.wstring()}, "invalid_args", "mixed developer flags reject");
        Reject({L"--extension-manifest", valid.wstring(), L"--extension-api"}, "invalid_args", "new flag mixed with load rejects");
        Reject({L"--extension-validate-json", valid.wstring(), L"--test-instance"}, "invalid_args", "validation rejects extra startup option");
        Reject({L"--unknown", L"--extension-api"}, "invalid_args", "new flag discovered after unrelated option rejects");
        Reject({L"--extension-api", L"--extension-check", valid.wstring()}, "invalid_args", "guard with a path rejects");
        Reject({L"--extension-validate-json", valid.wstring(), L"--extension-check", valid.wstring()},
            "invalid_args", "validation guard with a path rejects");
        Reject({L"--extension-check", L"--extension-api"}, "invalid_args", "guard before developer flag rejects");
        Reject({L"--extension-api", L"--extension-check", L"--extension-check"}, "invalid_args", "duplicate guards reject");

        Arguments load({L"--extension-manifest", valid.wstring()});
        Check(!PrepareStartup(load.Count(), load.pointers.data()).exit_requested && Enabled(), "seed runtime before developer CLI");
        const auto capture_path = fixture.File(L"stdout.json", "");
        HANDLE capture = CreateFileW(capture_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (capture == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open captured stdout");
        StartupResult startup;
        {
            StdoutOverride redirected(capture);
            Arguments args({L"--extension-api", L"--extension-check"});
            startup = PrepareStartup(args.Count(), args.pointers.data());
        }
        CloseHandle(capture);
        Check(startup.exit_requested && startup.exit_code == 0 && !Enabled() && !CurrentManifest(),
            "developer startup exits without retaining or enabling runtime package");
        std::ifstream captured(capture_path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(captured)), std::istreambuf_iterator<char>());
        Check(bytes == pulse::json::Serialize(DescribeExtensionApi()) + "\n", "inherited stdout receives exactly one UTF-8 contract JSON");
        std::optional<int> write_failure;
        {
            StdoutOverride redirected(INVALID_HANDLE_VALUE);
            Arguments args({L"--extension-api"});
            write_failure = HandleDeveloperCli(args.Count(), args.pointers.data());
        }
        Check(write_failure && *write_failure == 3, "stdout failure returns exit code 3");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[FAIL] CLI fixture: %s\n", error.what());
        return 1;
    }
    return failures ? 1 : 0;
}
