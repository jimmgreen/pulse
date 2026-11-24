#include "../app/git_status.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr size_t kBlockSize = 16384;
constexpr size_t kFiniteBlocks = 128;

int Fixture(bool stream) {
    std::array<char, kBlockSize> block;
    block.fill('g');
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!stream) {
        for (size_t i = 0; i < kFiniteBlocks; ++i) {
            DWORD written = 0;
            if (!WriteFile(output, block.data(), static_cast<DWORD>(block.size()), &written, nullptr) ||
                written != block.size()) return 2;
        }
        return 0;
    }
    const ULONGLONG until = GetTickCount64() + 2000;
    // Large pending writes keep the small anonymous pipe supplied while the
    // reader drains it; tiny writes can leave scheduling gaps between reads.
    const std::string stream_block(1024 * 1024, 'g');
    std::vector<std::thread> writers;
    for (int i = 0; i < 8; ++i) {
        writers.emplace_back([&] {
            while (GetTickCount64() < until) {
                DWORD written = 0;
                if (!WriteFile(output, stream_block.data(), static_cast<DWORD>(stream_block.size()), &written, nullptr)) return;
            }
        });
    }
    for (auto& writer : writers) writer.join();
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--fixture-stream") return Fixture(true);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--fixture-finite") return Fixture(false);
    if (argc != 2) {
        std::puts("usage: git_run_timeout_test.exe FIXTURE_DIRECTORY");
        return 2;
    }
    const std::filesystem::path fixture = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(fixture);
    std::array<wchar_t, 32768> self{};
    if (!GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())) ||
        !CopyFileW(self.data(), (fixture / L"git.exe").c_str(), FALSE) ||
        !SetEnvironmentVariableW(L"PATH", fixture.c_str())) {
        std::printf("[FAIL] fixture setup error=%lu\n", GetLastError());
        return 2;
    }

    const auto finite = pulse::app::RunGit(fixture.wstring(), L"--fixture-finite", 10000, false);
    const bool complete = finite.started && !finite.timed_out && finite.exit_code == 0 &&
        finite.out.size() == kBlockSize * kFiniteBlocks && finite.out.find_first_not_of('g') == std::string::npos;
    std::printf("[%s] finite output: bytes=%zu expected=%zu exit=%lu timed_out=%d\n",
                complete ? "PASS" : "FAIL", finite.out.size(), kBlockSize * kFiniteBlocks,
                finite.exit_code, finite.timed_out);

    const ULONGLONG start = GetTickCount64();
    const auto stream = pulse::app::RunGit(fixture.wstring(), L"--fixture-stream", 100, true);
    const ULONGLONG elapsed = GetTickCount64() - start;
    const bool bounded = stream.started && stream.timed_out && !stream.out.empty() && elapsed < 1000;
    std::printf("[%s] continuous output: elapsed_ms=%llu timeout_ms=100 limit_ms=1000 bytes=%zu timed_out=%d\n",
                bounded ? "PASS" : "FAIL", elapsed, stream.out.size(), stream.timed_out);
    return complete && bounded ? 0 : 1;
}
