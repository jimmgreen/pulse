#include "content_agent.h"
#include "content_search.h"
#include "content_search_protocol.h"
#include "content_instant_session.h"
#include "../common/current_user_security.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <memory>
#include <set>

namespace pulse::index {
namespace {

bool ReadAll(HANDLE pipe, void* output, DWORD size) {
    auto* bytes = static_cast<uint8_t*>(output);
    while (size) {
        DWORD read = 0;
        if (!ReadFile(pipe, bytes, size, &read, nullptr) || !read) return false;
        bytes += read;
        size -= read;
    }
    return true;
}

bool WriteAll(HANDLE pipe, const void* input, DWORD size) {
    const auto* bytes = static_cast<const uint8_t*>(input);
    while (size) {
        DWORD written = 0;
        if (!WriteFile(pipe, bytes, size, &written, nullptr) || !written) return false;
        bytes += written;
        size -= written;
    }
    return true;
}

bool ParseRequest(const std::vector<uint8_t>& payload, ContentSearchRequest& request) {
    ipc::PayloadReader reader(payload.data(), payload.size());
    uint32_t mode = 0;
    uint32_t flags = 0;
    uint32_t maximum_hits = 0;
    if (!reader.GetU64(request.generation) || !reader.GetU32(mode) ||
        !reader.GetU32(flags) || !reader.GetU64(request.maximum_file_bytes) ||
        !reader.GetU32(maximum_hits) || !reader.GetString(request.root) ||
        !reader.GetString(request.needle)) return false;
    if (mode > static_cast<uint32_t>(ContentSearchMode::Duplicates) ||
        request.root.size() > 32768 || request.needle.size() > 4096 ||
        request.maximum_file_bytes > 64ull * 1024ull * 1024ull ||
        (mode == static_cast<uint32_t>(ContentSearchMode::Duplicates) && maximum_hits > 10000)) return false;
    request.mode = static_cast<ContentSearchMode>(mode);
    request.recursive = (flags & 1) != 0;
    request.case_sensitive = (flags & 2) != 0;
    request.skip_system_locations = (flags & 4) != 0;
    request.whole_word = (flags & 8) != 0;
    const bool roots_only = (flags & 16) != 0 && request.mode == ContentSearchMode::Content;
    request.allow_uncovered_root = (flags & 32) != 0;
    request.maximum_hits = maximum_hits;
    if (reader.remaining() >= 12) {
        uint32_t extra_roots = 0;
        if (!reader.GetU64(request.minimum_file_bytes) || !reader.GetU32(extra_roots) ||
            extra_roots > 32) return false;
        std::vector<std::wstring> extras;
        extras.reserve(extra_roots);
        for (uint32_t i = 0; i < extra_roots; ++i) {
            std::wstring extra;
            if (!reader.GetString(extra) || extra.empty() || extra.size() > 32768) return false;
            extras.push_back(std::move(extra));
        }
        if (!request.root.empty()) request.roots.push_back(request.root);
        request.roots.insert(request.roots.end(), extras.begin(), extras.end());
        // The root field was only the first folder of a multi-folder search.
        if (roots_only) request.root.clear();
        if (reader.remaining() >= 12) {
            uint32_t match_mode = 0;
            uint32_t extra_needles = 0;
            if (!reader.GetU32(match_mode) || !reader.GetU32(extra_needles) ||
                extra_needles > 32 || match_mode > 2) return false;
            request.match_mode = static_cast<ContentMatchMode>(match_mode);
            request.needles.reserve(extra_needles);
            for (uint32_t i = 0; i < extra_needles; ++i) {
                std::wstring needle;
                if (!reader.GetString(needle) || needle.size() > 4096) return false;
                if (!needle.empty()) request.needles.push_back(std::move(needle));
            }
            uint32_t excluded = 0;
            if (!reader.GetU32(excluded) || excluded > 32) return false;
            request.excluded_needles.reserve(excluded);
            for (uint32_t i = 0; i < excluded; ++i) {
                std::wstring needle;
                if (!reader.GetString(needle) || needle.size() > 4096) return false;
                if (!needle.empty()) request.excluded_needles.push_back(std::move(needle));
            }
            uint32_t candidates = 0;
            if (!reader.GetU32(candidates) || candidates > 10000) return false;
            request.candidate_paths.reserve(candidates);
            for (uint32_t i = 0; i < candidates; ++i) {
                std::wstring path;
                if (!reader.GetString(path) || path.empty() || path.size() > 32768) return false;
                request.candidate_paths.push_back(std::move(path));
            }
        }
    } else if (!request.root.empty()) {
        request.roots.push_back(request.root);
    }
    if (reader.remaining() && !reader.GetString(request.filename_query)) return false;
    if (reader.remaining()) {
        uint32_t sort = 0, descending = 0;
        if (!reader.GetU32(sort) || sort > 5 || !reader.GetU32(descending) || descending > 1) return false;
        request.sort = static_cast<ContentResultSort>(sort); request.sort_desc = descending != 0;
    }
    if (request.mode == ContentSearchMode::Duplicates && request.roots.empty() && request.candidate_paths.empty()) return false;
    if (request.root.empty() && !request.roots.empty() && !roots_only) request.root = request.roots.front();
    if (reader.remaining()) {
        uint32_t version = 0;
        if (!reader.GetU32(version) || version != 1 || !reader.GetU64(request.session_id)) return false;
    }
    if (reader.remaining() && (!reader.GetU64(request.maximum_document_bytes) || !request.maximum_document_bytes ||
        request.maximum_document_bytes > 512ull * 1024 * 1024)) return false;
    if (reader.remaining()) {
        uint32_t task_scan = 0;
        if (!reader.GetU32(task_scan) || task_scan > 1) return false;
        request.task_scan = task_scan != 0;
    }
    return reader.remaining() == 0;
}

bool SendBatch(HANDLE pipe, const ContentSearchProgress& progress,
               const std::vector<ContentHit>& hits) {
    ipc::PayloadWriter writer;
    writer.PutU64(progress.generation);
    writer.PutU64(progress.scanned_files);
    writer.PutU64(progress.scanned_bytes);
    uint32_t flags = progress.done ? 1u : 0u;
    if (progress.truncated) flags |= 2u;
    if (progress.live) flags |= 4u;
    if (progress.delta) flags |= 8u;
    writer.PutU32(flags);
    writer.PutU32(progress.error);
    writer.PutU32(static_cast<uint32_t>(progress.phase));
    writer.PutU64(progress.total_files);
    writer.PutString(progress.current_root);
    writer.PutU32(static_cast<uint32_t>(hits.size()));
    for (const auto& hit : hits) {
        writer.PutString(hit.path);
        writer.PutString(hit.name);
        writer.PutString(hit.snippet);
        writer.PutU64(hit.size);
        writer.PutU64(hit.modified);
        writer.PutU32(hit.line);
        writer.PutU32(hit.group | (hit.removed ? 0x80000000u : 0u));
    }
    writer.PutU64(progress.index_revision);
    writer.PutU32(static_cast<uint32_t>(hits.size()));
    for(const auto& hit:hits) writer.PutU64(hit.file_id);
    content::PutSubscriptionStatus(writer, progress);
    const auto header = content::Header(content::RSP_BATCH,
        static_cast<uint32_t>(writer.data().size()));
    return WriteAll(pipe, &header, sizeof(header)) &&
        (writer.data().empty() || WriteAll(pipe, writer.data().data(),
                                          static_cast<DWORD>(writer.data().size())));
}

} // namespace

int RunContentAgent(const std::wstring& token) {
    if (token.empty() || token.size() > 64 ||
        !std::all_of(token.begin(), token.end(), [](wchar_t c) {
            return iswalnum(c) || c == L'-' || c == L'_';
        })) return ERROR_INVALID_PARAMETER;
    CurrentUserSecurityAttributes security;
    if (!security) return ERROR_ACCESS_DENIED;
    const std::wstring pipe_name = content::PipeName(token);
    HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64 * 1024, 64 * 1024, 0, security.get());
    if (pipe == INVALID_HANDLE_VALUE) return static_cast<int>(GetLastError());
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        const DWORD error = GetLastError();
        CloseHandle(pipe);
        return static_cast<int>(error);
    }
    ipc::MsgHeader header{};
    if (!ReadAll(pipe, &header, sizeof(header)) || header.magic != content::kMagic ||
        header.type != content::REQ_SEARCH || header.payload_size > content::kMaximumPayload) {
        CloseHandle(pipe);
        return ERROR_INVALID_DATA;
    }
    std::vector<uint8_t> payload(header.payload_size);
    if (header.payload_size && !ReadAll(pipe, payload.data(), header.payload_size)) {
        CloseHandle(pipe);
        return ERROR_BROKEN_PIPE;
    }
    ContentSearchRequest request;
    if (!ParseRequest(payload, request)) {
        CloseHandle(pipe);
        return ERROR_INVALID_DATA;
    }
    std::atomic<bool> cancelled{false};
    const bool ok = RunContentSearch(request, cancelled,
        [pipe](const ContentSearchProgress& progress, std::vector<ContentHit> hits) {
            return SendBatch(pipe, progress, hits);
        });
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    return ok ? 0 : ERROR_CANCELLED;
}

