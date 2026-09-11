#include "change_tracking_client.h"
#include "index_protocol.h"
#include "network_agent_protocol.h"
#include <algorithm>
#include <chrono>
#include <atomic>

namespace pulse::index {
namespace {
bool Transfer(HANDLE pipe, void* bytes, DWORD size, bool write, ULONGLONG deadline = 0, HANDLE cancel = nullptr) {
    if (!deadline) deadline = GetTickCount64() + 1500;
    auto* p = static_cast<uint8_t*>(bytes);
    while (size) {
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) { SetLastError(ERROR_OPERATION_ABORTED); return false; }
        const auto now = GetTickCount64();
        if (now >= deadline) { SetLastError(ERROR_TIMEOUT); return false; }
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;
        DWORD done = 0;
        BOOL ok = write ? WriteFile(pipe, p, size, &done, &overlapped)
                        : ReadFile(pipe, p, size, &done, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            HANDLE events[] = {overlapped.hEvent, cancel};
            const DWORD wait = WaitForMultipleObjects(cancel ? 2u : 1u, events, FALSE, static_cast<DWORD>(deadline - now));
            const bool timeout = wait != WAIT_OBJECT_0;
            if (timeout) CancelIoEx(pipe, &overlapped);
            ok = GetOverlappedResult(pipe, &overlapped, &done, TRUE);
            if (timeout) { CloseHandle(overlapped.hEvent); SetLastError(wait == WAIT_OBJECT_0 + 1 ? ERROR_OPERATION_ABORTED : ERROR_TIMEOUT); return false; }
        }
        const DWORD error = GetLastError();
        CloseHandle(overlapped.hEvent);
        if (!ok || !done) { SetLastError(ok ? ERROR_BROKEN_PIPE : error); return false; }
        p += done;
        size -= done;
    }
    return true;
}
struct ExchangeDiagnostics { const char* stage = "start"; DWORD error = 0; ULONGLONG elapsed = 0; uint32_t skipped = 0; };
bool Exchange(HANDLE pipe, uint32_t type, const std::vector<uint8_t>& input,
              std::vector<uint8_t>& output, uint32_t magic = kIndexMagic,
              ExchangeDiagnostics* diagnostics = nullptr, DWORD timeout_ms = 1500, HANDLE cancel = nullptr) {
    if (input.size() > kIndexMaxRequestPayload) return false;
    static std::atomic<uint32_t> next_request{1};
    uint32_t request_id = next_request.fetch_add(1);
    if (!request_id) request_id = next_request.fetch_add(1);
    const auto started = GetTickCount64();
    const auto deadline = started + timeout_ms;
    auto finish = [&](bool ok, const char* stage) {
        if (diagnostics) { diagnostics->stage = stage; diagnostics->error = ok ? 0 : GetLastError();
            diagnostics->elapsed = GetTickCount64() - started; }
        return ok;
    };
    auto header = MakeIndexHdr(type, request_id, static_cast<uint32_t>(input.size()));
    header.magic = magic;
    if (!Transfer(pipe, &header, sizeof(header), true, deadline, cancel)) return finish(false, "write-header");
    if (!input.empty() && !Transfer(pipe, const_cast<uint8_t*>(input.data()),
        static_cast<DWORD>(input.size()), true, deadline, cancel)) return finish(false, "write-payload");
    while (GetTickCount64() < deadline) {
        if (!Transfer(pipe, &header, sizeof(header), false, deadline, cancel)) return finish(false, "read-header");
        if (header.magic != magic || header.payload_size > kIndexMaxPayload) {
            SetLastError(ERROR_INVALID_DATA); return finish(false, "validate-header");
        }
        output.resize(header.payload_size);
        if (!output.empty() && !Transfer(pipe, output.data(), header.payload_size, false, deadline, cancel))
            return finish(false, "read-payload");
        if (header.type == type + 100 && header.request_id == request_id) return finish(true, "reply");
        if (diagnostics) ++diagnostics->skipped;
    }
    SetLastError(ERROR_TIMEOUT); return finish(false, "broadcast-deadline");
}
}
void ChangeTrackingClient::Start(HWND notify, UINT message) {
    std::lock_guard lock(mutex_);
    if (running_) return;
    cancel_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!cancel_) return;
    notify_ = notify; message_ = message; running_ = true;
    worker_ = std::thread(&ChangeTrackingClient::Worker, this);
}
void ChangeTrackingClient::Stop() {
    { std::lock_guard lock(mutex_); running_ = false; if (cancel_) SetEvent(cancel_); }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (cancel_) { CloseHandle(cancel_); cancel_ = nullptr; }
}
void ChangeTrackingClient::SetEnabled(bool enabled) {
    { std::lock_guard lock(mutex_); if (enabled_ == enabled) return;
      enabled_ = enabled; changed_ = true;
      if (cancel_) { if (enabled) ResetEvent(cancel_); else SetEvent(cancel_); }
      if (!enabled) { summary_.pending = false; details_queue_.clear(); details_results_.clear();
          have_summary_ = false; } }
    cv_.notify_one();
}
void ChangeTrackingClient::QuerySummaryAsync(std::vector<std::wstring> paths, uint64_t since, uint32_t id) {
    ipc::PayloadWriter writer;
    writer.PutU64(since); writer.PutStringArray(paths);
    { std::lock_guard lock(mutex_); summary_ = {id, writer.data(), true}; have_summary_ = false; }
    cv_.notify_one();
}
void ChangeTrackingClient::QueryDetailsAsync(std::wstring path, uint64_t since, uint64_t before,
                                            uint32_t limit, uint32_t id, uint32_t kind_filter) {
    ipc::PayloadWriter writer;
    writer.PutString(path); writer.PutU64(since); writer.PutU64(before);
    writer.PutU32(std::clamp(limit, 1u, 200u)); writer.PutU32(kind_filter);
    { std::lock_guard lock(mutex_);
      details_results_.erase(id);
      if (details_queue_.size() >= 64) {
          const auto rejected = details_queue_.front().id;
          details_queue_.pop_front();
          if (details_results_.size() >= 64) details_results_.erase(details_results_.begin());
          details_results_[rejected] = {};
          if (notify_ && message_) PostMessageW(notify_, message_, rejected, 1);
      }
      details_queue_.push_back({id, writer.data(), true}); }
    cv_.notify_one();
}
bool ChangeTrackingClient::TakeSummary(uint32_t id, ChangeResponse& result) {
    std::lock_guard lock(mutex_);
    if (!have_summary_ || summary_id_ != id) return false;
    result = std::move(summary_result_); have_summary_ = false; return true;
}
bool ChangeTrackingClient::TakeDetails(uint32_t id, ChangeResponse& result) {
    std::lock_guard lock(mutex_);
    const auto found = details_results_.find(id);
    if (found == details_results_.end()) return false;
    result = std::move(found->second); details_results_.erase(found); return true;
}
bool Decode(const std::vector<uint8_t>& bytes, bool details, ChangeResponse& out) {
    ipc::PayloadReader reader(bytes.data(), bytes.size());
    uint32_t state = 0, count = 0;
    if (!reader.GetU32(state) || state > 5) return false;
    out.state = static_cast<ChangeState>(state);
    if (details && !reader.GetU64(out.next_cursor)) return false;
    if (!reader.GetU32(count) || count > (details ? 200u : 8192u)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (details) {
            ChangeRecord record;
            uint32_t kind = 0, directory = 0, source = 0;
            if (!reader.GetU64(record.id) || !reader.GetU64(record.time) ||
                !reader.GetU32(kind) || kind > 5 || !reader.GetU32(directory) ||
                !reader.GetU32(source) || source > 1 || !reader.GetString(record.path) ||
                !reader.GetString(record.old_path)) return false;
            record.kind = static_cast<ChangeKind>(kind); record.is_dir = directory != 0;
            record.source = static_cast<ChangeSource>(source);
            out.records.push_back(std::move(record));
        } else {
            ChangeSummary summary;
            uint32_t deleted = 0, incomplete = 0;
            if (!reader.GetString(summary.path) || !reader.GetU64(summary.last_change) ||
                !reader.GetU32(summary.count) || !reader.GetU32(state) || state > 5) return false;
            summary.state = static_cast<ChangeState>(state);
            for (auto& value : summary.counts) if (!reader.GetU32(value)) return false;
            if (!reader.GetU32(summary.initial_count) || !reader.GetU32(deleted) || !reader.GetU32(incomplete)) return false;
            summary.has_deleted = deleted != 0; summary.incomplete = incomplete != 0;
            out.summaries.push_back(std::move(summary));
        }
    }
    return true;
}

