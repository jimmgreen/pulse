#pragma once
#include "../../../../sdk/native/include/pulse/search.h"
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulse::extensions::native {

class DiagnosticsSink;

struct SearchQuerySpec {
    uint64_t request_id = 0;
    uint32_t flags = PULSE_SEARCH_FLAG_NONE;
    uint32_t max_results = 20;
    std::wstring query;
    std::wstring kind_token;
    std::wstring scope_directory;
};

struct SearchHitData {
    std::string path;
    std::string name;
    uint64_t size_bytes = 0;
    int64_t modified_unix_ms = 0;
    uint32_t flags = PULSE_SEARCH_HIT_NONE;
};

struct SearchBackendReply {
    PulseStatus status = PULSE_OK;
    uint64_t total_matches = 0;
    bool index_connected = false;
    std::vector<SearchHitData> hits;
};

struct SearchBackend {
    std::function<bool()> valid_owner;
    std::function<PulseStatus(SearchQuerySpec spec, std::function<void(SearchBackendReply)> complete)> start;
    std::function<void(uint64_t request_id)> cancel;
};

class SearchAdapter {
public:
    SearchAdapter(SearchBackend backend, std::function<bool()> alive,
                  std::shared_ptr<DiagnosticsSink> diagnostics = nullptr,
                  std::function<void()> wake = nullptr);
    SearchAdapter(HWND owner, std::function<bool()> alive,
                  std::shared_ptr<DiagnosticsSink> diagnostics = nullptr,
                  std::function<void()> wake = nullptr);
    ~SearchAdapter();
    SearchAdapter(const SearchAdapter&) = delete;
    SearchAdapter& operator=(const SearchAdapter&) = delete;

    void SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink);
    void SetWake(std::function<void()> wake);
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void Poll() noexcept;
    bool HasPendingWork() const noexcept;
    void Close() noexcept;
    bool CallbackActive() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::extensions::native
