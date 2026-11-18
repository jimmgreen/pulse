#include "../app/extensions/runtime.h"
#include "../app/extensions/package_revision.h"
#include "../app/extensions/store_trust_watch.h"
#include "../app/extensions/audit_persistence.h"
#include "../common/json_value.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#include <condition_variable>
#include <chrono>
#include <mutex>
using namespace pulse;
using namespace pulse::extensions;
namespace {
int failures = 0;
void Check(bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; }
std::string Hex(const std::vector<UCHAR>& bytes) {
    std::string out;
    for (auto c : bytes) { out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15]; }
    return out;
}
struct Keys {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_KEY_HANDLE key{};
    ~Keys() { if (key) BCryptDestroyKey(key); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
};
void Put(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary); output << bytes;
}
bool Policy(const std::filesystem::path& root, const std::string& bytes) {
    Put(root / L"policy.pending", bytes);
    for (int i = 0; i < 100; ++i) {
        if (MoveFileExW((root / L"policy.pending").c_str(), (root / L"signature-policy.json").c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        Sleep(10);
    }
    return false;
}
}
int wmain() {
    Keys keys;
    if (BCryptOpenAlgorithmProvider(&keys.algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGenerateKeyPair(keys.algorithm, &keys.key, 2048, 0) < 0 || BCryptFinalizeKeyPair(keys.key, 0) < 0) return 2;
    ULONG size = 0;
    if (BCryptExportKey(keys.key, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &size, 0) < 0) return 2;
    std::vector<UCHAR> blob(size);
    if (BCryptExportKey(keys.key, nullptr, BCRYPT_RSAPUBLIC_BLOB, blob.data(), size, &size, 0) < 0) return 2;
    const std::string manifest = R"({"apiVersion":1,"id":"signed","name":"Signed"})";
    const std::string other = R"({"apiVersion":1,"id":"unsigned","name":"Unsigned"})";
    constexpr char prefix[] = "Pulse.Extension.Manifest.Signature\0v1\0";
    const auto digest = PackageRevision(std::string(prefix, sizeof(prefix) - 1) + manifest);
    std::vector<UCHAR> hash;
    for (size_t i = 0; i < digest.size(); i += 2) hash.push_back(static_cast<UCHAR>(std::stoul(digest.substr(i, 2), nullptr, 16)));
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    if (BCryptSignHash(keys.key, &padding, hash.data(), static_cast<ULONG>(hash.size()), nullptr, 0, &size, BCRYPT_PAD_PKCS1) < 0) return 2;
    std::vector<UCHAR> signature(size);
    if (BCryptSignHash(keys.key, &padding, hash.data(), static_cast<ULONG>(hash.size()), signature.data(), size, &size, BCRYPT_PAD_PKCS1) < 0) return 2;
    const auto pin = PackageRevision(std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
    auto signed_value = json::Value::Object();
    signed_value.set("formatVersion", 1); signed_value.set("algorithm", "RSA-PKCS1-SHA256");
    signed_value.set("publisher", pin); signed_value.set("signature", Hex(signature));
    auto policy = json::Value::Object(), publishers = json::Value::Array(), publisher = json::Value::Object();
    policy.set("formatVersion", 1); policy.set("requireSigned", false);
    publisher.set("id", pin); publisher.set("publicKey", Hex(blob)); publishers.push(publisher); policy.set("publishers", publishers);
    const auto original_policy = json::Serialize(policy);
    wchar_t temp[32768]{};
    if (!GetTempPathW(32768, temp)) return 2;
    const auto root = std::filesystem::path(temp) / (L"PulseTrustTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(root)) return 2;
    std::filesystem::create_directory(root / L"versions");
    Put(root / L"signature-policy.json", original_policy);
    auto registry = json::Value::Object(), entries = json::Value::Array();
    for (const auto& bytes : {manifest, other}) {
        const auto revision = PackageRevision(bytes);
        Put(root / L"versions" / (revision + ".json"), bytes);
        auto entry = json::Value::Object();
        entry.set("id", bytes == manifest ? "signed" : "unsigned"); entry.set("name", bytes == manifest ? "Signed" : "Unsigned");
        entry.set("enabled", true); entry.set("activeRevision", revision); entries.push(entry);
    }
    Put(root / L"versions" / (PackageRevision(manifest) + ".sig.json"), json::Serialize(signed_value));
    registry.set("formatVersion", 1); registry.set("packages", entries); Put(root / L"registry.json", json::Serialize(registry));
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", root.c_str());
    std::vector<std::wstring> arguments{L"pulse.exe", L"--test-instance", L"--extension-store", root.wstring()};
    std::vector<wchar_t*> argv; for (auto& arg : arguments) argv.push_back(arg.data());
    const auto started = PrepareStartup(static_cast<int>(argv.size()), argv.data());
    Check(!started.exit_requested && PackageCount() == 2, "load real signed and unsigned store fixture");
    const auto signed_binding = CaptureCallerBinding(0, 42), unsigned_binding = CaptureCallerBinding(1, 42);
    const auto ticket = IssueCaller(0, 42);
    Check(signed_binding && unsigned_binding && ticket && StartDevelopmentReload(), "start trust worker with active principal and ticket");
    bool replacements = true;
    unsigned replacement_failures = 0;
    for (int i = 0; i < 64; ++i) {
        const auto source = root / L"policy.pending", target = root / L"signature-policy.json";
        const auto content = original_policy + std::string(32000, ' ');
        bool staged = false;
        {
            std::ofstream output(source, std::ios::binary | std::ios::trunc);
            output.write(content.data(), static_cast<std::streamsize>(content.size()));
            output.close();
            staged = !output.fail();
        }
        bool replaced = false;
        DWORD move_error = ERROR_SUCCESS;
        unsigned retries = 0;
        // Same bounded writer policy as package_signing.atomic_write, not an OS atomicity claim.
        for (unsigned attempt = 0; attempt < 50; ++attempt) {
            replaced = MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
            move_error = replaced ? ERROR_SUCCESS : GetLastError();
            if (replaced || (move_error != ERROR_ACCESS_DENIED && move_error != ERROR_SHARING_VIOLATION &&
                             move_error != ERROR_LOCK_VIOLATION) || attempt == 49) break;
            ++retries; Sleep(10);
        }
        if (retries) std::printf("[DIAG] bounded_writer index=%d retries=%u final_error=%lu\n", i, retries, move_error);
        replacements &= staged && replaced;
        if (!staged || !replaced) {
            ++replacement_failures;
            std::printf("[DIAG] replace index=%d staged=%d error=%lu source_attributes=%lu target_attributes=%lu ancestor_attributes=%lu\n",
                        i, staged ? 1 : 0, move_error, GetFileAttributesW(source.c_str()),
                        GetFileAttributesW(target.c_str()), GetFileAttributesW(root.c_str()));
            const auto access = [](const std::filesystem::path& path, DWORD desired, DWORD flags) {
                HANDLE file = CreateFileW(path.c_str(), desired, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_EXISTING, flags | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                const DWORD error = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
                if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                return error;
            };
            std::printf("[DIAG] current access error source_DELETE=%lu target_DELETE=%lu ancestor_READ_ATTRIBUTES=%lu ancestor_ADD_FILE=%lu; probes occur after failure\n",
                        access(source, DELETE, 0), access(target, DELETE, 0),
                        access(root, FILE_READ_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS),
                        access(root, FILE_ADD_FILE, FILE_FLAG_BACKUP_SEMANTICS));
            const auto identity = [](const std::filesystem::path& path, const char* label) {
                HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (file == INVALID_HANDLE_VALUE) { std::printf("[DIAG] %s identity_open_error=%lu\n", label, GetLastError()); return; }
                BY_HANDLE_FILE_INFORMATION info{};
                FILE_STANDARD_INFO standard{};
                const bool have_id = GetFileInformationByHandle(file, &info) != FALSE;
                const bool have_standard = GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) != FALSE;
                std::printf("[DIAG] %s identity_valid=%d volume=%lu id=%08lx:%08lx links=%lu standard_valid=%d delete_pending=%d bytes=%lld\n",
                            label, have_id ? 1 : 0, info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
                            info.nNumberOfLinks, have_standard ? 1 : 0, standard.DeletePending ? 1 : 0, standard.EndOfFile.QuadPart);
                CloseHandle(file);
            };
            identity(source, "source"); identity(target, "target");
            DWORD handles = 0;
            GetProcessHandleCount(GetCurrentProcess(), &handles);
            std::printf("[DIAG] process_handles=%lu target_WRITE_access_error=%lu\n", handles,
                        access(target, GENERIC_WRITE, 0));
            if (replacement_failures == 1) {
                // No intervening write or loop iteration: retain this failed source/target pair.
                StopDevelopmentReload();
                identity(source, "source_after_stop"); identity(target, "target_after_stop");
                const bool control = MoveFileExW(source.c_str(), target.c_str(),
                                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
                const DWORD control_error = control ? ERROR_SUCCESS : GetLastError();
                std::printf("[DIAG] immediate_same_pair_after_stop index=%d replaced=%d error=%lu; original failure remains\n",
                            i, control ? 1 : 0, control_error);
                // Isolate the exact sharing modes without a concurrently scheduled watcher.
                const auto controls = root / L"rename-controls";
                std::filesystem::create_directory(controls);
                for (int mode = 0; mode < 4; ++mode) {
                    const auto from = controls / (L"from-" + std::to_wstring(mode));
                    const auto to = controls / (L"to-" + std::to_wstring(mode));
                    Put(from, content); Put(to, content);
                    HANDLE directory = INVALID_HANDLE_VALUE, file = INVALID_HANDLE_VALUE;
                    if (mode & 1) directory = CreateFileW(controls.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                    const DWORD directory_error = (mode & 1) && directory == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
                    if (mode & 2) file = CreateFileW(to.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                    const DWORD file_error = (mode & 2) && file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
                    const bool moved = MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
                    const DWORD move = moved ? ERROR_SUCCESS : GetLastError();
                    std::printf("[DIAG] held_handle_control mode=%d (1=directory,2=target) directory_open=%lu target_open=%lu moved=%d error=%lu\n",
                                mode, directory_error, file_error, moved ? 1 : 0, move);
                    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
                    if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
                }
                Check(StartDevelopmentReload(), "restart observer after immediate failure and sharing controls");
            }
        }
        Sleep(20); PollDevelopmentReload([] {});
    }
    std::printf("[DIAG] replacement_failures=%u signed_enabled=%d trust_check_sequence=%llu\n",
                replacement_failures, PackageEnabled(0) ? 1 : 0,
                static_cast<unsigned long long>(StoreTrustCheckSequence()));
    Check(replacements, "bounded policy writer commits concurrent watcher replacements without losing old policy on failure");
    Check(PackageEnabled(0), "concurrent policy replacement preserves signed package authorization");
    Put(root / L"registry.json", "{}");
    Sleep(1100); PollDevelopmentReload([] {});
    Check(PackageEnabled(0) && PackageEnabled(1), "trust polling never reloads changed registry or enable state");
    Put(root / L"registry.json", json::Serialize(registry));
    policy.set("publishers", json::Value::Array());
    Check(Policy(root, json::Serialize(policy)), "atomically revoke temporary publisher pin");
    bool closed = false;
    const auto deadline = GetTickCount64() + 5000;
    while (PackageEnabled(0) && GetTickCount64() < deadline) {
        PollDevelopmentReload([&] { closed = true; }); Sleep(20);
    }
    Check(!PackageEnabled(0) && closed && signed_binding && !ValidateCallerBinding(*signed_binding) && ticket && !ValidateCaller(*ticket),
          "running revocation rejects old binding ticket and closes stale UI");
    Check(PackageEnabled(1) && unsigned_binding && ValidateCallerBinding(*unsigned_binding), "unrelated unsigned package remains authorized by unchanged policy semantics");
    SetPackageEnabled(0, true);
    Check(!PackageEnabled(0), "ordinary enable cannot restore a trust-revoked package");
    Check(Policy(root, original_policy), "restore trusted pin without automatic privilege restoration");
    Sleep(1100); PollDevelopmentReload([] {});
    Check(!PackageEnabled(0), "restored policy requires explicit new startup");
    StopDevelopmentReload();
    std::filesystem::remove(root / L"signature-policy.json");
    {
        StoreTrustWatch watch(root.wstring(), 777, {{manifest, json::Serialize(signed_value)}, {other, {}}});
        std::optional<StoreTrustUpdate> update;
        const auto until = GetTickCount64() + 5000;
        while (!update && GetTickCount64() < until) { update = watch.TakeUpdate(); Sleep(10); }
        Check(update && update->generation == 777 && update->revoked == std::vector<size_t>{0},
              "missing policy rejects signed snapshot while unsigned development remains trusted");
    }
    Put(root / L"signature-policy.json", "{}");
    {
        StoreTrustWatch watch(root.wstring(), 778, {{manifest, json::Serialize(signed_value)}, {other, {}}});
        std::optional<StoreTrustUpdate> update;
        const auto until = GetTickCount64() + 5000;
        while (!update && GetTickCount64() < until) { update = watch.TakeUpdate(); Sleep(10); }
        Check(update && update->revoked == std::vector<size_t>({0, 1}), "malformed policy follows verifier rejection for each snapshot");
    }
    Check(Policy(root, original_policy), "restore policy for explicit restart");
    // A replacement startup invalidates prior watch epochs and obtains fresh verified bytes.
    Check(!PrepareStartup(static_cast<int>(argv.size()), argv.data()).exit_requested && PackageEnabled(0), "new startup re-verifies restored signature");
    StopDevelopmentReload();
    const auto shutdown_binding = CaptureCallerBinding(0, 43);
    const auto admitted_before = PersistentExtensionAuditStatus().accepted;
    PersistExtensionAudit("shutdown.fixture", "audit.list", "ok");
    Check(PersistentExtensionAuditStatus().accepted == admitted_before + 1, "admit final diagnostic immediately before host shutdown");
    PrepareShutdown();
    const auto history = ReadPersistentAudit((root / L"extension-audit").wstring());
    bool final_record = false;
    if (const auto* records = history.find("records"))
        for (const auto& record : records->items()) final_record |= record.get_string("packageId") == "shutdown.fixture";
    Check(history.get_bool("ok") && final_record && shutdown_binding && !ValidateCallerBinding(*shutdown_binding),
          "explicit shutdown joins audit before static teardown and revokes old authority");
    PrepareShutdown();
    SetPackageEnabled(0, true);
    Check(!PackageEnabled(0), "shutdown is idempotent and cannot reactivate old packages");
    wchar_t program[] = L"pulse.exe"; wchar_t* reset[]{program}; PrepareStartup(1, reset);
    struct Gate { std::mutex mutex; std::condition_variable wake; bool entered = false, release = false; };
    auto gate = std::make_shared<Gate>();
    auto blocked = std::make_unique<StoreTrustWatch>(root.wstring(), 900, std::vector<StoreTrustSnapshot>{{other, {}}},
        [gate](const std::wstring&, std::string&) {
            std::unique_lock lock(gate->mutex); gate->entered = true; gate->wake.notify_all();
            gate->wake.wait(lock, [&] { return gate->release; }); return false;
        });
    {
        std::unique_lock lock(gate->mutex);
        Check(gate->wake.wait_for(lock, std::chrono::seconds(5), [&] { return gate->entered; }), "inject genuinely blocked worker read");
    }
    const auto stop_started = GetTickCount64();
    blocked.reset();
    const auto stop_elapsed = GetTickCount64() - stop_started;
    Check(stop_elapsed >= 150 && stop_elapsed < 750, "blocked reader shutdown has bounded 200ms wait without terminating thread");
    int refused = 0;
    for (int i = 0; i < 4; ++i) {
        try { StoreTrustWatch denied(root.wstring(), 901, {}); }
        catch (...) { ++refused; }
    }
    Check(refused == 4, "unreaped worker occupies sole slot rather than accumulating on restart");
    { std::lock_guard lock(gate->mutex); gate->release = true; }
    gate->wake.notify_all();
    std::unique_ptr<StoreTrustWatch> recovered;
    const auto recovery_deadline = GetTickCount64() + 5000;
    while (!recovered && GetTickCount64() < recovery_deadline) {
        try { recovered = std::make_unique<StoreTrustWatch>(root.wstring(), 902, std::vector<StoreTrustSnapshot>{}); }
        catch (...) { Sleep(10); }
    }
    Check(recovered != nullptr && StoreTrustCheckSequence() == 0, "released old worker frees slot without publishing stale results to runtime");
    recovered.reset();
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr);
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
