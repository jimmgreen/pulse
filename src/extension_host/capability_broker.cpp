#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#if defined(WINVER) && WINVER < 0x0A00
#undef WINVER
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include "capability_broker.h"

#include <aclapi.h>
#include <algorithm>
#include <bcrypt.h>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <sddl.h>
#include <userenv.h>
#include <utility>

namespace pulse::extension_host {
namespace {

class UniqueHandle {
  public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    UniqueHandle(UniqueHandle &&other) noexcept : handle_(other.Release()) {}
    UniqueHandle &operator=(UniqueHandle &&other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    HANDLE Get() const noexcept { return handle_; }
    HANDLE *Put() noexcept {
        Reset();
        return &handle_;
    }
    bool Valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE Release() noexcept {
        HANDLE out = handle_;
        handle_ = nullptr;
        return out;
    }
    void Reset(HANDLE next = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = next;
    }

  private:
    HANDLE handle_ = nullptr;
};

class ProcThreadAttributeBag {
  public:
    ~ProcThreadAttributeBag() {
        if (list != nullptr) {
            DeleteProcThreadAttributeList(list);
        }
    }

    bool Initialize(DWORD attribute_count) {
        SIZE_T bytes_needed = 0;
        InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &bytes_needed);
        if (bytes_needed == 0) {
            return false;
        }
        storage_.resize(bytes_needed);
        auto *candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(candidate, attribute_count, 0, &bytes_needed)) {
            return false;
        }
        list = candidate;
        return true;
    }

    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;

  private:
    std::vector<uint8_t> storage_;
};

// Load userenv.dll dynamically on the host only so child worker binaries never carry
// userenv/user32 static imports into Low-Integrity or Restricted-Token sandboxes.
class DynamicUserenvApi {
  public:
    DynamicUserenvApi()
        : module_(LoadLibraryExW(L"userenv.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (module_ != nullptr) {
            Resolve(create_profile, "CreateAppContainerProfile");
            Resolve(delete_profile, "DeleteAppContainerProfile");
            Resolve(derive_sid, "DeriveAppContainerSidFromAppContainerName");
        } else {
            load_error = GetLastError();
        }
    }
    ~DynamicUserenvApi() {
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }
    DynamicUserenvApi(const DynamicUserenvApi &) = delete;
    DynamicUserenvApi &operator=(const DynamicUserenvApi &) = delete;

    decltype(&CreateAppContainerProfile) create_profile = nullptr;
    decltype(&DeleteAppContainerProfile) delete_profile = nullptr;
    decltype(&DeriveAppContainerSidFromAppContainerName) derive_sid = nullptr;
    DWORD load_error = ERROR_SUCCESS;

  private:
    template <typename Fn>
    void Resolve(Fn &out_fn, const char *symbol) {
        const FARPROC proc = GetProcAddress(module_, symbol);
        if (proc == nullptr && load_error == ERROR_SUCCESS) {
            load_error = GetLastError();
        }
        static_assert(sizeof(out_fn) == sizeof(proc));
        std::memcpy(&out_fn, &proc, sizeof(out_fn));
    }

    HMODULE module_ = nullptr;
};

bool GenerateRandomBytes(uint8_t *buffer, size_t size) {
    if (size > static_cast<size_t>((std::numeric_limits<ULONG>::max)())) {
        return false;
    }
    const NTSTATUS status = BCryptGenRandom(
        nullptr, buffer, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0;
}

std::wstring BytesToHexW(const uint8_t *bytes, size_t count) {
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(kHex[(bytes[i] >> 4) & 0x0Fu]);
        out.push_back(kHex[bytes[i] & 0x0Fu]);
    }
    return out;
}

bool HexToBytesW(const std::wstring &hex, uint8_t *out, size_t expected_bytes) {
    if (hex.size() != expected_bytes * 2) {
        return false;
    }
    auto nibble = [](wchar_t ch, uint8_t &val) -> bool {
        if (ch >= L'0' && ch <= L'9') {
            val = static_cast<uint8_t>(ch - L'0');
            return true;
        }
        if (ch >= L'a' && ch <= L'f') {
            val = static_cast<uint8_t>(10 + (ch - L'a'));
            return true;
        }
        if (ch >= L'A' && ch <= L'F') {
            val = static_cast<uint8_t>(10 + (ch - L'A'));
            return true;
        }
        return false;
    };
    for (size_t i = 0; i < expected_bytes; ++i) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!nibble(hex[i * 2], hi) || !nibble(hex[i * 2 + 1], lo)) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool ConstantTimeEquals(const uint8_t *lhs, const uint8_t *rhs, size_t len) noexcept {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= static_cast<uint8_t>(lhs[i] ^ rhs[i]);
    }
    return diff == 0;
}

std::wstring QuoteArg(const std::wstring &arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring quoted = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(*it);
        }
    }
    quoted.push_back(L'"');
    return quoted;
}

class ByteWriter {
  public:
    void WriteU32(uint32_t value) {
        const size_t pos = bytes_.size();
        bytes_.resize(pos + sizeof(value));
        std::memcpy(bytes_.data() + pos, &value, sizeof(value));
    }

    void WriteU64(uint64_t value) {
        const size_t pos = bytes_.size();
        bytes_.resize(pos + sizeof(value));
        std::memcpy(bytes_.data() + pos, &value, sizeof(value));
    }

    bool WriteBytes(const void *data, size_t len) {
        if (len > kMaxBrokerPayloadBytes) {
            return false;
        }
        WriteU32(static_cast<uint32_t>(len));
        if (len > 0) {
            const size_t pos = bytes_.size();
            bytes_.resize(pos + len);
            std::memcpy(bytes_.data() + pos, data, len);
        }
        return true;
    }

    bool WriteString(const std::string &text) {
        return WriteBytes(text.data(), text.size());
    }

    bool WriteWString(const std::wstring &text) {
        if (text.size() > kMaxBrokerPayloadBytes / sizeof(wchar_t)) {
            return false;
        }
        WriteU32(static_cast<uint32_t>(text.size()));
        if (!text.empty()) {
            const size_t byte_len = text.size() * sizeof(wchar_t);
            const size_t pos = bytes_.size();
            bytes_.resize(pos + byte_len);
            std::memcpy(bytes_.data() + pos, text.data(), byte_len);
        }
        return true;
    }

    std::vector<uint8_t> Take() noexcept { return std::move(bytes_); }

  private:
    std::vector<uint8_t> bytes_;
};

class ByteReader {
  public:
    explicit ByteReader(const std::vector<uint8_t> &bytes) noexcept
        : data_(bytes.data()), size_(bytes.size()) {}

    bool ReadU32(uint32_t &out) noexcept {
        if ( size_ - offset_ < sizeof(out)) {
            return false;
        }
        std::memcpy(&out, data_ + offset_, sizeof(out));
        offset_ += sizeof(out);
        return true;
    }

    bool ReadU64(uint64_t &out) noexcept {
        if (size_ - offset_ < sizeof(out)) {
            return false;
        }
        std::memcpy(&out, data_ + offset_, sizeof(out));
        offset_ += sizeof(out);
        return true;
    }

    bool ReadBytes(std::vector<uint8_t> &out) {
        uint32_t len = 0;
        if (!ReadU32(len) || size_ - offset_ < len) {
            return false;
        }
        out.assign(data_ + offset_, data_ + offset_ + len);
        offset_ += len;
        return true;
    }

    bool ReadString(std::string &out) {
        uint32_t len = 0;
        if (!ReadU32(len) || size_ - offset_ < len) {
            return false;
        }
        out.assign(reinterpret_cast<const char *>(data_ + offset_), len);
        offset_ += len;
        return true;
    }

    bool ReadWString(std::wstring &out) {
        uint32_t char_count = 0;
        if (!ReadU32(char_count)) {
            return false;
        }
        const size_t byte_len = static_cast<size_t>(char_count) * sizeof(wchar_t);
        if (char_count > kMaxBrokerPayloadBytes / sizeof(wchar_t) || size_ - offset_ < byte_len) {
            return false;
        }
        out.resize(char_count);
        if (byte_len > 0) {
            std::memcpy(out.data(), data_ + offset_, byte_len);
            offset_ += byte_len;
        }
        return true;
    }

    bool Empty() const noexcept { return offset_ == size_; }

  private:
    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
    size_t offset_ = 0;
};

enum class PipeIoStatus : uint8_t {
    Ok = 0,
    Canceled = 1,
    Timeout = 2,
    Broken = 3,
};

PipeIoStatus ReadExactOverlapped(HANDLE pipe, HANDLE io_event, HANDLE cancel_event, void *buffer,
                                 size_t byte_count, ULONGLONG deadline_tick) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || io_event == nullptr) {
        return PipeIoStatus::Broken;
    }
    auto *out = static_cast<uint8_t *>(buffer);
    size_t offset = 0;
    while (offset < byte_count) {
        if (cancel_event != nullptr && WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0) {
            return PipeIoStatus::Canceled;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline_tick) {
            return PipeIoStatus::Timeout;
        }
        if (!ResetEvent(io_event)) {
            return PipeIoStatus::Broken;
        }
        OVERLAPPED ov{};
        ov.hEvent = io_event;
        const DWORD chunk = static_cast<DWORD>(
            (std::min<size_t>)(byte_count - offset, 64u * 1024u));
        DWORD transferred = 0;
        const BOOL immediate = ReadFile(pipe, out + offset, chunk, &transferred, &ov);
        if (!immediate) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                return PipeIoStatus::Broken;
            }
            HANDLE waits[2]{io_event, cancel_event};
            const DWORD wait_count = cancel_event != nullptr ? 2u : 1u;
            const DWORD wait_ms = static_cast<DWORD>(
                (std::min<ULONGLONG>)(deadline_tick - now, 0x7fffffffULL));
            const DWORD wait_res = WaitForMultipleObjects(wait_count, waits, FALSE, wait_ms);
            if (wait_res != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                DWORD canceled_bytes = 0;
                GetOverlappedResult(pipe, &ov, &canceled_bytes, TRUE);
                if (cancel_event != nullptr && wait_res == WAIT_OBJECT_0 + 1) {
                    return PipeIoStatus::Canceled;
                }
                if (wait_res == WAIT_TIMEOUT) {
                    return PipeIoStatus::Timeout;
                }
                return PipeIoStatus::Broken;
            }
            if (!GetOverlappedResult(pipe, &ov, &transferred, FALSE)) {
                return PipeIoStatus::Broken;
            }
        }
        if (transferred == 0) {
            return PipeIoStatus::Broken;
        }
        offset += transferred;
    }
    return PipeIoStatus::Ok;
}

