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

#include "../extension_host/capability_broker.h"

#include <aclapi.h>
#include <atomic>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <sddl.h>
#include <string>
#include <string_view>
#include <vector>
#include <winioctl.h>

namespace {

using pulse::extension_host::BrokerCapability;
using pulse::extension_host::BrokerConfig;
using pulse::extension_host::BrokerStatus;
using pulse::extension_host::CapabilityBroker;
using pulse::extension_host::IsolationTier;
using pulse::extension_host::kBrokerSessionTokenBytes;
using pulse::extension_host::kSettingsVersionUnconditional;
using pulse::extension_host::SandboxMode;
using pulse::extension_host::SanitizedContextSnapshot;
using pulse::extension_host::WorkerBrokerClient;

enum class ChildCommand : uint32_t {
    InspectSandboxToken = 1,
    AttemptChildSpawn = 2,
    AttemptDirectFileRead = 3,
    BrokerReadAndTryWrite = 4,
    ExerciseAllDenied = 5,
    ExerciseSettingsAndContext = 6,
    CheckSettingsVsContextLive = 7,
    AttemptExcessMemoryAlloc = 8,
    DeliberateInfiniteSpin = 9,
};

class PayloadBuilder {
  public:
    void PutU32(uint32_t v) {
        const size_t pos = buf_.size();
        buf_.resize(pos + sizeof(v));
        std::memcpy(buf_.data() + pos, &v, sizeof(v));
    }
    void PutU64(uint64_t v) {
        const size_t pos = buf_.size();
        buf_.resize(pos + sizeof(v));
        std::memcpy(buf_.data() + pos, &v, sizeof(v));
    }
    void PutString(const std::string &s) {
        PutU32(static_cast<uint32_t>(s.size()));
        if (!s.empty()) {
            const size_t pos = buf_.size();
            buf_.resize(pos + s.size());
            std::memcpy(buf_.data() + pos, s.data(), s.size());
        }
    }
    void PutWString(const std::wstring &s) {
        PutU32(static_cast<uint32_t>(s.size()));
        if (!s.empty()) {
            const size_t bytes = s.size() * sizeof(wchar_t);
            const size_t pos = buf_.size();
            buf_.resize(pos + bytes);
            std::memcpy(buf_.data() + pos, s.data(), bytes);
        }
    }
    std::vector<uint8_t> Take() noexcept { return std::move(buf_); }

  private:
    std::vector<uint8_t> buf_;
};

class PayloadParser {
  public:
    explicit PayloadParser(const std::vector<uint8_t> &buf) noexcept
        : data_(buf.data()), size_(buf.size()) {}

    bool GetU32(uint32_t &out) noexcept {
        if (size_ - offset_ < sizeof(out)) {
            return false;
        }
        std::memcpy(&out, data_ + offset_, sizeof(out));
        offset_ += sizeof(out);
        return true;
    }
    bool GetU64(uint64_t &out) noexcept {
        if (size_ - offset_ < sizeof(out)) {
            return false;
        }
        std::memcpy(&out, data_ + offset_, sizeof(out));
        offset_ += sizeof(out);
        return true;
    }
    bool GetString(std::string &out) {
        uint32_t len = 0;
        if (!GetU32(len) || size_ - offset_ < len) {
            return false;
        }
        out.assign(reinterpret_cast<const char *>(data_ + offset_), len);
        offset_ += len;
        return true;
    }
    bool GetWString(std::wstring &out) {
        uint32_t chars = 0;
        if (!GetU32(chars) || size_ - offset_ < static_cast<size_t>(chars) * sizeof(wchar_t)) {
            return false;
        }
        out.resize(chars);
        if (chars > 0) {
            std::memcpy(out.data(), data_ + offset_, static_cast<size_t>(chars) * sizeof(wchar_t));
            offset_ += static_cast<size_t>(chars) * sizeof(wchar_t);
        }
        return true;
    }
    bool Done() const noexcept { return offset_ == size_; }

  private:
    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
    size_t offset_ = 0;
};

std::wstring SidToText(PSID sid) {
    LPWSTR text = nullptr;
    if (sid == nullptr || !ConvertSidToStringSidW(sid, &text)) {
        return {};
    }
    std::wstring result(text);
    LocalFree(text);
    return result;
}

std::wstring CurrentUserSidString() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<uint8_t> bytes(size);
    const BOOL ok = GetTokenInformation(token, TokenUser, bytes.data(), size, &size);
    CloseHandle(token);
    if (!ok || bytes.empty()) {
        return {};
    }
    return SidToText(reinterpret_cast<const TOKEN_USER *>(bytes.data())->User.Sid);
}

bool ApplyProtectedDacl(const std::wstring &path, const std::wstring &sddl) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        return false;
    }
    const BOOL ok = SetFileSecurityW(
        path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, sd);
    LocalFree(sd);
    return ok != FALSE;
}

bool WriteTextFile(const std::wstring &path, const std::string &content) {
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written,
                              nullptr);
    CloseHandle(file);
    return ok != FALSE && written == content.size();
}

bool ReadTextFileHost(const std::wstring &path, std::string &out) {
    out.clear();
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    char buf[4096]{};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buf, sizeof(buf), &read, nullptr);
    CloseHandle(file);
    if (!ok) {
        return false;
    }
    out.assign(buf, read);
    return true;
}

