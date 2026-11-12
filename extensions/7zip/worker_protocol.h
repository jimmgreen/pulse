// worker_protocol.h - bounded binary wire protocol between pulse_7zip.dll (in
// pulse.exe) and out-of-process pulse_7zip_worker.exe (hosting 7z.dll).
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "engine.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace worker_ipc {

inline constexpr uint32_t kProtocolMagic = 0x375A5750u;  // "PWZ7"
inline constexpr uint16_t kProtocolVersion = 1u;
inline constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;  // 16 MiB strict cap
inline constexpr uint32_t kMaxStringChars = 1u * 1024u * 1024u;
inline constexpr uint32_t kMaxVectorItems = 500000u;
inline constexpr size_t kSessionNonceBytes = 16u;

enum class FrameType : uint16_t {
    Hello = 1,
    HelloAck = 2,

    ReqFormats = 10,
    ReqList = 11,
    ReqExtract = 12,
    ReqCreate = 13,
    ReqHash = 14,

    MsgCancel = 20,
    MsgPasswordReply = 21,
    MsgShutdown = 22,

    EvtProgress = 30,
    EvtNeedPassword = 31,

    RespFormats = 40,
    RespList = 41,
    RespExtract = 42,
    RespCreate = 43,
    RespHash = 44,

    // Fault-injection opcodes accepted ONLY when worker runs with --allow-fault-injection.
    ReqFaultCrashAv = 200,
    ReqFaultHang = 201,
    ReqFaultExceedMemory = 202,
    ReqFaultOversizeFrame = 203,
    ReqFaultWrongGeneration = 204,
    RespFault = 240,
};

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic = kProtocolMagic;
    uint16_t version = kProtocolVersion;
    uint16_t type = 0;
    uint64_t request_id = 0;
    uint64_t generation = 0;
    uint32_t payload_size = 0;
    uint32_t reserved = 0;
};
#pragma pack(pop)
static_assert(sizeof(FrameHeader) == 32, "FrameHeader wire layout must be 32 bytes");

class ByteWriter {
    std::vector<uint8_t> bytes_;
    bool sensitive_ = false;
public:
    explicit ByteWriter(bool sensitive = false) : sensitive_(sensitive) { bytes_.reserve(256); }
    ~ByteWriter() {
        if (sensitive_ && !bytes_.empty()) SecureZeroMemory(bytes_.data(), bytes_.size());
    }
    ByteWriter(const ByteWriter&) = delete;
    ByteWriter& operator=(const ByteWriter&) = delete;

    const std::vector<uint8_t>& Bytes() const noexcept { return bytes_; }

    bool Raw(const void* data, size_t size) {
        if (size > kMaxFrameBytes || bytes_.size() > kMaxFrameBytes - size) return false;
        const auto* ptr = static_cast<const uint8_t*>(data);
        bytes_.insert(bytes_.end(), ptr, ptr + size);
        return true;
    }
    bool U8(uint8_t v) { return Raw(&v, sizeof(v)); }
    bool Bool(bool v) { return U8(v ? 1u : 0u); }
    bool U32(uint32_t v) { return Raw(&v, sizeof(v)); }
    bool U64(uint64_t v) { return Raw(&v, sizeof(v)); }

    bool Str(const std::string& text) {
        if (text.size() > kMaxStringChars) return false;
        const uint32_t len = static_cast<uint32_t>(text.size());
        return U32(len) && (len == 0 || Raw(text.data(), len));
    }
    bool WStr(const std::wstring& text) {
        if (text.size() > kMaxStringChars) return false;
        const uint32_t len = static_cast<uint32_t>(text.size());
        return U32(len) && (len == 0 || Raw(text.data(), static_cast<size_t>(len) * sizeof(wchar_t)));
    }
    bool WStrVec(const std::vector<std::wstring>& items) {
        if (items.size() > kMaxVectorItems) return false;
        if (!U32(static_cast<uint32_t>(items.size()))) return false;
        for (const auto& item : items) {
            if (!WStr(item)) return false;
        }
        return true;
    }
    bool Budget(const engine::Budget& budget) {
        return U64(budget.max_total_bytes) &&
               U64(budget.max_single_bytes) &&
               U64(budget.min_free_disk_bytes) &&
               U64(static_cast<uint64_t>(budget.max_entries)) &&
               U64(static_cast<uint64_t>(budget.max_path_bytes)) &&
               U32(budget.max_duration_ms) &&
               Bool(budget.expand_wrappers);
    }
    bool Progress(const engine::Progress& progress) {
        return WStr(progress.current) &&
               U64(progress.completed_bytes) &&
               U64(progress.total_bytes) &&
               U64(progress.completed_items) &&
               U64(progress.total_items);
    }
};