PipeIoStatus WriteExactOverlapped(HANDLE pipe, HANDLE io_event, HANDLE cancel_event,
                                  const void *buffer, size_t byte_count, ULONGLONG deadline_tick) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || io_event == nullptr) {
        return PipeIoStatus::Broken;
    }
    const auto *in = static_cast<const uint8_t *>(buffer);
    size_t offset = 0;
    while (offset < byte_count) {
        if (cancel_event != nullptr && WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0) {
            return PipeIoStatus::Canceled;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline_tick) {
            return PipeIoStatus::Timeout;
        }
        if (!ResetEvent(io_event)) {
            return PipeIoStatus::Broken;
        }
        OVERLAPPED ov{};
        ov.hEvent = io_event;
        const DWORD chunk = static_cast<DWORD>(
            (std::min<size_t>)(byte_count - offset, 64u * 1024u));
        DWORD transferred = 0;
        const BOOL immediate = WriteFile(pipe, in + offset, chunk, &transferred, &ov);
        if (!immediate) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                return PipeIoStatus::Broken;
            }
            HANDLE waits[2]{io_event, cancel_event};
            const DWORD wait_count = cancel_event != nullptr ? 2u : 1u;
            const DWORD wait_ms = static_cast<DWORD>(
                (std::min<ULONGLONG>)(deadline_tick - now, 0x7fffffffULL));
            const DWORD wait_res = WaitForMultipleObjects(wait_count, waits, FALSE, wait_ms);
            if (wait_res != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &ov);
                DWORD canceled_bytes = 0;
                GetOverlappedResult(pipe, &ov, &canceled_bytes, TRUE);
                if (cancel_event != nullptr && wait_res == WAIT_OBJECT_0 + 1) {
                    return PipeIoStatus::Canceled;
                }
                if (wait_res == WAIT_TIMEOUT) {
                    return PipeIoStatus::Timeout;
                }
                return PipeIoStatus::Broken;
            }
            if (!GetOverlappedResult(pipe, &ov, &transferred, FALSE)) {
                return PipeIoStatus::Broken;
            }
        }
        if (transferred == 0) {
            return PipeIoStatus::Broken;
        }
        offset += transferred;
    }
    return PipeIoStatus::Ok;
}

BrokerStatus MapPipeIoStatus(PipeIoStatus io_status, bool session_revoked) noexcept {
    if (io_status == PipeIoStatus::Canceled || session_revoked) {
        return BrokerStatus::Revoked;
    }
    if (io_status == PipeIoStatus::Timeout) {
        return BrokerStatus::Timeout;
    }
    return BrokerStatus::TransportError;
}

BrokerStatus WriteFrame(HANDLE pipe, HANDLE io_event, HANDLE cancel_event, BrokerOp op,
                        uint64_t request_id, BrokerStatus status,
                        const std::vector<uint8_t> &payload, ULONGLONG deadline_tick,
                        bool session_revoked = false) {
    if (payload.size() > kMaxBrokerPayloadBytes) {
        return BrokerStatus::InvalidArgument;
    }
    BrokerFrameHeader header{};
    header.magic = kBrokerFrameMagic;
    header.version = kBrokerProtocolVersion;
    header.op = static_cast<uint16_t>(op);
    header.request_id = request_id;
    header.payload_size = static_cast<uint32_t>(payload.size());
    header.status = static_cast<uint32_t>(status);

    const PipeIoStatus hdr_io =
        WriteExactOverlapped(pipe, io_event, cancel_event, &header, sizeof(header), deadline_tick);
    if (hdr_io != PipeIoStatus::Ok) {
        return MapPipeIoStatus(hdr_io, session_revoked);
    }
    if (!payload.empty()) {
        const PipeIoStatus body_io = WriteExactOverlapped(
            pipe, io_event, cancel_event, payload.data(), payload.size(), deadline_tick);
        if (body_io != PipeIoStatus::Ok) {
            return MapPipeIoStatus(body_io, session_revoked);
        }
    }
    return BrokerStatus::Ok;
}

BrokerStatus ReadFrame(HANDLE pipe, HANDLE io_event, HANDLE cancel_event,
                       BrokerFrameHeader &header_out, std::vector<uint8_t> &payload_out,
                       ULONGLONG deadline_tick, bool session_revoked = false) {
    const PipeIoStatus hdr_io = ReadExactOverlapped(
        pipe, io_event, cancel_event, &header_out, sizeof(header_out), deadline_tick);
    if (hdr_io != PipeIoStatus::Ok) {
        return MapPipeIoStatus(hdr_io, session_revoked);
    }
    if (header_out.magic != kBrokerFrameMagic || header_out.version != kBrokerProtocolVersion ||
        header_out.payload_size > kMaxBrokerPayloadBytes) {
        return BrokerStatus::InvalidArgument;
    }
    payload_out.resize(header_out.payload_size);
    if (header_out.payload_size > 0) {
        const PipeIoStatus body_io = ReadExactOverlapped(
            pipe, io_event, cancel_event, payload_out.data(), payload_out.size(), deadline_tick);
        if (body_io != PipeIoStatus::Ok) {
            return MapPipeIoStatus(body_io, session_revoked);
        }
    }
    return BrokerStatus::Ok;
}

bool QueryTokenBuffer(HANDLE token, TOKEN_INFORMATION_CLASS info_class,
                      std::vector<uint8_t> &bytes_out) {
    DWORD needed = 0;
    GetTokenInformation(token, info_class, nullptr, 0, &needed);
    if (needed == 0) {
        return false;
    }
    bytes_out.resize(needed);
    return GetTokenInformation(token, info_class, bytes_out.data(), needed, &needed) != FALSE;
}

bool CreateWellKnownSidBuffer(WELL_KNOWN_SID_TYPE type, std::vector<uint8_t> &sid_out) {
    DWORD size = SECURITY_MAX_SID_SIZE;
    sid_out.resize(size);
    if (!CreateWellKnownSid(type, nullptr, sid_out.data(), &size)) {
        return false;
    }
    sid_out.resize(size);
    return true;
}

bool SidInGroupList(PSID candidate, const TOKEN_GROUPS *groups) noexcept {
    if (candidate == nullptr || groups == nullptr) {
        return false;
    }
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if (groups->Groups[i].Sid != nullptr && EqualSid(candidate, groups->Groups[i].Sid)) {
            return true;
        }
    }
    return false;
}

DWORD GetTokenIntegrityRid(HANDLE token, bool &ok_out) {
    ok_out = false;
    std::vector<uint8_t> il_bytes;
    if (!QueryTokenBuffer(token, TokenIntegrityLevel, il_bytes)) {
        return 0;
    }
    const auto *tml = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(il_bytes.data());
    if (tml->Label.Sid == nullptr || !IsValidSid(tml->Label.Sid)) {
        return 0;
    }
    const UCHAR count = *GetSidSubAuthorityCount(tml->Label.Sid);
    if (count == 0) {
        return 0;
    }
    ok_out = true;
    return *GetSidSubAuthority(tml->Label.Sid, static_cast<DWORD>(count - 1));
}

bool ConfigureTokenDefaultDacl(HANDLE token, PSID user_sid, PSID restricted_grant_sid) {
    std::vector<uint8_t> system_sid;
    if (!CreateWellKnownSidBuffer(WinLocalSystemSid, system_sid)) {
        return false;
    }
    PSID sids[3]{
        reinterpret_cast<PSID>(system_sid.data()),
        user_sid,
        restricted_grant_sid,
    };
    DWORD acl_bytes = sizeof(ACL);
    for (PSID sid : sids) {
        if (sid != nullptr && IsValidSid(sid)) {
            acl_bytes += sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(sid);
        }
    }
    acl_bytes = (acl_bytes + 3u) & ~3u;
    std::vector<uint8_t> acl_storage(acl_bytes, 0);
    auto *acl = reinterpret_cast<PACL>(acl_storage.data());
    if (!InitializeAcl(acl, acl_bytes, ACL_REVISION)) {
        return false;
    }
    for (PSID sid : sids) {
        if (sid != nullptr && IsValidSid(sid)) {
            if (!AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, sid)) {
                return false;
            }
        }
    }
    TOKEN_DEFAULT_DACL default_dacl{};
    default_dacl.DefaultDacl = acl;
    return SetTokenInformation(token, TokenDefaultDacl, &default_dacl, sizeof(default_dacl)) !=
           FALSE;
}

std::wstring SidToString(PSID sid) {
    LPWSTR raw = nullptr;
    if (sid == nullptr || !ConvertSidToStringSidW(sid, &raw)) {
        return {};
    }
    std::wstring out(raw);
    LocalFree(raw);
    return out;
}

std::wstring TrimTrailingDirectorySlash(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        // Preserve root slash on "C:\" and "\\?\C:\".
        if (path.size() == 7 && path.rfind(L"\\\\?\\", 0) == 0 && path[5] == L':') {
            break;
        }
        path.pop_back();
    }
    return path;
}

bool CanonicalizeFullPath(const std::wstring &input, std::wstring &output) {
    if (input.empty()) {
        return false;
    }
    const DWORD needed = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return false;
    }
    std::wstring buffer(needed, L'\0');
    const DWORD written = GetFullPathNameW(input.c_str(), needed, buffer.data(), nullptr);
    if (written == 0 || written >= needed) {
        return false;
    }
    buffer.resize(written);
    output = TrimTrailingDirectorySlash(std::move(buffer));
    return true;
}

bool ResolveFinalHandlePath(HANDLE handle, std::wstring &output) {
    const DWORD needed =
        GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (needed == 0) {
        return false;
    }
    std::wstring buffer(needed, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle, buffer.data(), needed, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written >= needed) {
        return false;
    }
    buffer.resize(written);
    output = TrimTrailingDirectorySlash(std::move(buffer));
    return true;
}

