#include "../ipc/shell_client.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>

namespace {
struct Sample {
    ULONGLONG start = 0;
    ULONGLONG first = 0;
    ULONGLONG pin = 0;
    ULONGLONG complete = 0;
    size_t rows = 0;
    bool done = false;
    bool final_pin = false;
};
std::mutex mutex;
std::condition_variable changed;
std::map<uint32_t, Sample> samples;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    const bool baseline = argc > 2 && wcscmp(argv[2], L"--baseline") == 0;
    wchar_t absolute[32768]{};
    if (!GetFullPathNameW(argv[1], ARRAYSIZE(absolute), absolute, nullptr)) return 2;
    pulse::ipc::ShellClient::Callbacks callbacks;
    callbacks.ctx_items = [](uint32_t id, std::vector<pulse::ipc::CtxMenuItem> items,
                             bool partial, pulse::ipc::CtxItemsExtra) {
        std::lock_guard lock(mutex);
        auto& sample = samples[id];
        const auto elapsed = GetTickCount64() - sample.start;
        if (!sample.first && !items.empty()) sample.first = elapsed;
        for (const auto& item : items) {
            if (!sample.pin && (_wcsicmp(item.verb.c_str(), L"pintostartscreen") == 0 ||
                               _wcsicmp(item.verb.c_str(), L"unpinfromstartscreen") == 0))
                sample.pin = elapsed;
        }
        if (!partial) {
            sample.complete = elapsed;
            sample.rows = items.size();
            sample.done = true;
            for (const auto& item : items)
                sample.final_pin |= _wcsicmp(item.verb.c_str(), L"pintostartscreen") == 0 ||
                                    _wcsicmp(item.verb.c_str(), L"unpinfromstartscreen") == 0;
        }
        changed.notify_all();
    };
    auto& client = pulse::ipc::ShellClient::Instance();
    client.Start(std::move(callbacks));
    int failures = 0;
    std::vector<ULONGLONG> first, complete;
    for (int round = 0; round < 6; ++round) {
        const auto start = GetTickCount64();
        const auto id = client.QueryContextMenu({absolute}, 0, false, false, {},
            [start](uint32_t accepted) {
                std::lock_guard lock(mutex);
                samples[accepted].start = start;
            });
        Sample sample;
        {
            std::unique_lock lock(mutex);
            const bool ready = id && changed.wait_for(lock, std::chrono::seconds(15),
                [&] { return samples[id].done; });
            if (!ready) ++failures;
            sample = samples[id];
        }
        printf("round=%d first_ms=%llu pin_ms=%llu complete_ms=%llu rows=%zu done=%d\n",
            round, sample.first, sample.pin, sample.complete, sample.rows, sample.done);
        if (!baseline && !sample.final_pin) ++failures;
        if (round && sample.done) {
            first.push_back(sample.first);
            complete.push_back(sample.complete);
        }
        client.CloseContextMenu(id);
        Sleep(100); // allow the dismissed session to release its worker objects
    }
    auto verify_query = [&](const std::vector<std::wstring>& disabled, bool expect_pin) {
        const auto start = GetTickCount64();
        const auto id = client.QueryContextMenu({absolute}, 0, false, false, disabled,
            [start](uint32_t accepted) {
                std::lock_guard lock(mutex);
                samples[accepted].start = start;
            });
        bool valid = false;
        {
            std::unique_lock lock(mutex);
            valid = id && changed.wait_for(lock, std::chrono::seconds(15),
                [&] { return samples[id].done; }) && samples[id].final_pin == expect_pin;
        }
        client.CloseContextMenu(id);
        return valid;
    };
    const bool disabled = verify_query({L"{470c0ebd-5d73-4d58-9ced-e91e22e23282}"}, false);
    printf("[%s] explicit disable removes Start pin\n", disabled ? "PASS" : "FAIL");
    failures += !disabled;
    for (int i = 0; i < 3; ++i) {
        const auto id = client.QueryContextMenu({absolute}, 0, false, false);
        client.CloseContextMenu(id);
    }
    if (!baseline) {
        const bool recovered = verify_query({}, true);
        printf("[%s] rapid dismissal does not break the next menu\n", recovered ? "PASS" : "FAIL");
        failures += !recovered;
    }
    client.Stop();
    if (!first.empty()) {
        std::sort(first.begin(), first.end());
        std::sort(complete.begin(), complete.end());
        printf("warm_median_first_ms=%llu warm_median_complete_ms=%llu failures=%d\n",
            first[first.size() / 2], complete[complete.size() / 2], failures);
    }
    return failures ? 1 : 0;
}
