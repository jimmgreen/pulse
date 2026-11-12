// worker_main.cpp - isolated out-of-process 7-Zip worker executable
// (pulse_7zip_worker.exe). Hosts all 7z.dll execution outside pulse.exe.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "engine.h"
#include "worker_ipc.h"
#include "worker_protocol.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace worker_ipc;

constexpr DWORD kWriteTimeoutMs = 15000u;
constexpr DWORD kHandshakeTimeoutMs = 5000u;

struct QueuedRequest {
    FrameHeader header{};
    std::vector<uint8_t> payload;
    ~QueuedRequest() { WipeSensitive(payload); }
    QueuedRequest() = default;
    QueuedRequest(QueuedRequest&&) noexcept = default;
    QueuedRequest& operator=(QueuedRequest&&) noexcept = default;
};

struct WorkerState {
    std::mutex lock;
    std::condition_variable request_cv;
    std::condition_variable password_cv;

    std::optional<QueuedRequest> pending;
    bool busy = false;
    bool shutdown = false;
    bool protocol_error = false;
    std::atomic<bool> stop_reader{false};

    uint64_t active_request_id = 0;
    uint64_t active_generation = 0;
    std::atomic<bool> cancelled{false};

    bool awaiting_password = false;
    bool password_ready = false;
    bool password_ok = false;
    std::wstring password_value;

    ~WorkerState() { WipeSensitive(password_value); }
};

bool IsRequestFrame(FrameType type, bool allow_faults) noexcept {
    switch (type) {
    case FrameType::ReqFormats:
    case FrameType::ReqList:
    case FrameType::ReqExtract:
    case FrameType::ReqCreate:
    case FrameType::ReqHash:
        return true;
    case FrameType::ReqFaultCrashAv:
    case FrameType::ReqFaultHang:
    case FrameType::ReqFaultExceedMemory:
    case FrameType::ReqFaultOversizeFrame:
    case FrameType::ReqFaultWrongGeneration:
        return allow_faults;
    default:
        return false;
    }
}

void ControlReaderLoop(PipeChannel& channel, WorkerState& state, bool allow_faults) {
    for (;;) {
        FrameHeader hdr{};
        std::vector<uint8_t> payload;
        const auto io = channel.ReadFrame(
            hdr, payload, nullptr,
            [&] { return state.stop_reader.load(std::memory_order_acquire); },
            INFINITE);
        if (io != IoStatus::Ok) {
            std::lock_guard guard(state.lock);
            state.shutdown = true;
            state.cancelled.store(true, std::memory_order_release);
            state.request_cv.notify_all();
            state.password_cv.notify_all();
            return;
        }

        const auto type = static_cast<FrameType>(hdr.type);
        if (type == FrameType::MsgShutdown) {
            WipeSensitive(payload);
            std::lock_guard guard(state.lock);
            state.shutdown = true;
            state.cancelled.store(true, std::memory_order_release);
            state.request_cv.notify_all();
            state.password_cv.notify_all();
            return;
        }
        if (type == FrameType::MsgCancel) {
            WipeSensitive(payload);
            std::lock_guard guard(state.lock);
            if (state.busy && hdr.request_id == state.active_request_id && hdr.generation == state.active_generation) {
                state.cancelled.store(true, std::memory_order_release);
                state.password_cv.notify_all();
            }
            continue;
        }
        if (type == FrameType::MsgPasswordReply) {
            ByteReader reader(payload);
            bool ok = false;
            std::wstring pw;
            const bool parsed = reader.Bool(ok) && reader.WStr(pw) && reader.Empty();
            WipeSensitive(payload);
            if (!parsed) {
                WipeSensitive(pw);
                std::lock_guard guard(state.lock);
                state.protocol_error = true;
                state.shutdown = true;
                state.cancelled.store(true, std::memory_order_release);
                state.request_cv.notify_all();
                state.password_cv.notify_all();
                return;
            }
            std::lock_guard guard(state.lock);
            if (state.busy && state.awaiting_password &&
                hdr.request_id == state.active_request_id && hdr.generation == state.active_generation) {
                state.password_ok = ok;
                WipeSensitive(state.password_value);
                state.password_value = std::move(pw);
                state.password_ready = true;
                state.password_cv.notify_all();
            } else {
                WipeSensitive(pw);
            }
            continue;
        }
        if (IsRequestFrame(type, allow_faults)) {
            std::lock_guard guard(state.lock);
            if (state.busy || state.pending.has_value() || hdr.request_id == 0 || hdr.generation == 0) {
                WipeSensitive(payload);
                state.protocol_error = true;
                state.shutdown = true;
                state.cancelled.store(true, std::memory_order_release);
                state.request_cv.notify_all();
                state.password_cv.notify_all();
                return;
            }
            QueuedRequest req;
            req.header = hdr;
            req.payload = std::move(payload);
            state.pending.emplace(std::move(req));
            state.request_cv.notify_one();
            continue;
        }

        WipeSensitive(payload);
        std::lock_guard guard(state.lock);
        state.protocol_error = true;
        state.shutdown = true;
        state.cancelled.store(true, std::memory_order_release);
        state.request_cv.notify_all();
        state.password_cv.notify_all();
        return;
    }
}