bool ValidateOrdinaryAbsolutePathLexical(const std::wstring &path) {
    if (path.size() < 4) {
        return false;
    }
    for (wchar_t ch : path) {
        if (ch == L'\0' || ch < 0x20) {
            return false;
        }
    }
    const wchar_t drive = path[0];
    const bool is_drive =
        (drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z');
    if (!is_drive || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) {
        return false;
    }
    // Reject UNC/device prefixes, alternate data streams (':'), trailing slash, and '.' / '..' segments.
    if (path.back() == L'\\' || path.back() == L'/') {
        return false;
    }
    for (size_t i = 2; i < path.size(); ++i) {
        if (path[i] == L':') {
            return false;
        }
    }
    size_t seg_start = 3;
    while (seg_start <= path.size()) {
        size_t sep = path.find_first_of(L"\\/", seg_start);
        const size_t seg_end = (sep == std::wstring::npos) ? path.size() : sep;
        const size_t seg_len = seg_end - seg_start;
        if (seg_len == 0) {
            return false;
        }
        const std::wstring_view segment(path.data() + seg_start, seg_len);
        if (segment == L"." || segment == L"..") {
            return false;
        }
        if (sep == std::wstring::npos) {
            break;
        }
        seg_start = sep + 1;
    }
    return true;
}

bool IsPathStrictlyUnderRoot(const std::wstring &candidate, const std::wstring &root) {
    if (root.empty() || candidate.size() <= root.size()) {
        return false;
    }
    const bool root_has_slash = (root.back() == L'\\');
    if (!root_has_slash && candidate[root.size()] != L'\\') {
        return false;
    }
    if (!root_has_slash && candidate.size() <= root.size() + 1) {
        return false;
    }
    return CompareStringOrdinal(candidate.c_str(), static_cast<int>(root.size()), root.c_str(),
                                static_cast<int>(root.size()), TRUE) == CSTR_EQUAL;
}

enum class PathAttributeVerdict : uint8_t {
    OkFile = 0,
    OkDirectory = 1,
    NotFound = 2,
    ReparsePoint = 3,
    Invalid = 4,
};

PathAttributeVerdict InspectPathNoFollow(const std::wstring &path, std::wstring *final_path_out) {
    UniqueHandle attr_handle(CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!attr_handle.Valid()) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return PathAttributeVerdict::NotFound;
        }
        return PathAttributeVerdict::Invalid;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(attr_handle.Get(), &info)) {
        return PathAttributeVerdict::Invalid;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return PathAttributeVerdict::ReparsePoint;
    }
    if (final_path_out != nullptr && !ResolveFinalHandlePath(attr_handle.Get(), *final_path_out)) {
        return PathAttributeVerdict::Invalid;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return PathAttributeVerdict::OkDirectory;
    }
    return PathAttributeVerdict::OkFile;
}

std::wstring BuildUnicodeEnvironmentBlock(std::vector<std::wstring> entries) {
    std::sort(entries.begin(), entries.end(), [](const std::wstring &a, const std::wstring &b) {
        const size_t eq_a = a.find(L'=');
        const size_t eq_b = b.find(L'=');
        const int len_a = static_cast<int>(eq_a == std::wstring::npos ? a.size() : eq_a);
        const int len_b = static_cast<int>(eq_b == std::wstring::npos ? b.size() : eq_b);
        return CompareStringOrdinal(a.c_str(), len_a, b.c_str(), len_b, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto &entry : entries) {
        block += entry;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

} // namespace

CapabilityBroker::~CapabilityBroker() {
    TerminateAndCleanupWorker(ERROR_BROKEN_PIPE, true);
    if (io_event_ != nullptr) {
        CloseHandle(io_event_);
        io_event_ = nullptr;
    }
    if (cancel_event_ != nullptr) {
        CloseHandle(cancel_event_);
        cancel_event_ = nullptr;
    }
    if (appcontainer_sid_ != nullptr) {
        FreeSid(appcontainer_sid_);
        appcontainer_sid_ = nullptr;
    }
}

bool CapabilityBroker::Fail(const wchar_t *stage, DWORD error) {
    stage_ = stage;
    error_ = error;
    return false;
}

BrokerStatus CapabilityBroker::CheckCapabilityGate(BrokerCapability required) noexcept {
    if (session_revoked_.load(std::memory_order_acquire)) {
        denied_call_count_.fetch_add(1, std::memory_order_acq_rel);
        return BrokerStatus::Revoked;
    }
    const auto granted =
        static_cast<BrokerCapability>(granted_capabilities_.load(std::memory_order_acquire));
    if (!HasBrokerCapability(granted, required)) {
        denied_call_count_.fetch_add(1, std::memory_order_acq_rel);
        return BrokerStatus::PermissionDenied;
    }
    return BrokerStatus::Ok;
}

void CapabilityBroker::GrantCapabilities(BrokerCapability mask) noexcept {
    granted_capabilities_.fetch_or(static_cast<uint32_t>(mask), std::memory_order_acq_rel);
}

void CapabilityBroker::RevokeCapabilities(BrokerCapability mask) noexcept {
    granted_capabilities_.fetch_and(~static_cast<uint32_t>(mask), std::memory_order_acq_rel);
}

BrokerCapability CapabilityBroker::CurrentCapabilities() const noexcept {
    return static_cast<BrokerCapability>(granted_capabilities_.load(std::memory_order_acquire));
}

void CapabilityBroker::SetContextSnapshot(SanitizedContextSnapshot snapshot) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    context_snapshot_ = std::move(snapshot);
}

bool CapabilityBroker::IsWorkerAlive() const noexcept {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (worker_process_ == nullptr) {
        return false;
    }
    return WaitForSingleObject(worker_process_, 0) == WAIT_TIMEOUT;
}

bool CapabilityBroker::CanonicalizeAllowedRoots(const std::vector<std::wstring> &roots) {
    allowed_roots_.clear();
    allowed_roots_.reserve(roots.size());
    for (const auto &raw_root : roots) {
        std::wstring full_root;
        if (!CanonicalizeFullPath(raw_root, full_root)) {
            return Fail(L"allowed-root-canonicalize", GetLastError());
        }
        std::wstring final_root;
        const PathAttributeVerdict verdict = InspectPathNoFollow(full_root, &final_root);
        if (verdict == PathAttributeVerdict::ReparsePoint) {
            return Fail(L"allowed-root-reparse-point", ERROR_ACCESS_DENIED);
        }
        if (verdict != PathAttributeVerdict::OkDirectory) {
            return Fail(L"allowed-root-not-directory", ERROR_DIRECTORY);
        }
        allowed_roots_.push_back(CanonicalRoot{std::move(full_root), std::move(final_root)});
    }
    return true;
}

bool CapabilityBroker::CreateBrokerPipePair(HANDLE &client_pipe_out) {
    client_pipe_out = INVALID_HANDLE_VALUE;
    uint8_t random_name_bytes[16]{};
    if (!GenerateRandomBytes(random_name_bytes, sizeof(random_name_bytes)) ||
        !GenerateRandomBytes(session_token_, sizeof(session_token_))) {
        return Fail(L"BCryptGenRandom", ERROR_GEN_FAILURE);
    }

    UniqueHandle self_token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, self_token.Put())) {
        return Fail(L"OpenProcessToken-pipe", GetLastError());
    }
    std::vector<uint8_t> user_bytes;
    if (!QueryTokenBuffer(self_token.Get(), TokenUser, user_bytes)) {
        return Fail(L"GetTokenInformation-pipe-user", GetLastError());
    }
    const std::wstring user_sid =
        SidToString(reinterpret_cast<const TOKEN_USER *>(user_bytes.data())->User.Sid);
    if (user_sid.empty()) {
        return Fail(L"ConvertSidToStringSidW-pipe", GetLastError());
    }

    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + user_sid + L")";
    PSECURITY_DESCRIPTOR raw_sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &raw_sd, nullptr)) {
        return Fail(L"ConvertStringSecurityDescriptor-pipe", GetLastError());
    }
    struct LocalSdDeleter {
        void operator()(void *ptr) const noexcept {
            if (ptr != nullptr) {
                LocalFree(ptr);
            }
        }
    };
    std::unique_ptr<void, LocalSdDeleter> sd_guard(raw_sd);

    SECURITY_ATTRIBUTES server_sa{};
    server_sa.nLength = sizeof(server_sa);
    server_sa.lpSecurityDescriptor = raw_sd;
    server_sa.bInheritHandle = FALSE;

    const std::wstring suffix = BytesToHexW(random_name_bytes, sizeof(random_name_bytes));
    std::wstring pipe_name = L"\\\\.\\pipe\\LOCAL\\pulse_broker_" + suffix;
    const DWORD open_mode =
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED;
    const DWORD pipe_mode =
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;

    HANDLE server_pipe = CreateNamedPipeW(
        pipe_name.c_str(), open_mode, pipe_mode, 1, 64 * 1024, 64 * 1024, 0, &server_sa);
    if (server_pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND) {
        pipe_name = L"\\\\.\\pipe\\pulse_broker_" + suffix;
        server_pipe = CreateNamedPipeW(
            pipe_name.c_str(), open_mode, pipe_mode, 1, 64 * 1024, 64 * 1024, 0, &server_sa);
    }
    if (server_pipe == INVALID_HANDLE_VALUE) {
        return Fail(L"CreateNamedPipeW", GetLastError());
    }

    SECURITY_ATTRIBUTES client_sa{};
    client_sa.nLength = sizeof(client_sa);
    client_sa.lpSecurityDescriptor = nullptr;
    client_sa.bInheritHandle = TRUE;

    HANDLE client_pipe = CreateFileW(
        pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, &client_sa, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr);
    if (client_pipe == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        CloseHandle(server_pipe);
        return Fail(L"CreateFileW-client-pipe", err);
    }

    host_pipe_ = server_pipe;
    client_pipe_out = client_pipe;
    return true;
}

