#include "content_search_client.h"
#include "content_search_protocol.h"

#include <objbase.h>
#include <algorithm>
#include <chrono>

namespace pulse::index {
namespace {

std::wstring NewToken() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        return std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64());
    }
    wchar_t text[40]{};
    StringFromGUID2(guid, text, ARRAYSIZE(text));
    std::wstring token(text);
    token.erase(std::remove_if(token.begin(), token.end(), [](wchar_t value) {
        return value == L'{' || value == L'}';
    }), token.end());
    return token;
}

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

std::vector<uint8_t> RequestPayload(const ContentSearchRequest& request) {
    ipc::PayloadWriter writer;
    writer.PutU64(request.generation);
    writer.PutU32(static_cast<uint32_t>(request.mode));
    uint32_t flags = request.recursive ? 1u : 0u;
    if (request.case_sensitive) flags |= 2u;
    writer.PutU32(flags);
    writer.PutU64(request.maximum_file_bytes);
    writer.PutU32(static_cast<uint32_t>(request.maximum_hits));
    writer.PutString(request.root);
    writer.PutString(request.needle);
    return writer.data();
}

bool ParseUpdate(const std::vector<uint8_t>& payload, ContentSearchUpdate& update) {
    ipc::PayloadReader reader(payload.data(), payload.size());
    uint32_t flags = 0;
    uint32_t error = 0;
    uint32_t count = 0;
    if (!reader.GetU64(update.progress.generation) ||
        !reader.GetU64(update.progress.scanned_files) ||
        !reader.GetU64(update.progress.scanned_bytes) ||
        !reader.GetU32(flags) || !reader.GetU32(error) || !reader.GetU32(count) ||
        count > 10000) return false;
    update.progress.done = (flags & 1u) != 0;
    update.progress.truncated = (flags & 2u) != 0;
    update.progress.error = error;
    update.hits.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ContentHit hit;
        if (!reader.GetString(hit.path) || !reader.GetString(hit.name) ||
            !reader.GetString(hit.snippet) || !reader.GetU64(hit.size) ||
            !reader.GetU64(hit.modified) || !reader.GetU32(hit.line) ||
            !reader.GetU32(hit.group)) return false;
        update.hits.push_back(std::move(hit));
    }
    return true;
}

} // namespace

std::wstring ContentSearchClient::ExePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return L"Pulse.Index.exe";
    path.resize(length);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"Pulse.Index.exe"
        : path.substr(0, slash + 1) + L"Pulse.Index.exe";
}

void ContentSearchClient::Start(HWND notify, UINT update_message) {
    Stop();
    notify_ = notify;
    update_message_ = update_message;
    running_ = true;
}

void ContentSearchClient::Stop() {
    running_ = false;
    Cancel();
    std::lock_guard<std::mutex> lock(updates_mu_);
    updates_.clear();
}

void ContentSearchClient::Cancel() {
    generation_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(process_mu_);
        if (process_) TerminateProcess(process_, ERROR_CANCELLED);
    }
    if (worker_.joinable()) worker_.join();
}

void ContentSearchClient::SearchAsync(ContentSearchRequest request) {
    if (!running_) return;
    Cancel();
    generation_.store(request.generation);
    {
        std::lock_guard<std::mutex> lock(updates_mu_);
        updates_.clear();
    }
    worker_ = std::thread([this, request = std::move(request)] { Run(request); });
}

void ContentSearchClient::Publish(ContentSearchUpdate update) {
    if (!running_ || update.progress.generation != generation_.load()) return;
    {
        std::lock_guard<std::mutex> lock(updates_mu_);
        updates_.push_back(std::move(update));
    }
    if (notify_ && update_message_) PostMessageW(notify_, update_message_, 0, 0);
}

bool ContentSearchClient::TakeUpdate(ContentSearchUpdate& update) {
    std::lock_guard<std::mutex> lock(updates_mu_);
    if (updates_.empty()) return false;
    update = std::move(updates_.front());
    updates_.pop_front();
    return true;
}

void ContentSearchClient::Run(ContentSearchRequest request) {
    ContentSearchUpdate failure;
    failure.progress.generation = request.generation;
    failure.progress.done = true;
    const std::wstring exe = ExePath();
    const std::wstring token = NewToken();
    const std::wstring pipe_name = content::PipeName(token);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION created{};
    std::wstring command = L"\"" + exe + L"\" --content-agent " + token;
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &created)) {
        failure.progress.error = GetLastError();
        Publish(std::move(failure));
        return;
    }
    CloseHandle(created.hThread);
    {
        std::lock_guard<std::mutex> lock(process_mu_);
        process_ = created.hProcess;
    }

    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (running_ && generation_.load() == request.generation &&
           std::chrono::steady_clock::now() < deadline) {
        pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (WaitForSingleObject(created.hProcess, 10) == WAIT_OBJECT_0) break;
        WaitNamedPipeW(pipe_name.c_str(), 50);
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        failure.progress.error = generation_.load() == request.generation
            ? ERROR_TIMEOUT : ERROR_CANCELLED;
        Publish(std::move(failure));
    } else {
        const auto payload = RequestPayload(request);
        const auto header = content::Header(content::REQ_SEARCH,
                                             static_cast<uint32_t>(payload.size()));
        bool ok = WriteAll(pipe, &header, sizeof(header)) &&
                  WriteAll(pipe, payload.data(), static_cast<DWORD>(payload.size()));
        while (ok && running_ && generation_.load() == request.generation) {
            ipc::MsgHeader response{};
            ok = ReadAll(pipe, &response, sizeof(response)) &&
                 response.magic == content::kMagic &&
                 response.type == content::RSP_BATCH &&
                 response.payload_size <= content::kMaximumPayload;
            std::vector<uint8_t> bytes(ok ? response.payload_size : 0);
            if (ok && !bytes.empty()) ok = ReadAll(pipe, bytes.data(), response.payload_size);
            ContentSearchUpdate update;
            if (!ok || !ParseUpdate(bytes, update) ||
                update.progress.generation != request.generation) break;
            const bool done = update.progress.done;
            Publish(std::move(update));
            if (done) break;
        }
        CloseHandle(pipe);
    }

    {
        std::lock_guard<std::mutex> lock(process_mu_);
        if (process_ == created.hProcess) process_ = nullptr;
    }
    if (WaitForSingleObject(created.hProcess, 250) == WAIT_TIMEOUT)
        TerminateProcess(created.hProcess, ERROR_CANCELLED);
    CloseHandle(created.hProcess);
}

} // namespace pulse::index