bool CreateDirectoryJunction(const std::wstring &junction_path, const std::wstring &target_dir) {
    DWORD full_len = GetFullPathNameW(target_dir.c_str(), 0, nullptr, nullptr);
    if (full_len == 0) {
        return false;
    }
    std::wstring full_target(full_len, L'\0');
    const DWORD written_len =
        GetFullPathNameW(target_dir.c_str(), full_len, full_target.data(), nullptr);
    if (written_len == 0 || written_len >= full_len) {
        return false;
    }
    full_target.resize(written_len);
    while (full_target.size() > 3 && full_target.back() == L'\\') {
        full_target.pop_back();
    }

    if (!CreateDirectoryW(junction_path.c_str(), nullptr)) {
        return false;
    }

    HANDLE dir_handle = CreateFileW(
        junction_path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (dir_handle == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(junction_path.c_str());
        return false;
    }

    struct MountPointReparseBufferHeader {
        DWORD ReparseTag;
        WORD ReparseDataLength;
        WORD Reserved;
        WORD SubstituteNameOffset;
        WORD SubstituteNameLength;
        WORD PrintNameOffset;
        WORD PrintNameLength;
    };
    static_assert(sizeof(MountPointReparseBufferHeader) == 16);

    const std::wstring sub_name = L"\\??\\" + full_target;
    const std::wstring &print_name = full_target;
    const WORD sub_bytes = static_cast<WORD>(sub_name.size() * sizeof(wchar_t));
    const WORD print_bytes = static_cast<WORD>(print_name.size() * sizeof(wchar_t));
    const WORD reparse_data_len =
        static_cast<WORD>(8u + sub_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t));
    const DWORD total_bytes = 8u + reparse_data_len;

    std::vector<uint8_t> buffer(total_bytes, 0);
    auto *hdr = reinterpret_cast<MountPointReparseBufferHeader *>(buffer.data());
    hdr->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    hdr->ReparseDataLength = reparse_data_len;
    hdr->Reserved = 0;
    hdr->SubstituteNameOffset = 0;
    hdr->SubstituteNameLength = sub_bytes;
    hdr->PrintNameOffset = static_cast<WORD>(sub_bytes + sizeof(wchar_t));
    hdr->PrintNameLength = print_bytes;

    auto *path_buf = reinterpret_cast<wchar_t *>(buffer.data() + sizeof(MountPointReparseBufferHeader));
    std::memcpy(path_buf, sub_name.data(), sub_bytes);
    path_buf[sub_name.size()] = L'\0';
    std::memcpy(reinterpret_cast<uint8_t *>(path_buf) + hdr->PrintNameOffset, print_name.data(),
                print_bytes);

    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(
        dir_handle, FSCTL_SET_REPARSE_POINT, buffer.data(), total_bytes, nullptr, 0,
        &bytes_returned, nullptr);
    CloseHandle(dir_handle);
    if (!ok) {
        RemoveDirectoryW(junction_path.c_str());
        return false;
    }
    return true;
}

std::wstring RandomHex64() {
    uint8_t bytes[8]{};
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(16);
    for (uint8_t b : bytes) {
        out.push_back(kHex[(b >> 4) & 0x0Fu]);
        out.push_back(kHex[b & 0x0Fu]);
    }
    return out;
}

// --- Sandboxed Child Entrypoint ---