bool CapabilityBroker::ConfigureJobObject(HANDLE &job_out) {
    job_out = nullptr;
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.Valid()) {
        return Fail(L"CreateJobObjectW", GetLastError());
    }

    const uint32_t mem_mib = config_.memory_limit_mib > 0 ? config_.memory_limit_mib : 64u;
    const SIZE_T mem_bytes = static_cast<SIZE_T>(mem_mib) * 1024u * 1024u;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext_limits{};
    ext_limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_PROCESS_MEMORY |
        JOB_OBJECT_LIMIT_JOB_MEMORY;
    ext_limits.BasicLimitInformation.ActiveProcessLimit =
        config_.active_process_limit > 0 ? config_.active_process_limit : 1u;
    ext_limits.ProcessMemoryLimit = mem_bytes;
    ext_limits.JobMemoryLimit = mem_bytes;
    if (!SetInformationJobObject(
            job.Get(), JobObjectExtendedLimitInformation, &ext_limits, sizeof(ext_limits))) {
        return Fail(L"SetInformationJobObject-ExtendedLimits", GetLastError());
    }

    const uint32_t cpu_rate =
        std::clamp<uint32_t>(config_.cpu_rate_hard_cap > 0 ? config_.cpu_rate_hard_cap : 2500u,
                             1u, 10000u);
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu_info{};
    cpu_info.ControlFlags =
        JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    cpu_info.CpuRate = cpu_rate;
    if (!SetInformationJobObject(
            job.Get(), JobObjectCpuRateControlInformation, &cpu_info, sizeof(cpu_info))) {
        return Fail(L"SetInformationJobObject-CpuRate", GetLastError());
    }

    JOBOBJECT_BASIC_UI_RESTRICTIONS ui_limits{};
    ui_limits.UIRestrictionsClass =
        JOB_OBJECT_UILIMIT_HANDLES |
        JOB_OBJECT_UILIMIT_READCLIPBOARD |
        JOB_OBJECT_UILIMIT_WRITECLIPBOARD |
        JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS |
        JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
        JOB_OBJECT_UILIMIT_GLOBALATOMS |
        JOB_OBJECT_UILIMIT_DESKTOP |
        JOB_OBJECT_UILIMIT_EXITWINDOWS;
    if (!SetInformationJobObject(
            job.Get(), JobObjectBasicUIRestrictions, &ui_limits, sizeof(ui_limits))) {
        return Fail(L"SetInformationJobObject-UIRestrictions", GetLastError());
    }

    job_out = job.Release();
    return true;
}

bool CapabilityBroker::LaunchAppContainer(HANDLE client_pipe, const std::wstring &command_line,
                                          const std::wstring &system_root) {
    DynamicUserenvApi userenv;
    if (userenv.create_profile == nullptr || userenv.delete_profile == nullptr) {
        return Fail(L"userenv-unavailable",
                    userenv.load_error != ERROR_SUCCESS ? userenv.load_error
                                                        : ERROR_PROC_NOT_FOUND);
    }

    if (!config_.appcontainer_profile_name.empty()) {
        appcontainer_profile_ = config_.appcontainer_profile_name;
    } else {
        uint8_t profile_rand[8]{};
        if (!GenerateRandomBytes(profile_rand, sizeof(profile_rand))) {
            return Fail(L"BCryptGenRandom-profile", ERROR_GEN_FAILURE);
        }
        appcontainer_profile_ = L"Pulse.ExtensionBroker." +
                                std::to_wstring(GetCurrentProcessId()) + L"." +
                                BytesToHexW(profile_rand, sizeof(profile_rand));
    }

    if (appcontainer_sid_ != nullptr) {
        FreeSid(appcontainer_sid_);
        appcontainer_sid_ = nullptr;
    }
    owns_appcontainer_profile_ = false;
    const HRESULT hr = userenv.create_profile(
        appcontainer_profile_.c_str(), L"Pulse Extension Sandbox",
        L"Zero-capability AppContainer for Pulse extension worker", nullptr, 0,
        &appcontainer_sid_);
    if (FAILED(hr)) {
        appcontainer_profile_.clear();
        return Fail(L"CreateAppContainerProfile", static_cast<DWORD>(hr));
    }
    owns_appcontainer_profile_ = config_.delete_appcontainer_profile_on_close;

    wchar_t local_app_data[32768]{};
    const DWORD local_len = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, 32768);
    if (local_len == 0 || local_len >= 32768) {
        RemoveAppContainerProfile();
        return Fail(L"AppContainer-LOCALAPPDATA", ERROR_ENVVAR_NOT_FOUND);
    }

    HANDLE job_raw = nullptr;
    if (!ConfigureJobObject(job_raw)) {
        const DWORD saved_err = error_;
        const std::wstring saved_stage = stage_;
        RemoveAppContainerProfile();
        stage_ = saved_stage;
        error_ = saved_err;
        return false;
    }
    UniqueHandle job(job_raw);

    ProcThreadAttributeBag attrs;
    if (!attrs.Initialize(3)) {
        const DWORD err = GetLastError();
        RemoveAppContainerProfile();
        return Fail(L"InitializeProcThreadAttributeList-AppContainer", err);
    }

    SECURITY_CAPABILITIES sec_caps{};
    sec_caps.AppContainerSid = appcontainer_sid_;
    sec_caps.Capabilities = nullptr;
    sec_caps.CapabilityCount = 0;
    DWORD child_policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    HANDLE inherited_handles[1]{client_pipe};

    if (!UpdateProcThreadAttribute(attrs.list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                   &sec_caps, sizeof(sec_caps), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attrs.list, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,
                                   &child_policy, sizeof(child_policy), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attrs.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited_handles, sizeof(inherited_handles), nullptr,
                                   nullptr)) {
        const DWORD err = GetLastError();
        RemoveAppContainerProfile();
        return Fail(L"UpdateProcThreadAttribute-AppContainer", err);
    }

    std::wstring env_block = BuildUnicodeEnvironmentBlock({
        L"LOCALAPPDATA=" + std::wstring(local_app_data, local_len),
        L"SystemRoot=" + system_root,
    });
    std::wstring mutable_cmd = command_line;

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attrs.list;
    PROCESS_INFORMATION pi{};

    const BOOL created = CreateProcessW(
        config_.executable_path.c_str(), mutable_cmd.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | DETACHED_PROCESS |
            CREATE_UNICODE_ENVIRONMENT,
        env_block.data(), system_root.c_str(), &startup.StartupInfo, &pi);
    if (!created) {
        const DWORD err = GetLastError();
        RemoveAppContainerProfile();
        return Fail(L"CreateProcessW-AppContainer", err);
    }

    UniqueHandle proc_guard(pi.hProcess);
    UniqueHandle thread_guard(pi.hThread);

    // Verify zero-capability AppContainer token BEFORE resuming the child thread.
    UniqueHandle child_token;
    DWORD is_appcontainer = 0;
    DWORD ret_len = 0;
    bool token_ok = OpenProcessToken(proc_guard.Get(), TOKEN_QUERY, child_token.Put()) != FALSE &&
                    GetTokenInformation(child_token.Get(), TokenIsAppContainer, &is_appcontainer,
                                        sizeof(is_appcontainer), &ret_len) &&
                    is_appcontainer != 0;
    std::vector<uint8_t> ac_info_bytes;
    std::vector<uint8_t> cap_bytes;
    if (token_ok) {
        token_ok = QueryTokenBuffer(child_token.Get(), TokenAppContainerSid, ac_info_bytes) &&
                   EqualSid(reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION *>(
                                ac_info_bytes.data())
                                ->TokenAppContainer,
                            appcontainer_sid_) &&
                   QueryTokenBuffer(child_token.Get(), TokenCapabilities, cap_bytes) &&
                   reinterpret_cast<const TOKEN_GROUPS *>(cap_bytes.data())->GroupCount == 0;
    }
    child_token.Reset();

    if (!token_ok || !AssignProcessToJobObject(job.Get(), proc_guard.Get())) {
        const DWORD err = token_ok ? GetLastError() : ERROR_INVALID_TOKEN;
        TerminateProcess(proc_guard.Get(), err);
        WaitForSingleObject(proc_guard.Get(), 5000);
        proc_guard.Reset();
        thread_guard.Reset();
        job.Reset();
        RemoveAppContainerProfile();
        return Fail(token_ok ? L"AssignProcessToJobObject-AppContainer"
                             : L"VerifyAppContainerToken",
                    err);
    }

    if (ResumeThread(thread_guard.Get()) == static_cast<DWORD>(-1)) {
        const DWORD err = GetLastError();
        TerminateJobObject(job.Get(), err);
        WaitForSingleObject(proc_guard.Get(), 5000);
        proc_guard.Reset();
        thread_guard.Reset();
        job.Reset();
        RemoveAppContainerProfile();
        return Fail(L"ResumeThread-AppContainer", err);
    }

    job_ = job.Release();
    worker_process_ = proc_guard.Release();
    worker_pid_ = pi.dwProcessId;
    isolation_tier_ = IsolationTier::AppContainer;
    return true;
}

