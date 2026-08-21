#include "../index/index_protocol.h"
#include "../ipc/protocol.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>

using namespace pulse::index;
using pulse::ipc::MsgHeader;
using pulse::ipc::PayloadReader;
using pulse::ipc::PayloadWriter;
using pulse::ipc::PipeRead;
using pulse::ipc::PipeWrite;

namespace {

struct Sample {
    double ms = 0;
    uint32_t total = 0;
};

bool ReadFrame(HANDLE pipe, MsgHeader& header, std::vector<uint8_t>& payload) {
    if (!PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header)) ||
        header.magic != kIndexMagic || header.payload_size > kIndexMaxPayload)
        return false;
    payload.resize(header.payload_size);
    return payload.empty() || PipeRead(pipe, payload.data(), header.payload_size);
}

HANDLE Connect() {
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) {
        WaitNamedPipeW(kPipeName, 2000);
        pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    }
    return pipe;
}

bool QueryStatus(uint32_t& count, std::wstring& status) {
    HANDLE pipe = Connect();
    if (pipe == INVALID_HANDLE_VALUE) return false;
    const MsgHeader request = MakeIndexHdr(REQ_IDX_STATUS, 1, 0);
    bool ok = PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&request), sizeof(request));
    MsgHeader response{};
    std::vector<uint8_t> payload;
    while (ok && ReadFrame(pipe, response, payload)) {
        if (response.type != RSP_IDX_STATUS || response.request_id != 1) continue;
        PayloadReader reader(payload.data(), payload.size());
        uint32_t ready = 0;
        ok = reader.GetU32(ready) && reader.GetU32(count) && reader.GetString(status) && ready != 0;
        break;
    }
    CloseHandle(pipe);
    return ok;
}

bool RunSearch(const std::wstring& needle, uint32_t request_id, Sample& sample) {
    HANDLE pipe = Connect();
    if (pipe == INVALID_HANDLE_VALUE) return false;
    PayloadWriter writer;
    writer.PutU32(1u); // ranked
    writer.PutU32(static_cast<uint32_t>(ResultSort::Index));
    writer.PutU32(48);
    writer.PutU32(0);
    writer.PutString(needle);
    writer.PutString(L"");
    const MsgHeader request = MakeIndexHdr(REQ_IDX_SEARCH, request_id,
                                            static_cast<uint32_t>(writer.data().size()));
    const auto start = std::chrono::steady_clock::now();
    bool ok = PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&request), sizeof(request)) &&
              PipeWrite(pipe, writer.data().data(), static_cast<DWORD>(writer.data().size()));
    MsgHeader response{};
    std::vector<uint8_t> payload;
    while (ok && ReadFrame(pipe, response, payload)) {
        if (response.type != RSP_IDX_SEARCH || response.request_id != request_id) continue;
        PayloadReader reader(payload.data(), payload.size());
        uint32_t hits = 0;
        ok = reader.GetU32(sample.total) && reader.GetU32(hits);
        break;
    }
    sample.ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    CloseHandle(pipe);
    return ok;
}

double Percentile(std::vector<double> values, double percentile) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(percentile * static_cast<double>(values.size() - 1));
    return values[index];
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool enforce = argc > 1 && std::wstring_view(argv[1]) == L"--enforce";
    uint32_t indexed = 0;
    std::wstring status;
    if (!QueryStatus(indexed, status)) {
        std::wcerr << L"[FAIL] PulseIndex is unavailable or not ready\n";
        return 2;
    }
    // Avoid writing the localized status through a narrow/legacy console code
    // page; the numeric result is enough for automated performance checks.
    (void)status;
    std::wcout << L"index items: " << indexed << L"\n\n";
    std::wcout << L"query       p50(ms)   p95(ms)   matches   budget\n";

    struct Case { const wchar_t* text; double budget; };
    const Case cases[] = {{L"a", 150.0}, {L"ab", 50.0}, {L"abc", 50.0},
                          {L"report", 100.0}, {L"pdf", 100.0}};
    bool passed = true;
    uint32_t request_id = 100;
    for (const Case& test : cases) {
        std::vector<double> times;
        uint32_t matches = 0;
        for (int i = 0; i < 5; ++i) {
            Sample sample;
            if (!RunSearch(test.text, request_id++, sample)) {
                std::wcerr << L"[FAIL] query transport failed: " << test.text << L"\n";
                return 2;
            }
            times.push_back(sample.ms);
            matches = sample.total;
        }
        const double p50 = Percentile(times, 0.50);
        const double p95 = Percentile(times, 0.95);
        const bool within = p95 <= test.budget;
        passed = passed && within;
        std::wcout << std::left << std::setw(10) << test.text << std::right
                   << std::setw(9) << std::fixed << std::setprecision(2) << p50
                   << std::setw(10) << p95 << std::setw(10) << matches
                   << L"   " << (within ? L"PASS" : L"FAIL") << L" <= "
                   << test.budget << L" ms\n";
    }
    return enforce && !passed ? 1 : 0;
}