namespace {
bool IsNetworkPath(const std::wstring& path) {
    return (path.size() >= 8 && _wcsnicmp(path.c_str(), L"\\\\?\\UNC\\", 8) == 0) ||
           (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\?\\") && !path.starts_with(L"\\\\.\\"));
}
bool NetworkExchange(uint32_t type, const std::vector<uint8_t>& payload, std::vector<uint8_t>& reply, HANDLE cancel = nullptr, DWORD timeout_ms = 1500) {
    HANDLE pipe = CreateFileW(agent::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY &&
        WaitNamedPipeW(agent::kPipeName, 200)) {
        pipe = CreateFileW(agent::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;
    const bool ok = Exchange(pipe, type, payload, reply, agent::kMagic, nullptr, timeout_ms, cancel);
    CloseHandle(pipe);
    return ok;
}
}
void ChangeTrackingClient::Worker() {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool leased = false, network_leased = false;
    ULONGLONG renew_at = 0, network_renew_at = 0;
    auto disconnect = [&] { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE; leased = false; };
    std::vector<uint8_t> reply;
    auto lease = [&](bool enabled) {
        ipc::PayloadWriter writer; writer.PutU32(enabled ? 1u : 0u);
        return Exchange(pipe, REQ_IDX_CHANGE_LEASE, writer.data(), reply);
    };
    auto network_lease = [&](bool enabled) {
        ipc::PayloadWriter writer; writer.PutU32(enabled ? 1u : 0u);
        return NetworkExchange(7, writer.data(), reply);
    };
    auto query = [&](bool network, bool detail, const std::vector<uint8_t>& payload) {
        ChangeResponse result;
        { std::lock_guard lock(mutex_); if (!running_ || !enabled_) return result; }
        const bool ok = network
            ? network_leased && NetworkExchange(detail ? 9u : 8u, payload, reply, cancel_, 10000)
            : leased && Exchange(pipe, detail ? REQ_IDX_CHANGE_DETAILS : REQ_IDX_CHANGE_SUMMARIES, payload, reply, kIndexMagic, nullptr, 10000, cancel_);
        if (!ok || !Decode(reply, detail, result)) {
            result = {};
            if (!network) disconnect();
        }
        return result;
    };
    for (;;) {
        Request request;
        bool detail = false, enabled = false, changed = false;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(1), [&] {
                return !running_ || changed_ || (enabled_ && (summary_.pending || !details_queue_.empty()));
            });
            if (!running_) break;
            enabled = enabled_; changed = changed_; changed_ = false;
            if (enabled && !details_queue_.empty()) { request = std::move(details_queue_.front()); details_queue_.pop_front(); detail = true; }
            else if (enabled && summary_.pending) { request = summary_; summary_.pending = false; }
        }
        if (!enabled) {
            if (leased) lease(false);
            if (network_leased) network_lease(false);
            network_leased = false;
            disconnect(); continue;
        }
        if (changed || GetTickCount64() >= network_renew_at) {
            network_leased = network_lease(true);
            network_renew_at = GetTickCount64() + 10000;
        }
        if (pipe == INVALID_HANDLE_VALUE && (request.pending || changed || GetTickCount64() >= renew_at)) {
            pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            renew_at = GetTickCount64() + 10000;
        }
        if (pipe != INVALID_HANDLE_VALUE && (!leased || GetTickCount64() >= renew_at)) {
            leased = lease(true);
            renew_at = GetTickCount64() + 10000;
            if (!leased) disconnect();
        }
        if (!request.pending) continue;
        ChangeResponse result;
        ipc::PayloadReader reader(request.payload.data(), request.payload.size());
        if (detail) {
            std::wstring path;
            if (reader.GetString(path)) result = query(IsNetworkPath(path), true, request.payload);
        } else {
            uint64_t since = 0;
            uint32_t count = 0;
            std::vector<std::wstring> paths[2];
            if (reader.GetU64(since) && reader.GetU32(count)) {
                for (uint32_t i = 0; i < count; ++i) {
                    std::wstring path;
                    if (!reader.GetString(path)) break;
                    paths[IsNetworkPath(path) ? 1 : 0].push_back(std::move(path));
                }
            }
            result.state = ChangeState::Available;
            for (int endpoint = 0; endpoint < 2; ++endpoint) {
                // Bound batches even when a directory contains many folders.
                for (size_t first = 0; first < paths[endpoint].size(); first += 128) {
                    const size_t end = std::min(first + 128, paths[endpoint].size());
                    std::vector<std::wstring> batch(paths[endpoint].begin() + first, paths[endpoint].begin() + end);
                    ipc::PayloadWriter writer; writer.PutU64(since); writer.PutStringArray(batch);
                    auto part = query(endpoint != 0, false, writer.data());
                    if (part.state != ChangeState::Available) result.state = part.state;
                    if (part.summaries.empty()) {
                        for (auto& path : batch) { ChangeSummary summary; summary.path = std::move(path);
                            summary.state = part.state; result.summaries.push_back(std::move(summary)); }
                    } else {
                        for (auto& summary : part.summaries) result.summaries.push_back(std::move(summary));
                    }
                    std::lock_guard lock(mutex_);
                    if (!running_ || !enabled_ || summary_.id != request.id) break;
                }
            }
        }
        std::lock_guard lock(mutex_);
        if (!running_ || !enabled_ || (!detail && summary_.id != request.id)) continue;
        if (detail) {
            if (details_results_.size() >= 64) details_results_.erase(details_results_.begin());
            details_results_[request.id] = std::move(result);
        }
        else { summary_id_ = request.id; summary_result_ = std::move(result); have_summary_ = true; }
        if (notify_ && message_) PostMessageW(notify_, message_, request.id, detail ? 1 : 0);
    }
    if (leased) lease(false);
    if (network_leased) network_lease(false);
    disconnect();
}
}