bool CapabilityBroker::LaunchRestrictedToken(HANDLE client_pipe, const std::wstring &command_line,
                                             const std::wstring &system_root) {
    UniqueHandle parent_token;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY |
                              TOKEN_ADJUST_DEFAULT,
                          parent_token.Put())) {
        return Fail(L"OpenProcessToken-parent", GetLastError());
    }

    std::vector<uint8_t> user_bytes;
    std::vector<uint8_t> groups_bytes;
    if (!QueryTokenBuffer(parent_token.Get(), TokenUser, user_bytes) ||
        !QueryTokenBuffer(parent_token.Get(), TokenGroups, groups_bytes)) {
        return Fail(L"QueryTokenInformation-parent", GetLastError());
    }
    PSID user_sid = reinterpret_cast<TOKEN_USER *>(user_bytes.data())->User.Sid;
    const auto *groups = reinterpret_cast<const TOKEN_GROUPS *>(groups_bytes.data());

    std::vector<uint8_t> world_sid;
    std::vector<uint8_t> builtin_users_sid;
    if (!CreateWellKnownSidBuffer(WinWorldSid, world_sid) ||
        !CreateWellKnownSidBuffer(WinBuiltinUsersSid, builtin_users_sid)) {
        return Fail(L"CreateWellKnownSid-restrict", GetLastError());
    }
    PSID world_psid = reinterpret_cast<PSID>(world_sid.data());
    PSID users_psid = reinterpret_cast<PSID>(builtin_users_sid.data());

    PSID logon_psid = nullptr;
    std::vector<SID_AND_ATTRIBUTES> sids_to_disable;
    sids_to_disable.reserve(groups->GroupCount);
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        const SID_AND_ATTRIBUTES &entry = groups->Groups[i];
        const DWORD attrs = entry.Attributes;
        if ((attrs & (SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED)) != 0) {
            continue;
        }
        if ((attrs & SE_GROUP_LOGON_ID) != 0 && logon_psid == nullptr) {
            logon_psid = entry.Sid;
            continue;
        }
        if (EqualSid(entry.Sid, world_psid) || EqualSid(entry.Sid, users_psid)) {
            continue;
        }
        if ((attrs & SE_GROUP_USE_FOR_DENY_ONLY) == 0) {
            SID_AND_ATTRIBUTES disable_entry{};
            disable_entry.Sid = entry.Sid;
            disable_entry.Attributes = 0;
            sids_to_disable.push_back(disable_entry);
        }
    }

    // Build restricting SIDs: [Everyone (S-1-1-0), Builtin\Users (S-1-5-32-545), LogonSid],
    // intentionally omitting TokenUser so private user DACLs fail the kernel's second access check.
    // When running nested inside an outer restricted-token sandbox, intersect against the parent's
    // existing TokenRestrictedSids so NtFilterToken never rejects a non-subset SID.
    std::vector<uint8_t> parent_restricted_bytes;
    const TOKEN_GROUPS *parent_restricted = nullptr;
    if (IsTokenRestricted(parent_token.Get()) != FALSE &&
        QueryTokenBuffer(parent_token.Get(), TokenRestrictedSids, parent_restricted_bytes)) {
        const auto *parsed =
            reinterpret_cast<const TOKEN_GROUPS *>(parent_restricted_bytes.data());
        if (parsed->GroupCount > 0) {
            parent_restricted = parsed;
        }
    }

    std::vector<SID_AND_ATTRIBUTES> sids_to_restrict;
    auto maybe_add_restrict = [&](PSID candidate) {
        if (candidate == nullptr || EqualSid(candidate, user_sid)) {
            return;
        }
        if (parent_restricted != nullptr && !SidInGroupList(candidate, parent_restricted)) {
            return;
        }
        for (const auto &existing : sids_to_restrict) {
            if (EqualSid(existing.Sid, candidate)) {
                return;
            }
        }
        SID_AND_ATTRIBUTES item{};
        item.Sid = candidate;
        item.Attributes = 0;
        sids_to_restrict.push_back(item);
    };
    maybe_add_restrict(world_psid);
    maybe_add_restrict(users_psid);
    maybe_add_restrict(logon_psid);

    if (sids_to_restrict.empty() && parent_restricted != nullptr) {
        for (DWORD i = 0; i < parent_restricted->GroupCount; ++i) {
            PSID candidate = parent_restricted->Groups[i].Sid;
            if (candidate != nullptr && !EqualSid(candidate, user_sid)) {
                SID_AND_ATTRIBUTES item{};
                item.Sid = candidate;
                item.Attributes = 0;
                sids_to_restrict.push_back(item);
            }
        }
    }
    if (sids_to_restrict.empty()) {
        return Fail(L"BuildRestrictingSids-empty", ERROR_INVALID_SID);
    }

    UniqueHandle restricted_token;
    if (!CreateRestrictedToken(
            parent_token.Get(), DISABLE_MAX_PRIVILEGE | SANDBOX_INERT,
            static_cast<DWORD>(sids_to_disable.size()),
            sids_to_disable.empty() ? nullptr : sids_to_disable.data(), 0, nullptr,
            static_cast<DWORD>(sids_to_restrict.size()), sids_to_restrict.data(),
            restricted_token.Put())) {
        return Fail(L"CreateRestrictedToken", GetLastError());
    }

    PSID dacl_grant_sid =
        (logon_psid != nullptr &&
         (parent_restricted == nullptr || SidInGroupList(logon_psid, parent_restricted)))
            ? logon_psid
            : sids_to_restrict.front().Sid;
    if (!ConfigureTokenDefaultDacl(restricted_token.Get(), user_sid, dacl_grant_sid)) {
        return Fail(L"ConfigureTokenDefaultDacl", GetLastError());
    }

    bool il_ok = false;
    const DWORD current_il_rid = GetTokenIntegrityRid(restricted_token.Get(), il_ok);
    if (!il_ok) {
        return Fail(L"QueryRestrictedTokenIntegrity", ERROR_INVALID_TOKEN);
    }
    if (current_il_rid > SECURITY_MANDATORY_LOW_RID) {
        std::vector<uint8_t> low_il_sid;
        if (!CreateWellKnownSidBuffer(WinLowLabelSid, low_il_sid)) {
            return Fail(L"CreateWellKnownSid-LowLabel", GetLastError());
        }
        TOKEN_MANDATORY_LABEL tml{};
        tml.Label.Attributes = SE_GROUP_INTEGRITY;
        tml.Label.Sid = reinterpret_cast<PSID>(low_il_sid.data());
        const DWORD tml_size =
            sizeof(TOKEN_MANDATORY_LABEL) + GetLengthSid(tml.Label.Sid);
        if (!SetTokenInformation(restricted_token.Get(), TokenIntegrityLevel, &tml, tml_size)) {
            return Fail(L"SetTokenInformation-LowIntegrity", GetLastError());
        }
    }

    HANDLE job_raw = nullptr;
    if (!ConfigureJobObject(job_raw)) {
        return false;
    }
    UniqueHandle job(job_raw);

    ProcThreadAttributeBag attrs;
    if (!attrs.Initialize(2)) {
        return Fail(L"InitializeProcThreadAttributeList-Restricted", GetLastError());
    }
    DWORD child_policy = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
    HANDLE inherited_handles[1]{client_pipe};
    if (!UpdateProcThreadAttribute(attrs.list, 0, PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,
                                   &child_policy, sizeof(child_policy), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attrs.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited_handles, sizeof(inherited_handles), nullptr,
                                   nullptr)) {
        return Fail(L"UpdateProcThreadAttribute-Restricted", GetLastError());
    }

    std::wstring env_block = BuildUnicodeEnvironmentBlock({
        L"SystemRoot=" + system_root,
    });
    std::wstring mutable_cmd = command_line;

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attrs.list;
    PROCESS_INFORMATION pi{};

    const BOOL created = CreateProcessAsUserW(
        restricted_token.Get(), config_.executable_path.c_str(), mutable_cmd.data(), nullptr,
        nullptr, TRUE,
        CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | DETACHED_PROCESS |
            CREATE_UNICODE_ENVIRONMENT,
        env_block.data(), system_root.c_str(), &startup.StartupInfo, &pi);
    if (!created) {
        return Fail(L"CreateProcessAsUserW-Restricted", GetLastError());
    }

    UniqueHandle proc_guard(pi.hProcess);
    UniqueHandle thread_guard(pi.hThread);

    // Verify restricted token + Low Integrity BEFORE resuming the worker thread.
    UniqueHandle verify_token;
    bool verify_il_ok = false;
    bool verified = OpenProcessToken(proc_guard.Get(), TOKEN_QUERY, verify_token.Put()) != FALSE &&
                    IsTokenRestricted(verify_token.Get()) != FALSE;
    if (verified) {
        const DWORD child_il = GetTokenIntegrityRid(verify_token.Get(), verify_il_ok);
        verified = verify_il_ok && child_il <= SECURITY_MANDATORY_LOW_RID;
    }
    verify_token.Reset();

    if (!verified || !AssignProcessToJobObject(job.Get(), proc_guard.Get())) {
        const DWORD err = verified ? GetLastError() : ERROR_INVALID_TOKEN;
        TerminateProcess(proc_guard.Get(), err);
        WaitForSingleObject(proc_guard.Get(), 5000);
        return Fail(verified ? L"AssignProcessToJobObject-Restricted"
                             : L"VerifyRestrictedToken",
                    err);
    }

    if (ResumeThread(thread_guard.Get()) == static_cast<DWORD>(-1)) {
        const DWORD err = GetLastError();
        TerminateJobObject(job.Get(), err);
        WaitForSingleObject(proc_guard.Get(), 5000);
        return Fail(L"ResumeThread-Restricted", err);
    }

    job_ = job.Release();
    worker_process_ = proc_guard.Release();
    worker_pid_ = pi.dwProcessId;
    isolation_tier_ = IsolationTier::RestrictedToken;
    return true;
}

bool CapabilityBroker::PerformHandshake(uint32_t timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + (timeout_ms > 0 ? timeout_ms : 5000u);
    BrokerFrameHeader header{};
    std::vector<uint8_t> payload;
    const BrokerStatus recv_status =
        ReadFrame(host_pipe_, io_event_, cancel_event_, header, payload, deadline);
    if (recv_status != BrokerStatus::Ok) {
        return Fail(L"Handshake-ReadFrame",
                    recv_status == BrokerStatus::Timeout ? ERROR_TIMEOUT : ERROR_BROKEN_PIPE);
    }
    if (header.op != static_cast<uint16_t>(BrokerOp::Handshake) ||
        payload.size() != kBrokerSessionTokenBytes) {
        std::vector<uint8_t> empty;
        WriteFrame(host_pipe_, io_event_, cancel_event_, BrokerOp::HandshakeAck,
                   header.request_id, BrokerStatus::InvalidArgument, empty, deadline);
        return Fail(L"Handshake-MalformedFrame", ERROR_INVALID_DATA);
    }
    if (!ConstantTimeEquals(payload.data(), session_token_, kBrokerSessionTokenBytes)) {
        std::vector<uint8_t> empty;
        WriteFrame(host_pipe_, io_event_, cancel_event_, BrokerOp::HandshakeAck,
                   header.request_id, BrokerStatus::AuthFailed, empty, deadline);
        return Fail(L"Handshake-AuthFailed", ERROR_ACCESS_DENIED);
    }
    std::vector<uint8_t> empty;
    const BrokerStatus ack_status =
        WriteFrame(host_pipe_, io_event_, cancel_event_, BrokerOp::HandshakeAck,
                   header.request_id, BrokerStatus::Ok, empty, deadline);
    if (ack_status != BrokerStatus::Ok) {
        return Fail(L"Handshake-WriteAck", ERROR_BROKEN_PIPE);
    }
    return true;
}