int RunPersistentContentAgent(const std::wstring& token, DWORD parent_pid, bool read_only) {
    return RunPersistentContentAgent(token, parent_pid, read_only ? ContentAgentMode::Observer : ContentAgentMode::LegacyWriter);
}
int RunPersistentContentAgent(const std::wstring& token, DWORD parent_pid, ContentAgentMode mode) {
    if (token.empty() || token.size() > 64 || !parent_pid ||
        !std::all_of(token.begin(), token.end(), [](wchar_t c) { return iswalnum(c) || c == L'-' || c == L'_'; })) return ERROR_INVALID_PARAMETER;
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    if (!parent) return static_cast<int>(GetLastError());
    CurrentUserSecurityAttributes security;
    if (!security) { CloseHandle(parent); return ERROR_ACCESS_DENIED; }
    ContentIndex index({}, mode);
    std::atomic<bool> stopping{false};
    HANDLE accept_thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &accept_thread, THREAD_TERMINATE, FALSE, 0);
    HANDLE monitor_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread monitor([&] {
        HANDLE events[]{parent, monitor_stop};
        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
            stopping = true;
            if (accept_thread) CancelSynchronousIo(accept_thread);
        }
    });
    struct Handler { std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
    std::vector<Handler> handlers;
    std::mutex sessions_mu;
    std::map<uint64_t, std::shared_ptr<std::atomic<bool>>> sessions;
    std::set<uint64_t> cancelled_generations;
    const auto pipe_name = content::PipeName(token);
    while (!stopping.load()) {
        for (auto it = handlers.begin(); it != handlers.end();) {
            if (it->done->load()) { it->thread.join(); it = handlers.erase(it); } else ++it;
        }
        HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            16, 64 * 1024, 64 * 1024, 0, security.get());
        if (pipe == INVALID_HANDLE_VALUE) break;
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(pipe); if (stopping) break; continue; }
        if (stopping) { CloseHandle(pipe); break; }
        auto done = std::make_shared<std::atomic<bool>>(false);
        handlers.push_back({std::thread([&, pipe, done] {
            ipc::MsgHeader header{};
            bool valid = ReadAll(pipe, &header, sizeof(header)) && header.magic == content::kMagic && header.payload_size <= content::kMaximumPayload;
            std::vector<uint8_t> payload(valid ? header.payload_size : 0);
            if (valid && !payload.empty()) valid = ReadAll(pipe, payload.data(), header.payload_size);
            if (valid && (header.type == content::REQ_SEARCH || header.type == content::REQ_SUBSCRIBE)) {
                ContentSearchRequest request;
                if (ParseRequest(payload, request) && request.mode == ContentSearchMode::Content) {
                    auto cancel = std::make_shared<std::atomic<bool>>(false);
                    request.subscribe = header.type == content::REQ_SUBSCRIBE;
                    { std::lock_guard lock(sessions_mu); *cancel = cancelled_generations.contains(request.generation); sessions[request.generation] = cancel; }
                    do {
                        uint64_t revision = request.after_revision;
                        const auto send = [&](const auto& progress, auto hits) {
                            if (progress.done) revision = progress.index_revision;
                            return SendBatch(pipe, progress, hits);
                        };
                        if (mode == ContentAgentMode::Instant) {
                            ContentIndex query({}, ContentAgentMode::Instant);
                            RunInstantContentSession(query, request, *cancel, send);
                            break;
                        }
                        const bool ok = request.task_scan && !request.incremental && !request.after_revision
                            ? index.SearchTask(request, *cancel, send) : index.Search(request, *cancel, send);
                        if (!ok || !request.subscribe || cancel->load() || stopping.load()) break;
                        request.after_revision = revision;
                        request.incremental = true;
                        while (!cancel->load() && !stopping.load() && !index.WaitForRevision(revision, *cancel, 250)) {}
                    } while (!cancel->load() && !stopping.load());
                    { std::lock_guard lock(sessions_mu); sessions.erase(request.generation); }
                }
            } else if (valid) {
                ipc::PayloadReader reader(payload.data(), payload.size());
                DWORD error = 0;
                switch (header.type) {
                case content::REQ_CONFIG: { ContentIndexConfig config; if (!content::GetConfig(reader, config) || !index.Configure(config)) error = ERROR_INVALID_PARAMETER; break; }
                case content::REQ_CANCEL: { uint64_t generation = 0; if (reader.GetU64(generation)) { std::lock_guard lock(sessions_mu); cancelled_generations.insert(generation); if (cancelled_generations.size() > 256) cancelled_generations.erase(cancelled_generations.begin()); auto it = sessions.find(generation); if (it != sessions.end()) *it->second = true; } break; }
                case content::REQ_PAUSE: index.Pause(true); break;
                case content::REQ_RESUME: index.Pause(false); break;
                case content::REQ_REBUILD: index.Rebuild(); break;
                case content::REQ_SHUTDOWN: stopping = true; if (accept_thread) CancelSynchronousIo(accept_thread); break;
                case content::REQ_STATUS: break;
                default: error = ERROR_INVALID_PARAMETER; break;
                }
                auto status = index.Status(); if (error) status.error = error;
                ipc::PayloadWriter writer; content::PutStatus(writer, status); content::PutConfig(writer, index.Configuration());
                const auto response = content::Header(content::RSP_STATUS, static_cast<uint32_t>(writer.data().size()));
                WriteAll(pipe, &response, sizeof(response)); WriteAll(pipe, writer.data().data(), static_cast<DWORD>(writer.data().size()));
            }
            FlushFileBuffers(pipe); DisconnectNamedPipe(pipe); CloseHandle(pipe); *done = true;
        }), done});
    }
    stopping = true;
    { std::lock_guard lock(sessions_mu); for (auto& entry : sessions) *entry.second = true; }
    for (auto& handler : handlers) { CancelSynchronousIo(handler.thread.native_handle()); handler.thread.join(); }
    SetEvent(monitor_stop); monitor.join();
    CloseHandle(monitor_stop); CloseHandle(parent); if (accept_thread) CloseHandle(accept_thread);
    return 0;
}
} // namespace pulse::index