engine::Report MakeProgressSender(PipeChannel& channel, WorkerState& state, uint64_t req_id, uint64_t gen) {
    auto last_tick = std::make_shared<ULONGLONG>(0);
    auto last_items = std::make_shared<uint64_t>(UINT64_MAX);
    return [&channel, &state, req_id, gen, last_tick, last_items](const engine::Progress& value) {
        if (state.cancelled.load(std::memory_order_acquire)) return;
        const ULONGLONG now = GetTickCount64();
        const bool milestone = value.completed_items != *last_items ||
            (value.total_bytes > 0 && value.completed_bytes >= value.total_bytes);
        if (!milestone && now - *last_tick < 25) return;
        *last_tick = now;
        *last_items = value.completed_items;

        ByteWriter writer;
        if (!writer.Progress(value)) return;
        if (channel.WriteFrame(FrameType::EvtProgress, req_id, gen, writer.Bytes(), nullptr, kWriteTimeoutMs) != IoStatus::Ok) {
            state.cancelled.store(true, std::memory_order_release);
        }
    };
}

engine::AskPassword MakePasswordRequester(PipeChannel& channel, WorkerState& state, uint64_t req_id, uint64_t gen) {
    return [&channel, &state, req_id, gen](bool retry, std::wstring& out) -> bool {
        if (state.cancelled.load(std::memory_order_acquire)) return false;
        {
            std::lock_guard guard(state.lock);
            if (state.shutdown || state.cancelled.load(std::memory_order_acquire)) return false;
            state.awaiting_password = true;
            state.password_ready = false;
            state.password_ok = false;
            WipeSensitive(state.password_value);
        }

        ByteWriter writer;
        writer.Bool(retry);
        if (channel.WriteFrame(FrameType::EvtNeedPassword, req_id, gen, writer.Bytes(), nullptr, kWriteTimeoutMs) != IoStatus::Ok) {
            std::lock_guard guard(state.lock);
            state.awaiting_password = false;
            state.cancelled.store(true, std::memory_order_release);
            return false;
        }

        std::unique_lock lock(state.lock);
        state.password_cv.wait(lock, [&] {
            return state.password_ready || state.shutdown || state.cancelled.load(std::memory_order_acquire);
        });
        state.awaiting_password = false;
        if (!state.password_ready || state.shutdown || state.cancelled.load(std::memory_order_acquire) || !state.password_ok) {
            WipeSensitive(state.password_value);
            return false;
        }
        out = std::move(state.password_value);
        WipeSensitive(state.password_value);
        return true;
    };
}

#pragma warning(push)
#pragma warning(disable : 6011)  // Deliberate null-pointer dereference in fault-injection test mode.
__declspec(noinline) void TriggerDeliberateAccessViolation() {
    volatile int* null_ptr = nullptr;
    *null_ptr = 0x7A11DEAD;
}
#pragma warning(pop)