bool CapabilityBroker::Start(const BrokerConfig &config) {
    if (worker_process_ != nullptr || host_pipe_ != INVALID_HANDLE_VALUE) {
        return Fail(L"already-started", ERROR_INVALID_STATE);
    }
    if (config.executable_path.empty()) {
        return Fail(L"empty-executable", ERROR_INVALID_PARAMETER);
    }

    config_ = config;
    if (config_.package_id.empty()) {
        config_.package_id = L"default";
    }
    stage_.clear();
    error_ = ERROR_SUCCESS;
    isolation_tier_ = IsolationTier::None;
    worker_pid_ = 0;
    session_revoked_.store(false, std::memory_order_release);
    denied_call_count_.store(0, std::memory_order_release);
    brokered_handle_count_.store(0, std::memory_order_release);
    granted_capabilities_.store(
        static_cast<uint32_t>(config_.initial_capabilities), std::memory_order_release);

    if (!CanonicalizeAllowedRoots(config_.allowed_read_roots)) {
        return false;
    }

    if (io_event_ == nullptr) {
        io_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (io_event_ == nullptr) {
            return Fail(L"CreateEventW-io", GetLastError());
        }
    } else {
        ResetEvent(io_event_);
    }

    if (cancel_event_ == nullptr) {
        cancel_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (cancel_event_ == nullptr) {
            return Fail(L"CreateEventW-cancel", GetLastError());
        }
    } else {
        ResetEvent(cancel_event_);
    }

    wchar_t sys_dir[MAX_PATH]{};
    const UINT sys_len = GetWindowsDirectoryW(sys_dir, MAX_PATH);
    if (sys_len == 0 || sys_len >= MAX_PATH) {
        return Fail(L"GetWindowsDirectoryW", GetLastError());
    }
    const std::wstring system_root(sys_dir, sys_len);

    HANDLE raw_client_pipe = INVALID_HANDLE_VALUE;
    if (!CreateBrokerPipePair(raw_client_pipe)) {
        return false;
    }
    UniqueHandle client_pipe_guard(raw_client_pipe);

    const uint64_t pipe_value =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(client_pipe_guard.Get()));
    const std::wstring token_hex = BytesToHexW(session_token_, kBrokerSessionTokenBytes);
    std::wstring command_line = QuoteArg(config_.executable_path);
    if (!config_.extra_arguments.empty()) {
        command_line += L" " + config_.extra_arguments;
    }
    command_line += L" --broker-pipe " + std::to_wstring(pipe_value) + L" --broker-token " +
                    token_hex;

    bool launched = false;
    if (config_.sandbox_mode == SandboxMode::RequireAppContainer) {
        launched = LaunchAppContainer(client_pipe_guard.Get(), command_line, system_root);
    } else if (config_.sandbox_mode == SandboxMode::RequireRestrictedToken) {
        launched = LaunchRestrictedToken(client_pipe_guard.Get(), command_line, system_root);
    } else {
        launched = LaunchAppContainer(client_pipe_guard.Get(), command_line, system_root);
        if (!launched) {
            launched = LaunchRestrictedToken(client_pipe_guard.Get(), command_line, system_root);
        }
    }

    // Close the host's copy of client_pipe immediately after CreateProcess* so ONLY the worker
    // holds the client end of the pipe.
    client_pipe_guard.Reset();

    if (!launched) {
        const std::wstring saved_stage = stage_;
        const DWORD saved_error = error_;
        TerminateAndCleanupWorker(saved_error, false);
        stage_ = saved_stage;
        error_ = saved_error;
        return false;
    }

    if (!PerformHandshake(config_.handshake_timeout_ms)) {
        const std::wstring saved_stage = stage_;
        const DWORD saved_error = error_;
        TerminateAndCleanupWorker(saved_error, false);
        stage_ = saved_stage;
        error_ = saved_error;
        return false;
    }

    stage_.clear();
    error_ = ERROR_SUCCESS;
    return true;
}

void CapabilityBroker::RevokeSession() {
    TerminateAndCleanupWorker(ERROR_ACCESS_DENIED, true);
}

void CapabilityBroker::TerminateAndCleanupWorker(DWORD exit_reason, bool mark_revoked) {
    if (mark_revoked) {
        session_revoked_.store(true, std::memory_order_release);
    }
    if (cancel_event_ != nullptr) {
        SetEvent(cancel_event_);
    }

    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (job_ != nullptr) {
            TerminateJobObject(job_, exit_reason);
            CloseHandle(job_);
            job_ = nullptr;
        }
        if (worker_process_ != nullptr) {
            if (WaitForSingleObject(worker_process_, 5000) != WAIT_OBJECT_0) {
                TerminateProcess(worker_process_, exit_reason);
                WaitForSingleObject(worker_process_, 2000);
            }
            CloseHandle(worker_process_);
            worker_process_ = nullptr;
        }
        worker_pid_ = 0;
        RemoveAppContainerProfile();
    }

    {
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        if (host_pipe_ != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(host_pipe_);
            CloseHandle(host_pipe_);
            host_pipe_ = INVALID_HANDLE_VALUE;
        }
    }
}

bool CapabilityBroker::RemoveAppContainerProfile() {
    if (!owns_appcontainer_profile_ || appcontainer_profile_.empty()) {
        appcontainer_profile_.clear();
        owns_appcontainer_profile_ = false;
        return true;
    }
    DynamicUserenvApi userenv;
    if (userenv.delete_profile == nullptr) {
        return false;
    }
    const HRESULT hr = userenv.delete_profile(appcontainer_profile_.c_str());
    appcontainer_profile_.clear();
    owns_appcontainer_profile_ = false;
    if (appcontainer_sid_ != nullptr) {
        FreeSid(appcontainer_sid_);
        appcontainer_sid_ = nullptr;
    }
    return SUCCEEDED(hr);
}