int RunSandboxChild(int argc, wchar_t **argv) {
    HANDLE pipe_handle = INVALID_HANDLE_VALUE;
    uint8_t session_token[kBrokerSessionTokenBytes]{};
    if (!WorkerBrokerClient::ParseCommandLineArgs(argc, argv, pipe_handle, session_token)) {
        return 11;
    }

    WorkerBrokerClient client;
    if (client.Connect(pipe_handle, session_token, 5000) != BrokerStatus::Ok) {
        return 12;
    }

    std::vector<HANDLE> held_handles;
    const BrokerStatus loop_status = client.RunDispatchLoop(
        [&](uint32_t raw_cmd, const std::vector<uint8_t> &req,
            std::vector<uint8_t> &resp) -> BrokerStatus {
            PayloadParser parser(req);
            PayloadBuilder builder;

            switch (static_cast<ChildCommand>(raw_cmd)) {
            case ChildCommand::InspectSandboxToken: {
                std::wstring expected_user_sid;
                if (!parser.GetWString(expected_user_sid) || !parser.Done()) {
                    return BrokerStatus::InvalidArgument;
                }
                HANDLE tok = nullptr;
                if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
                    return BrokerStatus::InternalError;
                }
                DWORD is_ac = 0;
                DWORD ret_len = 0;
                GetTokenInformation(tok, TokenIsAppContainer, &is_ac, sizeof(is_ac), &ret_len);

                uint32_t cap_count = 0;
                if (is_ac != 0) {
                    DWORD cap_bytes = 0;
                    GetTokenInformation(tok, TokenCapabilities, nullptr, 0, &cap_bytes);
                    std::vector<uint8_t> cap_buf(cap_bytes);
                    if (GetTokenInformation(
                            tok, TokenCapabilities, cap_buf.data(), cap_bytes, &cap_bytes)) {
                        cap_count =
                            reinterpret_cast<const TOKEN_GROUPS *>(cap_buf.data())->GroupCount;
                    } else {
                        cap_count = 999u;
                    }
                }

                const uint32_t is_restricted = IsTokenRestricted(tok) != FALSE ? 1u : 0u;

                DWORD il_bytes = 0;
                GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &il_bytes);
                std::vector<uint8_t> il_buf(il_bytes);
                uint32_t il_rid = 0xFFFFFFFFu;
                if (GetTokenInformation(
                        tok, TokenIntegrityLevel, il_buf.data(), il_bytes, &il_bytes)) {
                    const auto *tml =
                        reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(il_buf.data());
                    const UCHAR sub_cnt = *GetSidSubAuthorityCount(tml->Label.Sid);
                    if (sub_cnt > 0) {
                        il_rid = *GetSidSubAuthority(tml->Label.Sid, sub_cnt - 1);
                    }
                }

                uint32_t restricted_count = 0;
                uint32_t user_sid_in_restricted = 0;
                DWORD rs_bytes = 0;
                GetTokenInformation(tok, TokenRestrictedSids, nullptr, 0, &rs_bytes);
                if (rs_bytes > 0) {
                    std::vector<uint8_t> rs_buf(rs_bytes);
                    if (GetTokenInformation(
                            tok, TokenRestrictedSids, rs_buf.data(), rs_bytes, &rs_bytes)) {
                        const auto *rs_groups =
                            reinterpret_cast<const TOKEN_GROUPS *>(rs_buf.data());
                        restricted_count = rs_groups->GroupCount;
                        for (DWORD i = 0; i < rs_groups->GroupCount; ++i) {
                            if (SidToText(rs_groups->Groups[i].Sid) == expected_user_sid) {
                                user_sid_in_restricted = 1u;
                            }
                        }
                    }
                }
                CloseHandle(tok);

                wchar_t env_buf[256]{};
                const uint32_t has_userprofile =
                    GetEnvironmentVariableW(L"USERPROFILE", env_buf, 256) > 0 ? 1u : 0u;
                const uint32_t has_sysroot =
                    GetEnvironmentVariableW(L"SystemRoot", env_buf, 256) > 0 ? 1u : 0u;

                builder.PutU32(is_ac != 0 ? 1u : 0u);
                builder.PutU32(cap_count);
                builder.PutU32(is_restricted);
                builder.PutU32(il_rid);
                builder.PutU32(restricted_count);
                builder.PutU32(user_sid_in_restricted);
                builder.PutU32(has_userprofile);
                builder.PutU32(has_sysroot);
                resp = builder.Take();
                return BrokerStatus::Ok;
            }
            case ChildCommand::AttemptChildSpawn: {
                PROCESS_MITIGATION_CHILD_PROCESS_POLICY policy{};
                const BOOL got_policy = GetProcessMitigationPolicy(
                    GetCurrentProcess(), ProcessChildProcessPolicy, &policy, sizeof(policy));
                const uint32_t mitigation_active =
                    (got_policy && policy.NoChildProcessCreation) ? 1u : 0u;

                wchar_t self_path[MAX_PATH]{};
                GetModuleFileNameW(nullptr, self_path, MAX_PATH);
                std::wstring cmd = std::wstring(L"\"") + self_path + L"\" --noop";
                STARTUPINFOW si{};
                si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                uint32_t spawn_blocked = 0;
                uint32_t spawn_err = 0;
                if (CreateProcessW(self_path, cmd.data(), nullptr, nullptr, FALSE,
                                   DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
                    TerminateProcess(pi.hProcess, 1);
                    WaitForSingleObject(pi.hProcess, 2000);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    spawn_blocked = 0;
                } else {
                    spawn_blocked = 1;
                    spawn_err = GetLastError();
                }
                builder.PutU32(mitigation_active);
                builder.PutU32(spawn_blocked);
                builder.PutU32(spawn_err);
                resp = builder.Take();
                return BrokerStatus::Ok;
            }
            case ChildCommand::AttemptDirectFileRead: {
                std::wstring path;
                if (!parser.GetWString(path) || !parser.Done()) {
                    return BrokerStatus::InvalidArgument;
                }
                HANDLE f = CreateFileW(
                    path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    CloseHandle(f);
                    builder.PutU32(1u);
                    builder.PutU32(ERROR_SUCCESS);
                } else {
                    builder.PutU32(0u);
                    builder.PutU32(GetLastError());
                }
                resp = builder.Take();
                return BrokerStatus::Ok;
            }
            case ChildCommand::BrokerReadAndTryWrite: {
                std::wstring path;
                uint32_t keep_open = 0;
                if (!parser.GetWString(path) || !parser.GetU32(keep_open) || !parser.Done()) {
                    return BrokerStatus::InvalidArgument;
                }
                HANDLE dup_handle = INVALID_HANDLE_VALUE;
                uint64_t file_size = 0;
                const BrokerStatus st = client.OpenReadFile(path, dup_handle, file_size);
                builder.PutU32(static_cast<uint32_t>(st));
                builder.PutU64(file_size);
                if (st != BrokerStatus::Ok) {
                    builder.PutU32(0u);
                    builder.PutU32(0u);
                    builder.PutString({});
                    resp = builder.Take();
                    return BrokerStatus::Ok;
                }

                std::string content(static_cast<size_t>(file_size), '\0');
                DWORD bytes_read = 0;
                const BOOL read_ok =
                    file_size == 0 ||
                    (ReadFile(dup_handle, content.data(), static_cast<DWORD>(file_size),
                              &bytes_read, nullptr) &&
                     bytes_read == file_size);
                if (!read_ok) {
                    content.clear();
                }

                DWORD written = 0;
                const char probe_byte = 'Z';
                const BOOL write_ok = WriteFile(dup_handle, &probe_byte, 1, &written, nullptr);
                const uint32_t write_denied = write_ok ? 0u : 1u;
                const uint32_t write_err = write_ok ? ERROR_SUCCESS : GetLastError();

                if (keep_open != 0) {
                    held_handles.push_back(dup_handle);
                } else {
                    CloseHandle(dup_handle);
                }

                builder.PutU32(write_denied);
                builder.PutU32(write_err);
                builder.PutString(content);
                resp = builder.Take();
                return BrokerStatus::Ok;
            }
            case ChildCommand::ExerciseAllDenied: {
                std::wstring path;
                if (!parser.GetWString(path) || !parser.Done()) {
                    return BrokerStatus::InvalidArgument;
                }
                std::string val;
                uint64_t ver = 0;
                SanitizedContextSnapshot snap{};
                HANDLE h = INVALID_HANDLE_VALUE;
                uint64_t sz = 0;
                const BrokerStatus s1 = client.SettingsGet(L"k", val, ver);
                const BrokerStatus s2 = client.SettingsSet(L"k", "v", 0, ver);
                const BrokerStatus s3 = client.ContextGet(snap);
                const BrokerStatus s4 = client.OpenReadFile(path, h, sz);
                if (h != INVALID_HANDLE_VALUE) {
                    CloseHandle(h);
                }
                builder.PutU32(static_cast<uint32_t>(s1));
                builder.PutU32(static_cast<uint32_t>(s2));
                builder.PutU32(static_cast<uint32_t>(s3));
                builder.PutU32(static_cast<uint32_t>(s4));
                resp = builder.Take();
                return BrokerStatus::Ok;
            }
            case ChildCommand::ExerciseSettingsAndContext: {
                SanitizedContextSnapshot snap{};
                const BrokerStatus ctx_st = client.ContextGet(snap);

                std::string val;
                uint64_t ver = 99;
                const BrokerStatus get_missing = client.SettingsGet(L"theme", val, ver);

                uint64_t v1 = 0;
                const BrokerStatus create_ok = client.SettingsSet(L"theme", "dark", 0, v1);

                uint64_t v_dup = 0;
                const BrokerStatus create_conflict = client.SettingsSet(L"theme", "light", 0, v_dup);

                uint64_t v2 = 0;
                const BrokerStatus cas_ok = client.SettingsSet(L"theme", "light", 1, v2);

                uint64_t v_stale = 0;
                const BrokerStatus cas_conflict = client.SettingsSet(L"theme", "stale", 1, v_stale);

                uint64_t v3 = 0;
                const BrokerStatus upsert_ok =
                    client.SettingsSet(L"theme", "forced", kSettingsVersionUnconditional, v3);

                std::string final_val;
                uint64_t final_ver = 0;
                const BrokerStatus get_final = client.SettingsGet(L"theme", final_val, final_ver);

                uint64_t v_over = 0;
                const std::string oversized(65537, 'x');
                const BrokerStatus oversize_st =
                    client.SettingsSet(L"theme", oversized, kSettingsVersionUnconditional, v_over);

                const bool all_ok =
                    ctx_st == BrokerStatus::Ok && snap.directory_label == L"WorkspaceRoot" &&
                    snap.total_items == 128u && snap.selected_items == 5u &&
                    get_missing == BrokerStatus::NotFound &&
                    create_ok == BrokerStatus::Ok && v1 == 1u &&
                    create_conflict == BrokerStatus::Conflict && v_dup == 1u &&
                    cas_ok == BrokerStatus::Ok && v2 == 2u &&
                    cas_conflict == BrokerStatus::Conflict && v_stale == 2u &&
                    upsert_ok == BrokerStatus::Ok && v3 == 3u &&
                    get_final == BrokerStatus::Ok && final_val == "forced" && final_ver == 3u &&
                    oversize_st == BrokerStatus::InvalidArgument;

                builder.PutU32(all_ok ? 1u : 0u);
                resp = builder.Take();
                return BrokerStatus::Ok;
            }
            case ChildCommand::CheckSettingsVsContextLive: {
                std::string val;
                uint64_t ver = 0;
                const BrokerStatus get_st = client.SettingsGet(L"theme", val, ver);
                const BrokerStatus set_st = client.SettingsSet(
                    L"theme", "mutated", kSettingsVersionUnconditional, ver);
                SanitizedContextSnapshot snap{};
                const BrokerStatus ctx_st = client.ContextGet(snap);
                builder.PutU32(static_cast<uint32_t>(get_st));
                builder.PutU32(static_cast<uint32_t>(set_st));
                builder.PutU32(static_cast<uint32_t>(ctx_st));
                builder.PutU32(snap.total_items);
                resp = builder.Take();
                return BrokerStatus::Ok;
            }
            case ChildCommand::AttemptExcessMemoryAlloc: {
                uint64_t bytes_to_alloc = 0;
                if (!parser.GetU64(bytes_to_alloc) || !parser.Done()) {
                    return BrokerStatus::InvalidArgument;
                }
                void *mem = VirtualAlloc(
                    nullptr, static_cast<SIZE_T>(bytes_to_alloc), MEM_RESERVE | MEM_COMMIT,
                    PAGE_READWRITE);
                if (mem != nullptr) {
                    VirtualFree(mem, 0, MEM_RELEASE);
                    builder.PutU32(1u);
                    builder.PutU32(ERROR_SUCCESS);
                } else {
                    builder.PutU32(0u);
                    builder.PutU32(GetLastError());
                }
                resp = builder.Take();
                return BrokerStatus::Ok;
            }
            case ChildCommand::DeliberateInfiniteSpin: {
                std::atomic<uint64_t> counter{0};
                for (;;) {
                    counter.fetch_add(1, std::memory_order_relaxed);
                }
            }
            default:
                return BrokerStatus::InvalidArgument;
            }
        });

    for (HANDLE h : held_handles) {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
    return loop_status == BrokerStatus::TransportError ? 0 : 1;
}