class ByteReader {
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t offset_ = 0;
public:
    ByteReader(const uint8_t* data, size_t size) noexcept : data_(data), size_(size) {}
    explicit ByteReader(const std::vector<uint8_t>& buffer) noexcept : data_(buffer.data()), size_(buffer.size()) {}

    bool Empty() const noexcept { return offset_ == size_; }
    size_t Remaining() const noexcept { return size_ - offset_; }

    bool Raw(void* out, size_t size) noexcept {
        if (size > size_ - offset_) return false;
        if (size > 0) std::memcpy(out, data_ + offset_, size);
        offset_ += size;
        return true;
    }
    bool U8(uint8_t& out) noexcept { return Raw(&out, sizeof(out)); }
    bool Bool(bool& out) noexcept {
        uint8_t raw = 0;
        if (!U8(raw) || raw > 1u) return false;
        out = raw != 0;
        return true;
    }
    bool U32(uint32_t& out) noexcept { return Raw(&out, sizeof(out)); }
    bool U64(uint64_t& out) noexcept { return Raw(&out, sizeof(out)); }

    bool Str(std::string& out) {
        uint32_t len = 0;
        if (!U32(len) || len > kMaxStringChars || len > Remaining()) return false;
        out.assign(reinterpret_cast<const char*>(data_ + offset_), len);
        offset_ += len;
        return true;
    }
    bool WStr(std::wstring& out) {
        uint32_t len = 0;
        if (!U32(len) || len > kMaxStringChars) return false;
        const size_t byte_count = static_cast<size_t>(len) * sizeof(wchar_t);
        if (byte_count > Remaining()) return false;
        out.resize(len);
        if (byte_count > 0) std::memcpy(out.data(), data_ + offset_, byte_count);
        offset_ += byte_count;
        return true;
    }
    bool WStrVec(std::vector<std::wstring>& out) {
        uint32_t count = 0;
        if (!U32(count) || count > kMaxVectorItems) return false;
        // Guard against amplification: each element requires at least 4 length bytes in payload.
        if (static_cast<size_t>(count) * sizeof(uint32_t) > Remaining()) return false;
        out.clear();
        out.reserve(std::min<size_t>(count, 4096u));
        for (uint32_t i = 0; i < count; ++i) {
            std::wstring item;
            if (!WStr(item)) return false;
            out.push_back(std::move(item));
        }
        return true;
    }
    bool Status(engine::Status& out) noexcept {
        uint32_t code = 0;
        if (!U32(code) || code > static_cast<uint32_t>(engine::Status::WorkerMemoryLimit)) return false;
        out = static_cast<engine::Status>(code);
        return true;
    }
    bool Budget(engine::Budget& out) noexcept {
        uint64_t max_entries = 0, max_path_bytes = 0;
        if (!U64(out.max_total_bytes) ||
            !U64(out.max_single_bytes) ||
            !U64(out.min_free_disk_bytes) ||
            !U64(max_entries) ||
            !U64(max_path_bytes) ||
            !U32(out.max_duration_ms) ||
            !Bool(out.expand_wrappers)) {
            return false;
        }
        out.max_entries = static_cast<size_t>(max_entries);
        out.max_path_bytes = static_cast<size_t>(max_path_bytes);
        return true;
    }
    bool Progress(engine::Progress& out) {
        return WStr(out.current) &&
               U64(out.completed_bytes) &&
               U64(out.total_bytes) &&
               U64(out.completed_items) &&
               U64(out.total_items);
    }
};

inline void WipeSensitive(std::wstring& secret) noexcept {
    if (!secret.empty()) {
        SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));
        secret.clear();
    }
}

inline void WipeSensitive(std::vector<uint8_t>& buffer) noexcept {
    if (!buffer.empty()) {
        SecureZeroMemory(buffer.data(), buffer.size());
        buffer.clear();
    }
}

} // namespace worker_ipc
