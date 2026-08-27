#include "content_agent.h"
#include "content_search.h"
#include "content_search_protocol.h"
#include "../common/current_user_security.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <vector>

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
        request.root.empty() || request.root.size() > 32768 || request.needle.size() > 4096 ||
        request.maximum_file_bytes > 64ull * 1024ull * 1024ull ||
        maximum_hits == 0 || maximum_hits > 10000) return false;
    request.mode = static_cast<ContentSearchMode>(mode);
    request.recursive = (flags & 1) != 0;
    request.case_sensitive = (flags & 2) != 0;
    request.maximum_hits = maximum_hits;
    return true;
}

bool SendBatch(HANDLE pipe, const ContentSearchProgress& progress,
               const std::vector<ContentHit>& hits) {
    ipc::PayloadWriter writer;
    writer.PutU64(progress.generation);
    writer.PutU64(progress.scanned_files);
    writer.PutU64(progress.scanned_bytes);
    uint32_t flags = progress.done ? 1u : 0u;
    if (progress.truncated) flags |= 2u;
    writer.PutU32(flags);
    writer.PutU32(progress.error);
    writer.PutU32(static_cast<uint32_t>(hits.size()));
    for (const auto& hit : hits) {
        writer.PutString(hit.path);
        writer.PutString(hit.name);
        writer.PutString(hit.snippet);
        writer.PutU64(hit.size);
        writer.PutU64(hit.modified);
        writer.PutU32(hit.line);
        writer.PutU32(hit.group);
    }
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

} // namespace pulse::index