int RunTamperHandshakeChild(int argc, wchar_t **argv) {
    HANDLE pipe_handle = INVALID_HANDLE_VALUE;
    uint8_t session_token[kBrokerSessionTokenBytes]{};
    if (!WorkerBrokerClient::ParseCommandLineArgs(argc, argv, pipe_handle, session_token)) {
        return 21;
    }
    session_token[0] ^= 0xFFu;
    WorkerBrokerClient client;
    const BrokerStatus st = client.Connect(pipe_handle, session_token, 3000);
    return st == BrokerStatus::AuthFailed ? 0 : 22;
}

struct TestSuiteCounters {
    int passed = 0;
    int failed = 0;

    void Check(bool condition, const char *name, const char *detail = nullptr) {
        if (condition) {
            ++passed;
            std::printf("[PASS] %s\n", name);
        } else {
            ++failed;
            if (detail != nullptr && detail[0] != '\0') {
                std::printf("[FAIL] %s (%s)\n", name, detail);
            } else {
                std::printf("[FAIL] %s\n", name);
            }
        }
        std::fflush(stdout);
    }

    void CheckBroker(bool condition, const char *name, const CapabilityBroker &broker) {
        if (condition) {
            Check(true, name);
            return;
        }
        char buf[256]{};
        std::snprintf(buf, sizeof(buf), "stage=%ls error=%lu/0x%08lx",
                      broker.LastErrorStage().c_str(), broker.LastError(), broker.LastError());
        Check(false, name, buf);
    }
};

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc >= 2) {
        const std::wstring_view mode(argv[1]);
        if (mode == L"--noop") {
            return 0;
        }
        if (mode == L"--sandbox-child") {
            return RunSandboxChild(argc, argv);
        }
        if (mode == L"--sandbox-tamper-token") {
            return RunTamperHandshakeChild(argc, argv);
        }
    }

    TestSuiteCounters suite;

    wchar_t self_exe_buf[32768]{};
    const DWORD self_len = GetModuleFileNameW(nullptr, self_exe_buf, 32768);
    if (self_len == 0 || self_len >= 32768) {
        std::printf("[FAIL] resolve self executable path\n");
        return 1;
    }
    const std::wstring self_exe(self_exe_buf, self_len);
    const std::wstring user_sid = CurrentUserSidString();
    if (user_sid.empty()) {
        std::printf("[FAIL] query current user SID\n");
        return 1;
    }

    CreateDirectoryW(L"bench_data", nullptr);
    DWORD bench_len = GetFullPathNameW(L"bench_data", 0, nullptr, nullptr);
    std::wstring bench_root(bench_len, L'\0');
    bench_len = GetFullPathNameW(L"bench_data", bench_len, bench_root.data(), nullptr);
    bench_root.resize(bench_len);
    while (bench_root.size() > 3 && bench_root.back() == L'\\') {
        bench_root.pop_back();
    }

    const std::wstring fixture_root = bench_root + L"\\tmp_broker_run_" + RandomHex64();
    const std::wstring worker_dir = fixture_root + L"\\worker";
    const std::wstring worker_exe = worker_dir + L"\\extension_broker_worker.exe";
    const std::wstring secret_dir = fixture_root + L"\\secret";
    const std::wstring secret_file = secret_dir + L"\\secret.txt";
    const std::wstring allowed_dir = fixture_root + L"\\allowed";
    const std::wstring allowed_readme = allowed_dir + L"\\readme.txt";
    const std::wstring allowed_junction = allowed_dir + L"\\link_to_secret";

    const std::wstring owner_only_sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;" + user_sid + L")";
    const std::wstring worker_exec_sddl =
        L"D:P(A;;FA;;;SY)(A;;FA;;;" + user_sid + L")(A;;GRGX;;;WD)(A;;GRGX;;;AC)";

    struct FixtureCleanupGuard {
        std::wstring fixture_root;
        std::wstring worker_dir;
        std::wstring worker_exe;
        std::wstring secret_dir;
        std::wstring secret_file;
        std::wstring allowed_dir;
        std::wstring allowed_readme;
        std::wstring allowed_junction;
        std::wstring owner_sddl;

        ~FixtureCleanupGuard() {
            // Remove junction link first without recursing into its target.
            RemoveDirectoryW(allowed_junction.c_str());
            ApplyProtectedDacl(allowed_readme, owner_sddl);
            ApplyProtectedDacl(secret_file, owner_sddl);
            ApplyProtectedDacl(worker_exe, owner_sddl);
            ApplyProtectedDacl(allowed_dir, owner_sddl);
            ApplyProtectedDacl(secret_dir, owner_sddl);
            ApplyProtectedDacl(worker_dir, owner_sddl);
            ApplyProtectedDacl(fixture_root, owner_sddl);
            DeleteFileW(allowed_readme.c_str());
            DeleteFileW(secret_file.c_str());
            DeleteFileW(worker_exe.c_str());
            RemoveDirectoryW(allowed_dir.c_str());
            RemoveDirectoryW(secret_dir.c_str());
            RemoveDirectoryW(worker_dir.c_str());
            RemoveDirectoryW(fixture_root.c_str());
        }
    } cleanup_guard{fixture_root, worker_dir,  worker_exe,     secret_dir,       secret_file,
                    allowed_dir,  allowed_readme, allowed_junction, owner_only_sddl};

    const std::string kSecretContent = "top-secret-user-material-99182";
    const std::string kAllowedContent = "brokered-readonly-readme-content-2026";

    const bool fixture_ok =
        CreateDirectoryW(fixture_root.c_str(), nullptr) &&
        ApplyProtectedDacl(fixture_root, owner_only_sddl) &&
        CreateDirectoryW(worker_dir.c_str(), nullptr) &&
        ApplyProtectedDacl(worker_dir, worker_exec_sddl) &&
        CopyFileW(self_exe.c_str(), worker_exe.c_str(), FALSE) &&
        ApplyProtectedDacl(worker_exe, worker_exec_sddl) &&
        CreateDirectoryW(secret_dir.c_str(), nullptr) &&
        ApplyProtectedDacl(secret_dir, owner_only_sddl) &&
        WriteTextFile(secret_file, kSecretContent) &&
        ApplyProtectedDacl(secret_file, owner_only_sddl) &&
        CreateDirectoryW(allowed_dir.c_str(), nullptr) &&
        ApplyProtectedDacl(allowed_dir, owner_only_sddl) &&
        WriteTextFile(allowed_readme, kAllowedContent) &&
        ApplyProtectedDacl(allowed_readme, owner_only_sddl) &&
        CreateDirectoryJunction(allowed_junction, secret_dir);

    suite.Check(fixture_ok, "Fixture creation with protected owner-only DACLs and NTFS junction");
    if (!fixture_ok) {
        std::printf("Summary: %d passed, %d failed\n", suite.passed, suite.failed);
        return 1;
    }

    // --- Group 1: Sandbox Token, Modes & Containment ---
    {
        BrokerConfig ac_cfg{};
        ac_cfg.package_id = L"pkg.ac";
        ac_cfg.executable_path = worker_exe;
        ac_cfg.extra_arguments = L"--sandbox-child";
        ac_cfg.sandbox_mode = SandboxMode::RequireAppContainer;

        CapabilityBroker ac_broker;
        const bool ac_started = ac_broker.Start(ac_cfg);
        if (ac_started) {
            PayloadBuilder req;
            req.PutWString(user_sid);
            std::vector<uint8_t> resp;
            const BrokerStatus st = ac_broker.InvokeWorker(
                static_cast<uint32_t>(ChildCommand::InspectSandboxToken), req.Take(), resp);
            PayloadParser p(resp);
            uint32_t is_ac = 0, cap_cnt = 99;
            const bool parsed = p.GetU32(is_ac) && p.GetU32(cap_cnt);
            suite.Check(
                ac_broker.ActiveIsolationTier() == IsolationTier::AppContainer &&
                    st == BrokerStatus::Ok && parsed && is_ac == 1u && cap_cnt == 0u,
                "RequireAppContainer launches genuine zero-capability AppContainer");
            ac_broker.RevokeSession();
        } else {
            suite.Check(
                ac_broker.ActiveIsolationTier() == IsolationTier::None &&
                    !ac_broker.IsWorkerAlive() && !ac_broker.LastErrorStage().empty() &&
                    ac_broker.LastError() != 0,
                "RequireAppContainer fails closed without silent downgrade when profile unavailable");
        }
    }

    {
        BrokerConfig auto_cfg{};
        auto_cfg.package_id = L"pkg.auto";
        auto_cfg.executable_path = worker_exe;
        auto_cfg.extra_arguments = L"--sandbox-child";
        auto_cfg.sandbox_mode = SandboxMode::AppContainerOrRestrictedToken;

        CapabilityBroker auto_broker;
        const bool started = auto_broker.Start(auto_cfg);
        suite.CheckBroker(
            started &&
                (auto_broker.ActiveIsolationTier() == IsolationTier::AppContainer ||
                 auto_broker.ActiveIsolationTier() == IsolationTier::RestrictedToken) &&
                auto_broker.IsWorkerAlive(),
            "AppContainerOrRestrictedToken launches sandboxed worker cleanly", auto_broker);
        auto_broker.RevokeSession();
    }

    {
        BrokerConfig tamper_cfg{};
        tamper_cfg.package_id = L"pkg.tamper";
        tamper_cfg.executable_path = worker_exe;
        tamper_cfg.extra_arguments = L"--sandbox-tamper-token";
        tamper_cfg.sandbox_mode = SandboxMode::RequireRestrictedToken;

        CapabilityBroker tamper_broker;
        const bool started = tamper_broker.Start(tamper_cfg);
        suite.CheckBroker(
            !started && !tamper_broker.IsWorkerAlive() &&
                tamper_broker.LastErrorStage() == L"Handshake-AuthFailed",
            "CSPRNG session handshake rejects tampered worker token and reaps worker",
            tamper_broker);
    }

    {
        BrokerConfig rt_cfg{};
        rt_cfg.package_id = L"pkg.rt";
        rt_cfg.executable_path = worker_exe;
        rt_cfg.extra_arguments = L"--sandbox-child";
        rt_cfg.sandbox_mode = SandboxMode::RequireRestrictedToken;

        CapabilityBroker rt_broker;
        const bool started = rt_broker.Start(rt_cfg);
        suite.CheckBroker(
            started && rt_broker.ActiveIsolationTier() == IsolationTier::RestrictedToken,
            "RequireRestrictedToken starts worker in RestrictedToken tier", rt_broker);

        if (started) {
            PayloadBuilder req;
            req.PutWString(user_sid);
            std::vector<uint8_t> resp;
            const BrokerStatus st = rt_broker.InvokeWorker(
                static_cast<uint32_t>(ChildCommand::InspectSandboxToken), req.Take(), resp);
            PayloadParser p(resp);
            uint32_t is_ac = 0, cap_cnt = 0, is_restricted = 0, il_rid = 0;
            uint32_t restricted_cnt = 0, user_in_restricted = 1, has_profile = 1, has_sysroot = 0;
            const bool ok_parse =
                p.GetU32(is_ac) && p.GetU32(cap_cnt) && p.GetU32(is_restricted) &&
                p.GetU32(il_rid) && p.GetU32(restricted_cnt) && p.GetU32(user_in_restricted) &&
                p.GetU32(has_profile) && p.GetU32(has_sysroot) && p.Done();

            suite.Check(
                st == BrokerStatus::Ok && ok_parse && is_restricted == 1u &&
                    il_rid == SECURITY_MANDATORY_LOW_RID && restricted_cnt > 0u &&
                    user_in_restricted == 0u && has_profile == 0u && has_sysroot == 1u,
                "Restricted worker runs at Low Integrity (S-1-16-4096) with restricting SIDs omitting UserSid and sanitized env");

            std::vector<uint8_t> empty_req;
            std::vector<uint8_t> spawn_resp;
            const BrokerStatus spawn_st = rt_broker.InvokeWorker(
                static_cast<uint32_t>(ChildCommand::AttemptChildSpawn), empty_req, spawn_resp);
            PayloadParser sp(spawn_resp);
            uint32_t mitigation_on = 0, spawn_blocked = 0, spawn_err = 0;
            const bool sp_ok =
                sp.GetU32(mitigation_on) && sp.GetU32(spawn_blocked) && sp.GetU32(spawn_err) &&
                sp.Done();
            suite.Check(
                spawn_st == BrokerStatus::Ok && sp_ok && mitigation_on == 1u &&
                    spawn_blocked == 1u &&
                    (spawn_err == ERROR_CHILD_PROCESS_BLOCKED || spawn_err == ERROR_ACCESS_DENIED),
                "Child process creation inside sandbox blocked by PROCESS_CREATION_CHILD_PROCESS_RESTRICTED");
        }
        rt_broker.RevokeSession();
    }

    // --- Group 2: Kernel Filesystem Isolation vs DuplicateHandle Brokering ---
    {
        std::string host_secret;
        suite.Check(
            ReadTextFileHost(secret_file, host_secret) && host_secret == kSecretContent,
            "Host reads protected secret\\secret.txt directly");

        BrokerConfig fs_cfg{};
        fs_cfg.package_id = L"pkg.fs";
        fs_cfg.executable_path = worker_exe;
        fs_cfg.extra_arguments = L"--sandbox-child";
        fs_cfg.sandbox_mode = SandboxMode::RequireRestrictedToken;
        fs_cfg.initial_capabilities = BrokerCapability::ReadAllowedRoots;
        fs_cfg.allowed_read_roots = {allowed_dir};

        CapabilityBroker fs_broker;
        const bool started = fs_broker.Start(fs_cfg);
        suite.Check(started, "Filesystem test broker session started");

        if (started) {
            auto check_direct_denied = [&](const std::wstring &target_path, const char *label) {
                PayloadBuilder req;
                req.PutWString(target_path);
                std::vector<uint8_t> resp;
                const BrokerStatus st = fs_broker.InvokeWorker(
                    static_cast<uint32_t>(ChildCommand::AttemptDirectFileRead), req.Take(), resp);
                PayloadParser p(resp);
                uint32_t opened = 1, win32_err = 0;
                const bool ok = p.GetU32(opened) && p.GetU32(win32_err) && p.Done();
                suite.Check(
                    st == BrokerStatus::Ok && ok && opened == 0u &&
                        win32_err == ERROR_ACCESS_DENIED,
                    label);
            };

            check_direct_denied(
                secret_file,
                "Worker direct CreateFileW on secret\\secret.txt kernel-denied (ERROR_ACCESS_DENIED)");
            check_direct_denied(
                allowed_readme,
                "Worker direct CreateFileW on allowed\\readme.txt kernel-denied (ERROR_ACCESS_DENIED)");

            PayloadBuilder req;
            req.PutWString(allowed_readme);
            req.PutU32(0u);
            std::vector<uint8_t> resp;
            const BrokerStatus st = fs_broker.InvokeWorker(
                static_cast<uint32_t>(ChildCommand::BrokerReadAndTryWrite), req.Take(), resp);
            PayloadParser p(resp);
            uint32_t broker_st = 99, write_denied = 0, write_err = 0;
            uint64_t file_sz = 0;
            std::string read_content;
            const bool parsed =
                p.GetU32(broker_st) && p.GetU64(file_sz) && p.GetU32(write_denied) &&
                p.GetU32(write_err) && p.GetString(read_content) && p.Done();

            suite.Check(
                st == BrokerStatus::Ok && parsed &&
                    static_cast<BrokerStatus>(broker_st) == BrokerStatus::Ok &&
                    file_sz == kAllowedContent.size() && read_content == kAllowedContent,
                "Broker OpenReadFile duplicates read-only HANDLE and worker reads exact content");
            suite.Check(
                st == BrokerStatus::Ok && parsed && write_denied == 1u &&
                    write_err == ERROR_ACCESS_DENIED,
                "Worker WriteFile on duplicated read-only HANDLE fails with ERROR_ACCESS_DENIED");
        }
        fs_broker.RevokeSession();
    }

    // --- Group 3: Path Escape, Reparse Point & Handle Budget Enforcement ---
    {
        BrokerConfig path_cfg{};
        path_cfg.package_id = L"pkg.paths";
        path_cfg.executable_path = worker_exe;
        path_cfg.extra_arguments = L"--sandbox-child";
        path_cfg.sandbox_mode = SandboxMode::AppContainerOrRestrictedToken;
        path_cfg.initial_capabilities = BrokerCapability::ReadAllowedRoots;
        path_cfg.allowed_read_roots = {allowed_dir};
        path_cfg.max_brokered_handles = 3;

        CapabilityBroker path_broker;
        const bool started = path_broker.Start(path_cfg);
        suite.Check(started, "Path & budget test broker session started");

        if (started) {
            auto try_broker_open = [&](const std::wstring &candidate) -> BrokerStatus {
                PayloadBuilder req;
                req.PutWString(candidate);
                req.PutU32(0u);
                std::vector<uint8_t> resp;
                const BrokerStatus rpc_st = path_broker.InvokeWorker(
                    static_cast<uint32_t>(ChildCommand::BrokerReadAndTryWrite), req.Take(), resp);
                if (rpc_st != BrokerStatus::Ok) {
                    return rpc_st;
                }
                PayloadParser p(resp);
                uint32_t op_st = 99;
                if (!p.GetU32(op_st)) {
                    return BrokerStatus::InternalError;
                }
                return static_cast<BrokerStatus>(op_st);
            };

            suite.Check(
                try_broker_open(secret_file) == BrokerStatus::PathOutsideRoot,
                "OpenReadFile rejects secret\\secret.txt outside allowed_read_roots");
            suite.Check(
                try_broker_open(allowed_dir + L"\\..\\secret\\secret.txt") ==
                    BrokerStatus::InvalidArgument,
                "OpenReadFile rejects '..' path traversal (allowed\\..\\secret\\secret.txt)");
            suite.Check(
                try_broker_open(allowed_readme + L":stream") == BrokerStatus::InvalidArgument &&
                    try_broker_open(L"readme.txt") == BrokerStatus::InvalidArgument &&
                    try_broker_open(L"\\\\.\\pipe\\test") == BrokerStatus::InvalidArgument,
                "OpenReadFile rejects ADS ':stream', relative paths, and device/UNC prefixes");
            suite.Check(
                try_broker_open(allowed_junction + L"\\secret.txt") ==
                        BrokerStatus::ReparsePointRejected &&
                    try_broker_open(allowed_junction) == BrokerStatus::ReparsePointRejected,
                "OpenReadFile rejects NTFS junction reparse point inside allowed root");

            const BrokerStatus b1 = try_broker_open(allowed_readme);
            const BrokerStatus b2 = try_broker_open(allowed_readme);
            const BrokerStatus b3 = try_broker_open(allowed_readme);
            const BrokerStatus b4 = try_broker_open(allowed_readme);
            suite.Check(
                b1 == BrokerStatus::Ok && b2 == BrokerStatus::Ok && b3 == BrokerStatus::Ok &&
                    b4 == BrokerStatus::BudgetExceeded &&
                    path_broker.BrokeredHandleCount() == 3u,
                "OpenReadFile enforces max_brokered_handles budget (BudgetExceeded above cap)");
        }
        path_broker.RevokeSession();
    }

    // --- Group 4: Capability Default-Deny, CAS Settings, and Live Capability Revocation ---
    {
        BrokerConfig deny_cfg{};
        deny_cfg.package_id = L"pkg.deny";
        deny_cfg.executable_path = worker_exe;
        deny_cfg.extra_arguments = L"--sandbox-child";
        deny_cfg.sandbox_mode = SandboxMode::AppContainerOrRestrictedToken;
        deny_cfg.initial_capabilities = BrokerCapability::None;
        deny_cfg.allowed_read_roots = {allowed_dir};

        CapabilityBroker deny_broker;
        const bool started = deny_broker.Start(deny_cfg);
        if (started) {
            PayloadBuilder req;
            req.PutWString(allowed_readme);
            std::vector<uint8_t> resp;
            const BrokerStatus st = deny_broker.InvokeWorker(
                static_cast<uint32_t>(ChildCommand::ExerciseAllDenied), req.Take(), resp);
            PayloadParser p(resp);
            uint32_t s1 = 0, s2 = 0, s3 = 0, s4 = 0;
            const bool parsed =
                p.GetU32(s1) && p.GetU32(s2) && p.GetU32(s3) && p.GetU32(s4) && p.Done();
            suite.Check(
                st == BrokerStatus::Ok && parsed &&
                    static_cast<BrokerStatus>(s1) == BrokerStatus::PermissionDenied &&
                    static_cast<BrokerStatus>(s2) == BrokerStatus::PermissionDenied &&
                    static_cast<BrokerStatus>(s3) == BrokerStatus::PermissionDenied &&
                    static_cast<BrokerStatus>(s4) == BrokerStatus::PermissionDenied &&
                    deny_broker.DeniedCallCount() == 4u,
                "Default-deny worker with BrokerCapability::None receives PermissionDenied on all 4 ops");
        } else {
            suite.Check(false, "Default-deny broker session start");
        }
        deny_broker.RevokeSession();
    }

    {
        BrokerConfig cap_cfg{};
        cap_cfg.package_id = L"pkg.live";
        cap_cfg.executable_path = worker_exe;
        cap_cfg.extra_arguments = L"--sandbox-child";
        cap_cfg.sandbox_mode = SandboxMode::AppContainerOrRestrictedToken;
        cap_cfg.initial_capabilities =
            BrokerCapability::SettingsOwn | BrokerCapability::ContextRead;

        CapabilityBroker cap_broker;
        cap_broker.SetContextSnapshot(SanitizedContextSnapshot{L"WorkspaceRoot", 128u, 5u});
        const bool started = cap_broker.Start(cap_cfg);
        if (started) {
            const DWORD initial_pid = cap_broker.WorkerProcessId();
            std::vector<uint8_t> empty_req;
            std::vector<uint8_t> cas_resp;
            const BrokerStatus cas_st = cap_broker.InvokeWorker(
                static_cast<uint32_t>(ChildCommand::ExerciseSettingsAndContext), empty_req,
                cas_resp);
            PayloadParser p1(cas_resp);
            uint32_t cas_ok = 0;
            const bool p1_ok = p1.GetU32(cas_ok) && p1.Done();
            suite.Check(
                cas_st == BrokerStatus::Ok && p1_ok && cas_ok == 1u,
                "SettingsGet/SettingsSet CAS (create-only, Conflict on stale version, unconditional upsert, <=64KiB) and ContextGet succeed");

            // Strip SettingsOwn mid-session while the worker remains alive!
            cap_broker.RevokeCapabilities(BrokerCapability::SettingsOwn);

            std::vector<uint8_t> live_resp;
            const BrokerStatus live_st = cap_broker.InvokeWorker(
                static_cast<uint32_t>(ChildCommand::CheckSettingsVsContextLive), empty_req,
                live_resp);
            PayloadParser p2(live_resp);
            uint32_t get_st = 0, set_st = 0, ctx_st = 0, total_items = 0;
            const bool p2_ok = p2.GetU32(get_st) && p2.GetU32(set_st) && p2.GetU32(ctx_st) &&
                               p2.GetU32(total_items) && p2.Done();
            suite.Check(
                live_st == BrokerStatus::Ok && p2_ok &&
                    cap_broker.WorkerProcessId() == initial_pid &&
                    static_cast<BrokerStatus>(get_st) == BrokerStatus::PermissionDenied &&
                    static_cast<BrokerStatus>(set_st) == BrokerStatus::PermissionDenied &&
                    static_cast<BrokerStatus>(ctx_st) == BrokerStatus::Ok &&
                    total_items == 128u && cap_broker.DeniedCallCount() == 2u,
                "Live mid-session RevokeCapabilities(SettingsOwn) immediately blocks SettingsGet/SettingsSet while ContextGet still succeeds");
        } else {
            suite.Check(false, "Live capability broker session start");
        }
        cap_broker.RevokeSession();
    }

    // --- Group 5: Resource/Hang Enforcement & RevokeSession() Cleanup ---
    {
        BrokerConfig mem_cfg{};
        mem_cfg.package_id = L"pkg.mem";
        mem_cfg.executable_path = worker_exe;
        mem_cfg.extra_arguments = L"--sandbox-child";
        mem_cfg.sandbox_mode = SandboxMode::AppContainerOrRestrictedToken;
        mem_cfg.memory_limit_mib = 48; // 48 MiB Job Object memory cap

        CapabilityBroker mem_broker;
        const bool started = mem_broker.Start(mem_cfg);
        if (started) {
            PayloadBuilder req;
            req.PutU64(64ull * 1024ull * 1024ull); // 64 MiB commit > 48 MiB cap
            std::vector<uint8_t> resp;
            const BrokerStatus st = mem_broker.InvokeWorker(
                static_cast<uint32_t>(ChildCommand::AttemptExcessMemoryAlloc), req.Take(), resp);
            PayloadParser p(resp);
            uint32_t alloc_ok = 1, alloc_err = 0;
            const bool parsed = p.GetU32(alloc_ok) && p.GetU32(alloc_err) && p.Done();
            suite.Check(
                st == BrokerStatus::Ok && parsed && alloc_ok == 0u &&
                    (alloc_err == ERROR_COMMITMENT_LIMIT ||
                     alloc_err == ERROR_NOT_ENOUGH_MEMORY),
                "Job Object 48 MiB memory cap rejects 64 MiB VirtualAlloc commit in worker");

            // Deliberate infinite loop times out (~250ms) and kills/reaps worker.
            std::vector<uint8_t> empty_req;
            std::vector<uint8_t> spin_resp;
            const ULONGLONG t0 = GetTickCount64();
            const BrokerStatus spin_st = mem_broker.InvokeWorker(
                static_cast<uint32_t>(ChildCommand::DeliberateInfiniteSpin), empty_req, spin_resp,
                250);
            const ULONGLONG elapsed = GetTickCount64() - t0;
            suite.Check(
                spin_st == BrokerStatus::Timeout && elapsed >= 180ull && elapsed < 3500ull &&
                    !mem_broker.IsWorkerAlive(),
                "Deliberate worker infinite loop times out (~250ms) and worker is killed/reaped");
        } else {
            suite.Check(false, "Memory & hang broker session start");
        }
        mem_broker.RevokeSession();
    }

    {
        BrokerConfig rev_cfg{};
        rev_cfg.package_id = L"pkg.revoke";
        rev_cfg.executable_path = worker_exe;
        rev_cfg.extra_arguments = L"--sandbox-child";
        rev_cfg.sandbox_mode = SandboxMode::AppContainerOrRestrictedToken;
        rev_cfg.initial_capabilities =
            BrokerCapability::ReadAllowedRoots | BrokerCapability::ContextRead;
        rev_cfg.allowed_read_roots = {allowed_dir};

        CapabilityBroker rev_broker;
        const bool started = rev_broker.Start(rev_cfg);
        if (started) {
            // Have the live worker open allowed\readme.txt and keep its duplicated HANDLE open.
            PayloadBuilder req;
            req.PutWString(allowed_readme);
            req.PutU32(1u); // keep_open = 1
            std::vector<uint8_t> resp;
            const BrokerStatus open_st = rev_broker.InvokeWorker(
                static_cast<uint32_t>(ChildCommand::BrokerReadAndTryWrite), req.Take(), resp);
            suite.Check(
                open_st == BrokerStatus::Ok && rev_broker.IsWorkerAlive(),
                "Live worker holds brokered HANDLE to allowed\\readme.txt before RevokeSession()");

            rev_broker.RevokeSession();

            const BOOL deleted = DeleteFileW(allowed_readme.c_str());
            const DWORD post_attrs = GetFileAttributesW(allowed_readme.c_str());
            suite.Check(
                !rev_broker.IsWorkerAlive() && deleted != FALSE &&
                    post_attrs == INVALID_FILE_ATTRIBUTES,
                "RevokeSession() kills live worker and releases all file locks so allowed\\readme.txt deletes cleanly");

            std::vector<uint8_t> empty_req;
            std::vector<uint8_t> post_resp;
            SanitizedContextSnapshot snap{};
            const BrokerStatus post_invoke = rev_broker.InvokeWorker(
                static_cast<uint32_t>(ChildCommand::CheckSettingsVsContextLive), empty_req,
                post_resp);
            const BrokerStatus post_ctx = rev_broker.HandleContextGet(snap);
            suite.Check(
                rev_broker.IsRevoked() && post_invoke == BrokerStatus::Revoked &&
                    post_ctx == BrokerStatus::Revoked && rev_broker.DeniedCallCount() >= 2u,
                "Post-RevokeSession() calls fail closed immediately with BrokerStatus::Revoked");
        } else {
            suite.Check(false, "RevokeSession test broker session start");
        }
    }

    std::printf("Summary: %d passed, %d failed\n", suite.passed, suite.failed);
    return suite.failed == 0 ? 0 : 1;
}