bool ExecuteFaultRequest(PipeChannel& channel, const QueuedRequest& req) {
    const auto type = static_cast<FrameType>(req.header.type);
    const uint64_t req_id = req.header.request_id;
    const uint64_t gen = req.header.generation;
    ByteReader reader(req.payload);

    if (type == FrameType::ReqFaultCrashAv) {
        TriggerDeliberateAccessViolation();
        TerminateProcess(GetCurrentProcess(), 0xC0000005u);
        return false;
    }
    if (type == FrameType::ReqFaultHang) {
        std::wstring lock_file_path;
        if (!reader.WStr(lock_file_path) || !reader.Empty()) return false;
        HANDLE held_file = INVALID_HANDLE_VALUE;
        if (!lock_file_path.empty()) {
            held_file = CreateFileW(lock_file_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    0,  // Exclusive lock: host cannot delete until worker is terminated!
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        (void)held_file;
        // Deliberately ignore cooperative cancellation to prove hard host timeout / TerminateJobObject.
        for (;;) {
            Sleep(1000);
        }
    }
    if (type == FrameType::ReqFaultExceedMemory) {
        uint64_t target_bytes = 0;
        if (!reader.U64(target_bytes) || !reader.Empty()) return false;
        const size_t chunk_size = 4u * 1024u * 1024u;
        std::vector<void*> chunks;
        uint64_t allocated = 0;
        bool failed = false;
        while (allocated < target_bytes) {
            void* mem = VirtualAlloc(nullptr, chunk_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!mem) {
                failed = true;
                break;
            }
            std::memset(mem, 0x5A, chunk_size);
            chunks.push_back(mem);
            allocated += chunk_size;
        }
        for (void* mem : chunks) VirtualFree(mem, 0, MEM_RELEASE);
        ByteWriter writer;
        writer.U32(static_cast<uint32_t>(failed ? engine::Status::WorkerMemoryLimit : engine::Status::Ok));
        writer.Str(failed ? "Job Object denied process memory allocation" : "Allocated within limit");
        return channel.WriteFrame(FrameType::RespFault, req_id, gen, writer.Bytes(), nullptr, kWriteTimeoutMs) == IoStatus::Ok;
    }
    if (type == FrameType::ReqFaultOversizeFrame) {
        FrameHeader bad{};
        bad.magic = kProtocolMagic;
        bad.version = kProtocolVersion;
        bad.type = static_cast<uint16_t>(FrameType::RespFault);
        bad.request_id = req_id;
        bad.generation = gen;
        bad.payload_size = kMaxFrameBytes + 4096u;
        bad.reserved = 0;
        channel.WriteRawHeaderOnly(bad, nullptr, kWriteTimeoutMs);
        return false;
    }
    if (type == FrameType::ReqFaultWrongGeneration) {
        ByteWriter writer;
        writer.U32(static_cast<uint32_t>(engine::Status::Ok));
        writer.Str("stale generation response");
        channel.WriteFrame(FrameType::RespFault, req_id, gen + 1u, writer.Bytes(), nullptr, kWriteTimeoutMs);
        return false;
    }
    return false;
}

bool HandleRequest(PipeChannel& channel, WorkerState& state, QueuedRequest& req, bool allow_faults) {
    const auto type = static_cast<FrameType>(req.header.type);
    const uint64_t req_id = req.header.request_id;
    const uint64_t gen = req.header.generation;
    const engine::Cancelled cancelled = [&state] { return state.cancelled.load(std::memory_order_acquire); };

    if (allow_faults && static_cast<uint16_t>(type) >= 200u) {
        return ExecuteFaultRequest(channel, req);
    }

    ByteReader reader(req.payload);

    if (type == FrameType::ReqFormats) {
        if (!reader.Empty()) return false;
        ByteWriter writer;
        try {
            const auto formats = engine::SupportedFormats();
            if (!writer.U32(static_cast<uint32_t>(engine::Status::Ok)) || !writer.WStrVec(formats)) return false;
        } catch (const std::bad_alloc&) {
            ByteWriter oom;
            oom.U32(static_cast<uint32_t>(engine::Status::WorkerMemoryLimit));
            oom.WStrVec({});
            return channel.WriteFrame(FrameType::RespFormats, req_id, gen, oom.Bytes(), nullptr, kWriteTimeoutMs) == IoStatus::Ok;
        }
        return channel.WriteFrame(FrameType::RespFormats, req_id, gen, writer.Bytes(), nullptr, kWriteTimeoutMs) == IoStatus::Ok;
    }

    if (type == FrameType::ReqList) {
        std::wstring path;
        uint64_t max_entries = 0;
        engine::Budget budget{};
        bool has_password_cb = false;
        if (!reader.WStr(path) || !reader.U64(max_entries) || !reader.Budget(budget) ||
            !reader.Bool(has_password_cb) || !reader.Empty()) {
            return false;
        }
        engine::Listing listing;
        try {
            const auto ask = has_password_cb ? MakePasswordRequester(channel, state, req_id, gen) : engine::AskPassword{};
            listing = engine::List(path, cancelled, ask, static_cast<size_t>(max_entries), budget);
        } catch (const std::bad_alloc&) {
            listing.status = engine::Status::WorkerMemoryLimit;
            listing.error = "Worker hit process memory limit";
            listing.entries.clear();
        }
        ByteWriter writer;
        bool ok = writer.U32(static_cast<uint32_t>(listing.status)) &&
                  writer.Str(listing.error) &&
                  writer.WStr(listing.format) &&
                  writer.Bool(listing.truncated) &&
                  writer.Bool(listing.nested) &&
                  writer.U32(static_cast<uint32_t>(listing.entries.size()));
        for (const auto& entry : listing.entries) {
            if (!ok) break;
            ok = writer.WStr(entry.path) &&
                 writer.U64(entry.size) &&
                 writer.U64(entry.packed) &&
                 writer.U64(entry.modified) &&
                 writer.U32(entry.attributes) &&
                 writer.Bool(entry.dir) &&
                 writer.Bool(entry.encrypted) &&
                 writer.Bool(entry.link);
        }
        if (!ok) {
            ByteWriter cap;
            cap.U32(static_cast<uint32_t>(engine::Status::LimitReached));
            cap.Str("Archive entry listing exceeds IPC frame cap");
            cap.WStr(listing.format);
            cap.Bool(true);
            cap.Bool(listing.nested);
            cap.U32(0);
            return channel.WriteFrame(FrameType::RespList, req_id, gen, cap.Bytes(), nullptr, kWriteTimeoutMs) == IoStatus::Ok;
        }
        return channel.WriteFrame(FrameType::RespList, req_id, gen, writer.Bytes(), nullptr, kWriteTimeoutMs) == IoStatus::Ok;
    }

    if (type == FrameType::ReqExtract) {
        std::wstring path, destination;
        bool test_only = false, has_report = false, has_password_cb = false;
        engine::Budget budget{};
        if (!reader.WStr(path) || !reader.WStr(destination) || !reader.Bool(test_only) ||
            !reader.Budget(budget) || !reader.Bool(has_report) || !reader.Bool(has_password_cb) || !reader.Empty()) {
            return false;
        }
        engine::ExtractResult result;
        try {
            const auto report = has_report ? MakeProgressSender(channel, state, req_id, gen) : engine::Report{};
            const auto ask = has_password_cb ? MakePasswordRequester(channel, state, req_id, gen) : engine::AskPassword{};
            result = engine::Extract(path, destination, test_only, cancelled, report, ask, budget);
        } catch (const std::bad_alloc&) {
            result.status = engine::Status::WorkerMemoryLimit;
            result.error = "Worker hit process memory limit";
        }
        ByteWriter writer;
        if (!writer.U32(static_cast<uint32_t>(result.status)) ||
            !writer.Str(result.error) ||
            !writer.U64(result.files) ||
            !writer.U64(result.bytes) ||
            !writer.U64(result.skipped_links)) {
            return false;
        }
        return channel.WriteFrame(FrameType::RespExtract, req_id, gen, writer.Bytes(), nullptr, kWriteTimeoutMs) == IoStatus::Ok;
    }

    if (type == FrameType::ReqCreate) {
        std::vector<std::wstring> sources;
        std::wstring destination, name;
        uint32_t format_raw = 0, level = 0;
        bool solid = true, encrypt_names = false, zip_aes = true, has_report = false;
        uint64_t volume_size = 0;
        engine::CreateOptions options;
        struct OptionsWipe {
            std::wstring& pw;
            ~OptionsWipe() { WipeSensitive(pw); }
        } wipe_pw{options.password};

        const bool parsed = reader.WStrVec(sources) &&
                            reader.WStr(destination) &&
                            reader.WStr(name) &&
                            reader.U32(format_raw) &&
                            format_raw <= static_cast<uint32_t>(engine::Format::TarBz2) &&
                            reader.U32(level) &&
                            reader.Bool(solid) &&
                            reader.WStr(options.password) &&
                            reader.Bool(encrypt_names) &&
                            reader.Bool(zip_aes) &&
                            reader.U64(volume_size) &&
                            reader.Bool(has_report) &&
                            reader.Empty();
        WipeSensitive(req.payload);
        if (!parsed) return false;

        options.format = static_cast<engine::Format>(format_raw);
        options.level = level;
        options.solid = solid;
        options.encrypt_names = encrypt_names;
        options.zip_aes = zip_aes;
        options.volume_size = volume_size;

        engine::CreateResult result;
        try {
            const auto report = has_report ? MakeProgressSender(channel, state, req_id, gen) : engine::Report{};
            result = engine::Create(sources, destination, name, options, cancelled, report);
        } catch (const std::bad_alloc&) {
            result.status = engine::Status::WorkerMemoryLimit;
            result.error = "Worker hit process memory limit";
        }
        ByteWriter writer;
        if (!writer.U32(static_cast<uint32_t>(result.status)) ||
            !writer.Str(result.error) ||
            !writer.WStrVec(result.outputs) ||
            !writer.U64(result.files) ||
            !writer.U64(result.bytes) ||
            !writer.U64(result.skipped)) {
            return false;
        }
        return channel.WriteFrame(FrameType::RespCreate, req_id, gen, writer.Bytes(), nullptr, kWriteTimeoutMs) == IoStatus::Ok;
    }

    if (type == FrameType::ReqHash) {
        std::vector<std::wstring> files;
        bool has_report = false;
        if (!reader.WStrVec(files) || !reader.Bool(has_report) || !reader.Empty()) return false;
        std::vector<engine::HashResult> results;
        engine::Status status = engine::Status::Ok;
        try {
            const auto report = has_report ? MakeProgressSender(channel, state, req_id, gen) : engine::Report{};
            results = engine::Hash(files, cancelled, report);
            if (cancelled()) status = engine::Status::Cancelled;
        } catch (const std::bad_alloc&) {
            status = engine::Status::WorkerMemoryLimit;
            results.clear();
        }
        ByteWriter writer;
        bool ok = writer.U32(static_cast<uint32_t>(status)) &&
                  writer.U32(static_cast<uint32_t>(results.size()));
        for (const auto& item : results) {
            if (!ok) break;
            ok = writer.WStr(item.path) &&
                 writer.U64(item.size) &&
                 writer.Str(item.crc32) &&
                 writer.Str(item.sha1) &&
                 writer.Str(item.sha256) &&
                 writer.Str(item.error);
        }
        if (!ok) return false;
        return channel.WriteFrame(FrameType::RespHash, req_id, gen, writer.Bytes(), nullptr, kWriteTimeoutMs) == IoStatus::Ok;
    }

    return false;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    std::wstring pipe_name;
    bool allow_faults = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--pipe" && i + 1 < argc) {
            pipe_name = argv[++i];
        } else if (arg == L"--allow-fault-injection") {
            allow_faults = true;
        } else {
            return 2;
        }
    }
    if (pipe_name.rfind(L"\\\\.\\pipe\\Pulse.7ZipWorker.", 0) != 0) {
        return 2;
    }

    HANDLE raw_pipe = ConnectWorkerClientPipe(pipe_name);
    if (raw_pipe == INVALID_HANDLE_VALUE) {
        return 3;
    }
    PipeChannel channel(raw_pipe);

    // Authenticate session handshake.
    FrameHeader hello_hdr{};
    std::vector<uint8_t> hello_payload;
    if (channel.ReadFrame(hello_hdr, hello_payload, nullptr, {}, kHandshakeTimeoutMs) != IoStatus::Ok ||
        static_cast<FrameType>(hello_hdr.type) != FrameType::Hello ||
        hello_hdr.request_id != 0 || hello_hdr.generation != 0 ||
        hello_payload.size() != kSessionNonceBytes) {
        return 4;
    }

    ByteWriter ack;
    ack.Raw(hello_payload.data(), kSessionNonceBytes);
    ack.U32(GetCurrentProcessId());
    if (channel.WriteFrame(FrameType::HelloAck, 0, 0, ack.Bytes(), nullptr, kHandshakeTimeoutMs) != IoStatus::Ok) {
        return 5;
    }

    WorkerState state;
    std::thread reader([&channel, &state, allow_faults] { ControlReaderLoop(channel, state, allow_faults); });

    for (;;) {
        QueuedRequest req;
        {
            std::unique_lock lock(state.lock);
            state.request_cv.wait(lock, [&] { return state.pending.has_value() || state.shutdown; });
            if (state.shutdown && !state.pending.has_value()) break;
            req = std::move(*state.pending);
            state.pending.reset();
            state.busy = true;
            state.active_request_id = req.header.request_id;
            state.active_generation = req.header.generation;
            state.cancelled.store(false, std::memory_order_release);
            state.awaiting_password = false;
            state.password_ready = false;
            WipeSensitive(state.password_value);
        }

        const bool ok = HandleRequest(channel, state, req, allow_faults);

        {
            std::lock_guard guard(state.lock);
            state.busy = false;
            state.active_request_id = 0;
            state.active_generation = 0;
            state.awaiting_password = false;
            WipeSensitive(state.password_value);
            if (!ok) {
                state.shutdown = true;
                break;
            }
        }
    }

    state.stop_reader.store(true, std::memory_order_release);
    CancelIoEx(channel.NativeHandle(), nullptr);
    if (reader.joinable()) reader.join();
    channel.Close();
    return state.protocol_error ? 6 : 0;
}