BrokerStatus CapabilityBroker::HandleSettingsGet(const std::wstring &key, std::string &value_out,
                                                 uint64_t &version_out) {
    value_out.clear();
    version_out = 0;
    const BrokerStatus gate = CheckCapabilityGate(BrokerCapability::SettingsOwn);
    if (gate != BrokerStatus::Ok) {
        return gate;
    }
    if (key.empty() || key.size() * sizeof(wchar_t) > kMaxSettingKeyBytes) {
        return BrokerStatus::InvalidArgument;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto pkg_it = settings_.find(config_.package_id);
    if (pkg_it == settings_.end()) {
        return BrokerStatus::NotFound;
    }
    const auto key_it = pkg_it->second.find(key);
    if (key_it == pkg_it->second.end()) {
        return BrokerStatus::NotFound;
    }
    value_out = key_it->second.value;
    version_out = key_it->second.version;
    return BrokerStatus::Ok;
}

BrokerStatus CapabilityBroker::HandleSettingsSet(const std::wstring &key, const std::string &value,
                                                 uint64_t expected_version,
                                                 uint64_t &new_version_out) {
    new_version_out = 0;
    const BrokerStatus gate = CheckCapabilityGate(BrokerCapability::SettingsOwn);
    if (gate != BrokerStatus::Ok) {
        return gate;
    }
    if (key.empty() || key.size() * sizeof(wchar_t) > kMaxSettingKeyBytes ||
        value.size() > kMaxSettingValueBytes) {
        return BrokerStatus::InvalidArgument;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    auto &pkg_map = settings_[config_.package_id];
    const auto it = pkg_map.find(key);
    const uint64_t current_version = (it == pkg_map.end()) ? 0u : it->second.version;

    if (expected_version == kSettingsVersionUnconditional) {
        const uint64_t next_version = current_version + 1u;
        pkg_map[key] = SettingRecord{value, next_version};
        new_version_out = next_version;
        return BrokerStatus::Ok;
    }
    if (expected_version == 0u) {
        if (it != pkg_map.end()) {
            new_version_out = current_version;
            return BrokerStatus::Conflict;
        }
        pkg_map.emplace(key, SettingRecord{value, 1u});
        new_version_out = 1u;
        return BrokerStatus::Ok;
    }
    if (it == pkg_map.end() || current_version != expected_version) {
        new_version_out = current_version;
        return BrokerStatus::Conflict;
    }

    it->second.value = value;
    it->second.version = current_version + 1u;
    new_version_out = it->second.version;
    return BrokerStatus::Ok;
}

BrokerStatus CapabilityBroker::HandleContextGet(SanitizedContextSnapshot &snapshot_out) {
    snapshot_out = {};
    const BrokerStatus gate = CheckCapabilityGate(BrokerCapability::ContextRead);
    if (gate != BrokerStatus::Ok) {
        return gate;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    snapshot_out = context_snapshot_;
    return BrokerStatus::Ok;
}

BrokerStatus CapabilityBroker::HandleOpenReadFile(const std::wstring &path,
                                                  BrokerReadFileResult &result_out) {
    result_out = {};
    const BrokerStatus gate = CheckCapabilityGate(BrokerCapability::ReadAllowedRoots);
    if (gate != BrokerStatus::Ok) {
        return gate;
    }
    if (!ValidateOrdinaryAbsolutePathLexical(path)) {
        return BrokerStatus::InvalidArgument;
    }

    std::wstring canonical_target;
    if (!CanonicalizeFullPath(path, canonical_target)) {
        return BrokerStatus::InvalidArgument;
    }

    const CanonicalRoot *matched_root = nullptr;
    for (const auto &root : allowed_roots_) {
        if (IsPathStrictlyUnderRoot(canonical_target, root.full_path)) {
            matched_root = &root;
            break;
        }
    }
    if (matched_root == nullptr) {
        return BrokerStatus::PathOutsideRoot;
    }

    // Inspect root, every ancestor directory under root, and the target itself with
    // FILE_FLAG_OPEN_REPARSE_POINT to block junctions and symlinks at any depth.
    std::wstring root_final_now;
    const PathAttributeVerdict root_verdict =
        InspectPathNoFollow(matched_root->full_path, &root_final_now);
    if (root_verdict == PathAttributeVerdict::ReparsePoint) {
        return BrokerStatus::ReparsePointRejected;
    }
    if (root_verdict != PathAttributeVerdict::OkDirectory) {
        return BrokerStatus::PathOutsideRoot;
    }

    size_t scan_pos = matched_root->full_path.size();
    if (matched_root->full_path.back() != L'\\') {
        ++scan_pos;
    }
    while (scan_pos < canonical_target.size()) {
        const size_t next_sep = canonical_target.find(L'\\', scan_pos);
        if (next_sep == std::wstring::npos) {
            break;
        }
        const std::wstring ancestor = canonical_target.substr(0, next_sep);
        const PathAttributeVerdict anc_verdict = InspectPathNoFollow(ancestor, nullptr);
        if (anc_verdict == PathAttributeVerdict::ReparsePoint) {
            return BrokerStatus::ReparsePointRejected;
        }
        if (anc_verdict == PathAttributeVerdict::NotFound) {
            return BrokerStatus::NotFound;
        }
        if (anc_verdict != PathAttributeVerdict::OkDirectory) {
            return BrokerStatus::InvalidArgument;
        }
        scan_pos = next_sep + 1;
    }

    const PathAttributeVerdict target_verdict = InspectPathNoFollow(canonical_target, nullptr);
    if (target_verdict == PathAttributeVerdict::ReparsePoint) {
        return BrokerStatus::ReparsePointRejected;
    }
    if (target_verdict == PathAttributeVerdict::NotFound) {
        return BrokerStatus::NotFound;
    }
    if (target_verdict != PathAttributeVerdict::OkFile) {
        return BrokerStatus::InvalidArgument;
    }

    // Reserve handle budget slot before opening/duplicating into the worker.
    uint32_t observed = brokered_handle_count_.load(std::memory_order_acquire);
    for (;;) {
        if (observed >= config_.max_brokered_handles) {
            return BrokerStatus::BudgetExceeded;
        }
        if (brokered_handle_count_.compare_exchange_weak(
                observed, observed + 1u, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }
    bool slot_committed = false;
    struct BudgetRollback {
        std::atomic<uint32_t> &counter;
        bool &committed;
        ~BudgetRollback() {
            if (!committed) {
                counter.fetch_sub(1u, std::memory_order_acq_rel);
            }
        }
    } rollback{brokered_handle_count_, slot_committed};

    UniqueHandle host_file(CreateFileW(
        canonical_target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!host_file.Valid()) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return BrokerStatus::NotFound;
        }
        if (err == ERROR_CANT_ACCESS_FILE) {
            return BrokerStatus::ReparsePointRejected;
        }
        return BrokerStatus::InvalidArgument;
    }

    BY_HANDLE_FILE_INFORMATION file_info{};
    if (!GetFileInformationByHandle(host_file.Get(), &file_info)) {
        return BrokerStatus::InternalError;
    }
    if ((file_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return BrokerStatus::ReparsePointRejected;
    }
    if ((file_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return BrokerStatus::InvalidArgument;
    }

    std::wstring final_file_path;
    if (!ResolveFinalHandlePath(host_file.Get(), final_file_path) ||
        !IsPathStrictlyUnderRoot(final_file_path, root_final_now)) {
        return BrokerStatus::PathOutsideRoot;
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(host_file.Get(), &file_size) || file_size.QuadPart < 0) {
        return BrokerStatus::InternalError;
    }

    HANDLE worker_handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (session_revoked_.load(std::memory_order_acquire)) {
            denied_call_count_.fetch_add(1, std::memory_order_acq_rel);
            return BrokerStatus::Revoked;
        }
        if (worker_process_ == nullptr) {
            return BrokerStatus::TransportError;
        }
        if (!DuplicateHandle(GetCurrentProcess(), host_file.Get(), worker_process_, &worker_handle,
                             GENERIC_READ, FALSE, 0)) {
            return BrokerStatus::InternalError;
        }
    }

    // Close host_file immediately so the host never holds a lock on the brokered file.
    host_file.Reset();
    slot_committed = true;

    result_out.worker_handle =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(worker_handle));
    result_out.file_size = static_cast<uint64_t>(file_size.QuadPart);
    return BrokerStatus::Ok;
}

BrokerStatus CapabilityBroker::DispatchBrokerFrame(const BrokerFrameHeader &req_header,
                                                   const std::vector<uint8_t> &req_payload,
                                                   BrokerOp &reply_op_out,
                                                   std::vector<uint8_t> &reply_payload_out) {
    reply_payload_out.clear();
    ByteReader reader(req_payload);
    ByteWriter writer;

    switch (static_cast<BrokerOp>(req_header.op)) {
    case BrokerOp::SettingsGet: {
        reply_op_out = BrokerOp::SettingsGetReply;
        std::wstring key;
        if (!reader.ReadWString(key) || !reader.Empty()) {
            return BrokerStatus::InvalidArgument;
        }
        std::string value;
        uint64_t version = 0;
        const BrokerStatus st = HandleSettingsGet(key, value, version);
        writer.WriteU64(version);
        if (!writer.WriteString(value)) {
            return BrokerStatus::InternalError;
        }
        reply_payload_out = writer.Take();
        return st;
    }
    case BrokerOp::SettingsSet: {
        reply_op_out = BrokerOp::SettingsSetReply;
        std::wstring key;
        uint64_t expected_version = 0;
        std::string value;
        if (!reader.ReadWString(key) || !reader.ReadU64(expected_version) ||
            !reader.ReadString(value) || !reader.Empty()) {
            return BrokerStatus::InvalidArgument;
        }
        uint64_t new_version = 0;
        const BrokerStatus st = HandleSettingsSet(key, value, expected_version, new_version);
        writer.WriteU64(new_version);
        reply_payload_out = writer.Take();
        return st;
    }
    case BrokerOp::ContextGet: {
        reply_op_out = BrokerOp::ContextGetReply;
        if (!reader.Empty()) {
            return BrokerStatus::InvalidArgument;
        }
        SanitizedContextSnapshot snap{};
        const BrokerStatus st = HandleContextGet(snap);
        if (!writer.WriteWString(snap.directory_label)) {
            return BrokerStatus::InternalError;
        }
        writer.WriteU32(snap.total_items);
        writer.WriteU32(snap.selected_items);
        reply_payload_out = writer.Take();
        return st;
    }
    case BrokerOp::OpenReadFile: {
        reply_op_out = BrokerOp::OpenReadFileReply;
        std::wstring path;
        if (!reader.ReadWString(path) || !reader.Empty()) {
            return BrokerStatus::InvalidArgument;
        }
        BrokerReadFileResult result{};
        const BrokerStatus st = HandleOpenReadFile(path, result);
        writer.WriteU64(result.worker_handle);
        writer.WriteU64(result.file_size);
        reply_payload_out = writer.Take();
        return st;
    }
    default:
        reply_op_out = BrokerOp::HostInvokeReply;
        return BrokerStatus::InvalidArgument;
    }
}

BrokerStatus CapabilityBroker::InvokeWorker(uint32_t command_op,
                                            const std::vector<uint8_t> &request_payload,
                                            std::vector<uint8_t> &response_payload,
                                            uint32_t timeout_ms) {
    response_payload.clear();
    if (session_revoked_.load(std::memory_order_acquire)) {
        denied_call_count_.fetch_add(1, std::memory_order_acq_rel);
        return BrokerStatus::Revoked;
    }

    const uint32_t effective_timeout =
        timeout_ms > 0 ? timeout_ms
                       : (config_.default_call_timeout_ms > 0 ? config_.default_call_timeout_ms
                                                              : 5000u);
    const ULONGLONG deadline = GetTickCount64() + effective_timeout;

    ByteWriter req_writer;
    req_writer.WriteU32(command_op);
    if (!req_writer.WriteBytes(request_payload.data(), request_payload.size())) {
        return BrokerStatus::InvalidArgument;
    }
    const std::vector<uint8_t> wire_req = req_writer.Take();
    const uint64_t invoke_req_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);

    bool kill_worker_for_timeout_or_protocol = false;
    BrokerStatus final_status = BrokerStatus::Ok;

    {
        std::lock_guard<std::mutex> io_lock(io_mutex_);
        if (session_revoked_.load(std::memory_order_acquire)) {
            denied_call_count_.fetch_add(1, std::memory_order_acq_rel);
            return BrokerStatus::Revoked;
        }
        if (host_pipe_ == INVALID_HANDLE_VALUE) {
            return BrokerStatus::TransportError;
        }

        final_status = WriteFrame(
            host_pipe_, io_event_, cancel_event_, BrokerOp::HostInvoke, invoke_req_id,
            BrokerStatus::Ok, wire_req, deadline,
            session_revoked_.load(std::memory_order_acquire));
        if (final_status != BrokerStatus::Ok) {
            kill_worker_for_timeout_or_protocol = (final_status == BrokerStatus::Timeout);
        } else {
            for (;;) {
                BrokerFrameHeader incoming_hdr{};
                std::vector<uint8_t> incoming_payload;
                final_status = ReadFrame(
                    host_pipe_, io_event_, cancel_event_, incoming_hdr, incoming_payload, deadline,
                    session_revoked_.load(std::memory_order_acquire));
                if (final_status != BrokerStatus::Ok) {
                    kill_worker_for_timeout_or_protocol =
                        (final_status == BrokerStatus::Timeout ||
                         final_status == BrokerStatus::InvalidArgument);
                    break;
                }

                if (incoming_hdr.op == static_cast<uint16_t>(BrokerOp::HostInvokeReply)) {
                    if (incoming_hdr.request_id != invoke_req_id) {
                        final_status = BrokerStatus::InvalidArgument;
                        kill_worker_for_timeout_or_protocol = true;
                        break;
                    }
                    ByteReader resp_reader(incoming_payload);
                    if (!resp_reader.ReadBytes(response_payload) || !resp_reader.Empty()) {
                        final_status = BrokerStatus::InvalidArgument;
                        kill_worker_for_timeout_or_protocol = true;
                        break;
                    }
                    final_status = static_cast<BrokerStatus>(incoming_hdr.status);
                    break;
                }

                BrokerOp reply_op = BrokerOp::HostInvokeReply;
                std::vector<uint8_t> reply_payload;
                const BrokerStatus op_status =
                    DispatchBrokerFrame(incoming_hdr, incoming_payload, reply_op, reply_payload);
                const BrokerStatus send_reply_status = WriteFrame(
                    host_pipe_, io_event_, cancel_event_, reply_op, incoming_hdr.request_id,
                    op_status, reply_payload, deadline,
                    session_revoked_.load(std::memory_order_acquire));
                if (send_reply_status != BrokerStatus::Ok) {
                    final_status = send_reply_status;
                    kill_worker_for_timeout_or_protocol =
                        (send_reply_status == BrokerStatus::Timeout);
                    break;
                }
            }
        }
    }

    if (kill_worker_for_timeout_or_protocol) {
        TerminateAndCleanupWorker(ERROR_TIMEOUT, false);
    }
    if (session_revoked_.load(std::memory_order_acquire) && final_status != BrokerStatus::Ok) {
        return BrokerStatus::Revoked;
    }
    return final_status;
}

// --- WorkerBrokerClient ---

WorkerBrokerClient::~WorkerBrokerClient() {
    Close();
}

bool WorkerBrokerClient::ParseCommandLineArgs(int argc, wchar_t **argv, HANDLE &pipe_out,
                                              uint8_t (&token_out)[kBrokerSessionTokenBytes]) {
    pipe_out = INVALID_HANDLE_VALUE;
    std::memset(token_out, 0, sizeof(token_out));
    bool have_pipe = false;
    bool have_token = false;

    for (int i = 1; i + 1 < argc; ++i) {
        const std::wstring_view flag(argv[i]);
        if (flag == L"--broker-pipe") {
            wchar_t *end_ptr = nullptr;
            const unsigned long long raw_val = std::wcstoull(argv[i + 1], &end_ptr, 10);
            if (end_ptr != nullptr && *end_ptr == L'\0' && raw_val != 0) {
                pipe_out = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(raw_val));
                have_pipe = true;
            }
            ++i;
        } else if (flag == L"--broker-token") {
            have_token = HexToBytesW(argv[i + 1], token_out, kBrokerSessionTokenBytes);
            ++i;
        }
    }
    return have_pipe && have_token && GetFileType(pipe_out) == FILE_TYPE_PIPE;
}

BrokerStatus WorkerBrokerClient::Connect(
    HANDLE inherited_pipe, const uint8_t (&session_token)[kBrokerSessionTokenBytes],
    uint32_t timeout_ms) {
    Close();
    if (inherited_pipe == nullptr || inherited_pipe == INVALID_HANDLE_VALUE ||
        GetFileType(inherited_pipe) != FILE_TYPE_PIPE) {
        return BrokerStatus::InvalidArgument;
    }
    io_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (io_event_ == nullptr) {
        return BrokerStatus::InternalError;
    }
    pipe_ = inherited_pipe;

    const ULONGLONG deadline = GetTickCount64() + (timeout_ms > 0 ? timeout_ms : 5000u);
    std::vector<uint8_t> hs_payload(session_token, session_token + kBrokerSessionTokenBytes);
    const uint64_t req_id = next_request_id_++;
    const BrokerStatus send_st = WriteFrame(
        pipe_, io_event_, nullptr, BrokerOp::Handshake, req_id, BrokerStatus::Ok, hs_payload,
        deadline);
    if (send_st != BrokerStatus::Ok) {
        Close();
        return send_st;
    }

    BrokerFrameHeader ack_hdr{};
    std::vector<uint8_t> ack_payload;
    const BrokerStatus recv_st =
        ReadFrame(pipe_, io_event_, nullptr, ack_hdr, ack_payload, deadline);
    if (recv_st != BrokerStatus::Ok) {
        Close();
        return recv_st;
    }
    if (ack_hdr.op != static_cast<uint16_t>(BrokerOp::HandshakeAck) ||
        ack_hdr.request_id != req_id) {
        Close();
        return BrokerStatus::InvalidArgument;
    }
    const auto ack_status = static_cast<BrokerStatus>(ack_hdr.status);
    if (ack_status != BrokerStatus::Ok) {
        Close();
    }
    return ack_status;
}

void WorkerBrokerClient::Close() noexcept {
    if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (io_event_ != nullptr) {
        CloseHandle(io_event_);
        io_event_ = nullptr;
    }
}

BrokerStatus WorkerBrokerClient::ReceiveHostInvoke(uint32_t &command_op_out,
                                                   uint64_t &request_id_out,
                                                   std::vector<uint8_t> &payload_out,
                                                   uint32_t timeout_ms) {
    command_op_out = 0;
    request_id_out = 0;
    payload_out.clear();
    const ULONGLONG deadline = GetTickCount64() + (timeout_ms > 0 ? timeout_ms : 30000u);

    BrokerFrameHeader hdr{};
    std::vector<uint8_t> raw_payload;
    const BrokerStatus st = ReadFrame(pipe_, io_event_, nullptr, hdr, raw_payload, deadline);
    if (st != BrokerStatus::Ok) {
        return st;
    }
    if (hdr.op != static_cast<uint16_t>(BrokerOp::HostInvoke)) {
        return BrokerStatus::InvalidArgument;
    }
    ByteReader reader(raw_payload);
    if (!reader.ReadU32(command_op_out) || !reader.ReadBytes(payload_out) || !reader.Empty()) {
        return BrokerStatus::InvalidArgument;
    }
    request_id_out = hdr.request_id;
    return BrokerStatus::Ok;
}

BrokerStatus WorkerBrokerClient::SendHostInvokeReply(uint64_t request_id, BrokerStatus status,
                                                     const std::vector<uint8_t> &payload,
                                                     uint32_t timeout_ms) {
    ByteWriter writer;
    if (!writer.WriteBytes(payload.data(), payload.size())) {
        return BrokerStatus::InvalidArgument;
    }
    const std::vector<uint8_t> wire = writer.Take();
    const ULONGLONG deadline = GetTickCount64() + (timeout_ms > 0 ? timeout_ms : 5000u);
    return WriteFrame(pipe_, io_event_, nullptr, BrokerOp::HostInvokeReply, request_id, status,
                      wire, deadline);
}

BrokerStatus WorkerBrokerClient::CallBroker(BrokerOp request_op, BrokerOp expected_reply_op,
                                            const std::vector<uint8_t> &request_payload,
                                            std::vector<uint8_t> &reply_payload_out,
                                            uint32_t timeout_ms) {
    reply_payload_out.clear();
    const ULONGLONG deadline = GetTickCount64() + (timeout_ms > 0 ? timeout_ms : 5000u);
    const uint64_t req_id = next_request_id_++;
    const BrokerStatus send_st = WriteFrame(
        pipe_, io_event_, nullptr, request_op, req_id, BrokerStatus::Ok, request_payload,
        deadline);
    if (send_st != BrokerStatus::Ok) {
        return send_st;
    }
    BrokerFrameHeader reply_hdr{};
    const BrokerStatus recv_st =
        ReadFrame(pipe_, io_event_, nullptr, reply_hdr, reply_payload_out, deadline);
    if (recv_st != BrokerStatus::Ok) {
        return recv_st;
    }
    if (reply_hdr.op != static_cast<uint16_t>(expected_reply_op) ||
        reply_hdr.request_id != req_id) {
        return BrokerStatus::InvalidArgument;
    }
    return static_cast<BrokerStatus>(reply_hdr.status);
}

BrokerStatus WorkerBrokerClient::SettingsGet(const std::wstring &key, std::string &value_out,
                                             uint64_t &version_out, uint32_t timeout_ms) {
    value_out.clear();
    version_out = 0;
    ByteWriter writer;
    if (!writer.WriteWString(key)) {
        return BrokerStatus::InvalidArgument;
    }
    std::vector<uint8_t> reply;
    const BrokerStatus st = CallBroker(
        BrokerOp::SettingsGet, BrokerOp::SettingsGetReply, writer.Take(), reply, timeout_ms);
    if (!reply.empty()) {
        ByteReader reader(reply);
        uint64_t parsed_ver = 0;
        std::string parsed_val;
        if (reader.ReadU64(parsed_ver) && reader.ReadString(parsed_val) && reader.Empty()) {
            version_out = parsed_ver;
            if (st == BrokerStatus::Ok) {
                value_out = std::move(parsed_val);
            }
        }
    }
    return st;
}

BrokerStatus WorkerBrokerClient::SettingsSet(const std::wstring &key, const std::string &value,
                                             uint64_t expected_version, uint64_t &new_version_out,
                                             uint32_t timeout_ms) {
    new_version_out = 0;
    ByteWriter writer;
    if (!writer.WriteWString(key)) {
        return BrokerStatus::InvalidArgument;
    }
    writer.WriteU64(expected_version);
    if (!writer.WriteString(value)) {
        return BrokerStatus::InvalidArgument;
    }
    std::vector<uint8_t> reply;
    const BrokerStatus st = CallBroker(
        BrokerOp::SettingsSet, BrokerOp::SettingsSetReply, writer.Take(), reply, timeout_ms);
    if (!reply.empty()) {
        ByteReader reader(reply);
        uint64_t parsed_ver = 0;
        if (reader.ReadU64(parsed_ver) && reader.Empty()) {
            new_version_out = parsed_ver;
        }
    }
    return st;
}

BrokerStatus WorkerBrokerClient::ContextGet(SanitizedContextSnapshot &snapshot_out,
                                            uint32_t timeout_ms) {
    snapshot_out = {};
    std::vector<uint8_t> empty;
    std::vector<uint8_t> reply;
    const BrokerStatus st =
        CallBroker(BrokerOp::ContextGet, BrokerOp::ContextGetReply, empty, reply, timeout_ms);
    if (st == BrokerStatus::Ok) {
        ByteReader reader(reply);
        if (!reader.ReadWString(snapshot_out.directory_label) ||
            !reader.ReadU32(snapshot_out.total_items) ||
            !reader.ReadU32(snapshot_out.selected_items) || !reader.Empty()) {
            return BrokerStatus::InvalidArgument;
        }
    }
    return st;
}

BrokerStatus WorkerBrokerClient::OpenReadFile(const std::wstring &path, HANDLE &file_handle_out,
                                              uint64_t &file_size_out, uint32_t timeout_ms) {
    file_handle_out = INVALID_HANDLE_VALUE;
    file_size_out = 0;
    ByteWriter writer;
    if (!writer.WriteWString(path)) {
        return BrokerStatus::InvalidArgument;
    }
    std::vector<uint8_t> reply;
    const BrokerStatus st = CallBroker(
        BrokerOp::OpenReadFile, BrokerOp::OpenReadFileReply, writer.Take(), reply, timeout_ms);
    if (st == BrokerStatus::Ok) {
        ByteReader reader(reply);
        uint64_t raw_handle = 0;
        uint64_t raw_size = 0;
        if (!reader.ReadU64(raw_handle) || !reader.ReadU64(raw_size) || !reader.Empty() ||
            raw_handle == 0) {
            return BrokerStatus::InvalidArgument;
        }
        file_handle_out = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(raw_handle));
        file_size_out = raw_size;
    }
    return st;
}

} // namespace pulse::extension_host
