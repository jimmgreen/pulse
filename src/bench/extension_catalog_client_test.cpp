#include "../app/extensions/store/catalog_client.h"
#include "../app/extensions/package_revision.h"
#include "../common/json_value.h"
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
using namespace pulse::extensions::store;
using pulse::json::Value;
void Require(bool ok) { if (!ok) throw std::runtime_error("fixture operation failed"); }
std::string Hex(const std::vector<UCHAR>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (auto b : bytes) { text += digits[b >> 4]; text += digits[b & 15]; }
    return text;
}
struct Key {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_KEY_HANDLE key{};
    std::string public_key, pin;
    Key() {
        Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) >= 0);
        Require(BCryptGenerateKeyPair(algorithm, &key, 2048, 0) >= 0);
        Require(BCryptFinalizeKeyPair(key, 0) >= 0);
        ULONG size = 0;
        Require(BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &size, 0) >= 0);
        std::vector<UCHAR> blob(size);
        Require(BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, blob.data(), size, &size, 0) >= 0);
        public_key = Hex(blob);
        pin = pulse::extensions::PackageRevision(std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
    }
    ~Key() { if (key) BCryptDestroyKey(key); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
    std::string Sign(std::string_view bytes) const {
        constexpr char prefix[] = "Pulse.Extension.Catalog.Signature\0v1\0";
        std::string payload(prefix, sizeof(prefix) - 1); payload.append(bytes);
        const auto hash = pulse::extensions::PackageRevision(payload);
        std::vector<UCHAR> digest;
        for (size_t i = 0; i < hash.size(); i += 2) digest.push_back(static_cast<UCHAR>(std::stoul(hash.substr(i, 2), nullptr, 16)));
        BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
        ULONG size = 0;
        Require(BCryptSignHash(key, &padding, digest.data(), static_cast<ULONG>(digest.size()), nullptr, 0, &size, BCRYPT_PAD_PKCS1) >= 0);
        std::vector<UCHAR> signature(size);
        Require(BCryptSignHash(key, &padding, digest.data(), static_cast<ULONG>(digest.size()), signature.data(), size, &size, BCRYPT_PAD_PKCS1) >= 0);
        Value value = Value::Object(); value.set("formatVersion", 1); value.set("algorithm", "RSA-PKCS1-SHA256");
        value.set("publisher", pin); value.set("signature", Hex(signature));
        return pulse::json::Serialize(value) + "\n";
    }
};
struct Directory {
    std::filesystem::path path;
    Directory() {
        wchar_t module[32768]{};
        Require(GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module))) != 0);
        path = std::filesystem::path(module).parent_path() /
            (L"catalog-client-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        Require(CreateDirectoryW(path.c_str(), nullptr) != FALSE);
    }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
std::string Bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void Write(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); Require(file.good());
}
struct Response { std::string bytes; bool ok = true; bool cancel = false; bool ignore_rejection = false; };
struct Fake {
    std::vector<Response> responses;
    std::vector<std::wstring> urls;
    std::vector<uint64_t> limits;
    std::atomic<bool>* cancellation = nullptr;
    std::function<void(size_t)> on_read;
    pulse::app::UpdateResponseReader Reader() {
        return [&](std::wstring_view url, uint64_t limit, const std::atomic<bool>&,
            const std::function<bool(const void*, DWORD)>& consume, pulse::app::UpdateError& category, DWORD& error) {
            const auto i = urls.size(); urls.emplace_back(url); limits.push_back(limit);
            if (on_read) on_read(i);
            if (i >= responses.size()) { category = pulse::app::UpdateError::Network; error = ERROR_BAD_NETPATH; return false; }
            const auto& r = responses[i];
            if (r.cancel && cancellation) cancellation->store(true);
            bool accepted = true;
            for (size_t offset = 0; offset < r.bytes.size(); offset += 65536) {
                const DWORD size = static_cast<DWORD>((std::min)(size_t{65536}, r.bytes.size() - offset));
                if (!consume(r.bytes.data() + offset, size)) { accepted = false; break; }
            }
            category = r.ok ? pulse::app::UpdateError::None : pulse::app::UpdateError::Network;
            error = r.ok ? ERROR_SUCCESS : ERROR_CONNECTION_ABORTED;
            return r.ok && (accepted || r.ignore_rejection);
        };
    }
};
std::string Index(std::string_view date) {
    return "{\n \"formatVersion\":1, \"generated\":\"" + std::string(date) +
        "\", \"publishers\":[], \"extensions\":[]\n}\n";
}
int passed = 0, failed = 0;
void Check(const char* name, bool ok) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); ok ? ++passed : ++failed; }
size_t TempCount(const std::filesystem::path& root) {
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) if (entry.path().filename().native().find(L".tmp-") != std::wstring::npos) ++count;
    return count;
}
} // namespace
int main() {
    try {
        Key key;
        Directory directory;
        const auto cache = directory.path / L"catalog.cache";
        const auto old_index = Index("2026-11-09T00:00:00Z"), newer = Index("2026-11-10T00:00:00Z");
        const auto old_sig = key.Sign(old_index), new_sig = key.Sign(newer);
        const testing::CatalogValidator validate = [&](std::string_view index, std::string_view sig, Catalog& catalog, std::string& error) {
            return testing::LoadVerifiedCatalogWithKey(index, sig, key.pin, key.public_key, catalog, error);
        };
        std::atomic<bool> cancelled{false};
        CatalogSnapshot snapshot;
        std::string error;
        auto fetch = [&](Fake& fake, const std::filesystem::path& path = {}, const testing::CatalogCacheHook& hook = {}) {
            fake.cancellation = &cancelled;
            return testing::FetchCatalogWithValidator(path, cancelled, snapshot, error, fake.Reader(), validate, hook);
        };
        pulse::app::SetUpdateSource(pulse::app::UpdateSource::Automatic);
        Fake good{{{old_index}, {old_sig}}};
        Check("online paired fetch and atomic cache", fetch(good, cache) == CatalogFetchStatus::Online && snapshot.cache_persisted &&
            !snapshot.offline && snapshot.index_bytes == old_index && snapshot.signature_bytes == old_sig && good.urls.size() == 2);
        Check("transport receives independent limits", good.limits == std::vector<uint64_t>{kMaxCatalogBytes, kMaxCatalogSignatureBytes});
        const auto old_cache = Bytes(cache);
        Check("single cache stores exact original byte strings", old_cache.substr(20) == old_index + old_sig && TempCount(directory.path) == 0);
        Fake offline;
        Check("network failure uses reverified offline cache", fetch(offline, cache) == CatalogFetchStatus::Offline && snapshot.offline &&
            snapshot.catalog.generated == "2026-11-09T00:00:00Z" && snapshot.source.empty() && !snapshot.diagnostic.empty());
        Fake mirror{{{old_index}, {"bad signature"}, {newer}, {new_sig}}};
        Check("bad mirror pair falls back to next complete pair", fetch(mirror) == CatalogFetchStatus::Online && snapshot.index_bytes == newer &&
            mirror.urls.size() == 4 && mirror.urls[0].starts_with(L"https://ghproxy.net/") && mirror.urls[1].starts_with(L"https://ghproxy.net/") &&
            mirror.urls[2].starts_with(L"https://gh-proxy.com/") && mirror.urls[3].starts_with(L"https://gh-proxy.com/"));
        Fake last_source{{{old_index}, {"bad"}, {newer}, {"bad"}, {newer}, {new_sig}}};
        Check("two mismatched mirror pairs fall back to GitHub pair", fetch(last_source) == CatalogFetchStatus::Online && last_source.urls.size() == 6 &&
            last_source.urls[4] == kCatalogIndexUrl && last_source.urls[5] == kCatalogSignatureUrl);
        Fake short_success{{{old_index.substr(0, old_index.size() / 2)}, {old_sig}, {newer}, {new_sig}}};
        Check("successful transport EOF with half document fails verification", fetch(short_success) == CatalogFetchStatus::Online && snapshot.index_bytes == newer && short_success.urls.size() == 4);
        Fake partial{{{"partial", false}, {newer}, {new_sig}}};
        Check("partial index discarded before fallback", fetch(partial) == CatalogFetchStatus::Online && snapshot.index_bytes == newer && partial.urls.size() == 3 &&
            partial.urls[1].ends_with(L"index.json") && partial.urls[2].ends_with(L"index.sig.json"));
        Fake partial_signature{{{old_index}, {old_sig.substr(0, 40), false}, {newer}, {new_sig}}};
        Check("partial signature never combined with another source", fetch(partial_signature) == CatalogFetchStatus::Online && snapshot.index_bytes == newer && partial_signature.urls.size() == 4);
        Fake oversized{{{std::string(kMaxCatalogBytes + 1, 'x'), true, false, true}, {newer}, {new_sig}}};
        Check("injected reader cannot bypass index byte limit", fetch(oversized) == CatalogFetchStatus::Online && snapshot.index_bytes == newer && oversized.urls.size() == 3);
        Fake oversized_sig{{{old_index}, {std::string(kMaxCatalogSignatureBytes + 1, 'x'), true, false, true}, {newer}, {new_sig}}};
        Check("injected reader cannot bypass signature byte limit", fetch(oversized_sig) == CatalogFetchStatus::Online && snapshot.index_bytes == newer && oversized_sig.urls.size() == 4);
        Fake captured{{{newer}, {new_sig}}};
        captured.on_read = [&](size_t) { pulse::app::SetUpdateSource(pulse::app::UpdateSource::GitHub); };
        Check("download setting captured before first reader call", fetch(captured) == CatalogFetchStatus::Online &&
            snapshot.requested_source == pulse::app::UpdateSource::Automatic && captured.urls[1].starts_with(L"https://ghproxy.net/"));
        Fake github{{{newer}, {new_sig}}};
        Check("explicit GitHub source uses only fixed official URLs", fetch(github) == CatalogFetchStatus::Online &&
            github.urls == std::vector<std::wstring>{std::wstring(kCatalogIndexUrl), std::wstring(kCatalogSignatureUrl)});
        cancelled = true; snapshot.index_bytes = "sentinel";
        Fake no_calls;
        Check("pre-cancel does not read cache/network or publish", fetch(no_calls, cache) == CatalogFetchStatus::Cancelled && no_calls.urls.empty() && snapshot.index_bytes == "sentinel" && Bytes(cache) == old_cache);
        cancelled = false;
        Fake cancel_stream{{{newer, true, true}}}; snapshot.index_bytes = "sentinel";
        Check("cancel during stream never falls back offline", fetch(cancel_stream, cache) == CatalogFetchStatus::Cancelled && snapshot.index_bytes == "sentinel" && Bytes(cache) == old_cache);
        cancelled = false;
        for (const auto point : {testing::CatalogCachePoint::BeforeRead, testing::CatalogCachePoint::AfterRead,
            testing::CatalogCachePoint::BeforeWrite, testing::CatalogCachePoint::AfterWrite, testing::CatalogCachePoint::BeforeCommit}) {
            Fake attempt{{{newer}, {new_sig}}}; snapshot.index_bytes = "sentinel";
            const auto hook = [&](testing::CatalogCachePoint current) { if (current == point) cancelled = true; };
            Check("cache cancellation checkpoint preserves output/old file", fetch(attempt, cache, hook) == CatalogFetchStatus::Cancelled &&
                snapshot.index_bytes == "sentinel" && Bytes(cache) == old_cache && TempCount(directory.path) == 0);
            cancelled = false;
        }
        bool old_visible = false;
        Fake atomic{{{newer}, {new_sig}}};
        const auto atomic_status = fetch(atomic, cache, [&](testing::CatalogCachePoint point) {
            if (point == testing::CatalogCachePoint::BeforeCommit) old_visible = Bytes(cache) == old_cache;
        });
        if (!snapshot.cache_persisted) std::printf("[DIAG] atomic update: %s; %s\n", error.c_str(), snapshot.diagnostic.c_str());
        Check("old cache visible until atomic commit", atomic_status == CatalogFetchStatus::Online && old_visible && snapshot.cache_persisted && Bytes(cache).substr(20) == newer + new_sig && TempCount(directory.path) == 0);
        const auto new_cache = Bytes(cache);
        Fake rollback{{{old_index}, {old_sig}}};
        Check("verified older online catalog rejected against cache", fetch(rollback, cache) == CatalogFetchStatus::Offline && snapshot.index_bytes == newer &&
            snapshot.diagnostic.find("rollback") != std::string::npos && Bytes(cache) == new_cache);
        pulse::app::SetUpdateSource(pulse::app::UpdateSource::Automatic);
        Fake rollback_fallback{{{old_index}, {old_sig}, {newer}, {new_sig}}};
        Check("rollback source rejection permits next verified source", fetch(rollback_fallback, cache) == CatalogFetchStatus::Online && rollback_fallback.urls.size() == 4);
        Write(cache, new_cache.substr(0, new_cache.size() - 1)); snapshot.index_bytes = "sentinel";
        Fake corrupted;
        Check("truncated cache cannot publish offline", fetch(corrupted, cache) == CatalogFetchStatus::Failed && snapshot.index_bytes == "sentinel" && error.find("cache_invalid_envelope") != std::string::npos);
        Write(cache, new_cache + "x"); Fake trailing;
        Check("trailing cache bytes rejected", fetch(trailing, cache) == CatalogFetchStatus::Failed);
        auto wrong_version = new_cache; wrong_version[8] = 2; Write(cache, wrong_version); Fake version;
        Check("unknown cache envelope version rejected", fetch(version, cache) == CatalogFetchStatus::Failed);
        auto wrong_length = new_cache; wrong_length[15] = 127; Write(cache, wrong_length); Fake length;
        Check("oversized envelope length rejected", fetch(length, cache) == CatalogFetchStatus::Failed);
        auto tampered = new_cache; tampered[25] ^= 1; Write(cache, tampered); Fake invalid_signature;
        Check("cached metadata never trusted without signature", fetch(invalid_signature, cache) == CatalogFetchStatus::Failed && error.find("cache_verification") != std::string::npos);
        Write(cache, std::string(kMaxCatalogBytes + kMaxCatalogSignatureBytes + 21, 'x')); Fake oversized_cache;
        Check("oversized cache rejected before allocation", fetch(oversized_cache, cache) == CatalogFetchStatus::Failed && error.find("cache_size_limit") != std::string::npos);
        Write(cache, new_cache);
        HANDLE locked = CreateFileW(cache.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(locked != INVALID_HANDLE_VALUE);
        Fake locked_update{{{newer}, {new_sig}}};
        const auto locked_status = fetch(locked_update, cache);
        CloseHandle(locked);
        Check("failed atomic replacement preserves old cache", locked_status == CatalogFetchStatus::Online && !snapshot.cache_persisted &&
            snapshot.diagnostic.find("cache_persistence_failed") != std::string::npos && Bytes(cache) == new_cache && TempCount(directory.path) == 0);
        Fake production{{{newer}, {new_sig}}};
        Check("production client cannot use fixture trust", FetchCatalog(cache, cancelled, snapshot, error, production.Reader()) == CatalogFetchStatus::Failed);
        Fake missing_parent{{{newer}, {new_sig}}};
        const auto missing = directory.path / L"not-created" / L"catalog.cache";
        Check("persistence failure still returns verified online snapshot", fetch(missing_parent, missing) == CatalogFetchStatus::Online &&
            !snapshot.cache_persisted && snapshot.diagnostic.find("cache_persistence_failed") != std::string::npos && !std::filesystem::exists(missing.parent_path()));
        Fake relative{{{newer}, {new_sig}}}; snapshot.index_bytes = "sentinel";
        Check("relative cache path refused without I/O", fetch(relative, L"relative.cache") == CatalogFetchStatus::Failed && relative.urls.empty() && snapshot.index_bytes == "sentinel");
        const auto directory_leaf = directory.path / L"directory-leaf"; Require(CreateDirectoryW(directory_leaf.c_str(), nullptr) != FALSE);
        Fake directory_cache{{{newer}, {new_sig}}};
        Check("directory cache leaf never replaced", fetch(directory_cache, directory_leaf) == CatalogFetchStatus::Online && !snapshot.cache_persisted && std::filesystem::is_directory(directory_leaf) && TempCount(directory.path) == 0);
        const auto symlink = directory.path / L"reparse.cache";
        if (CreateSymbolicLinkW(symlink.c_str(), cache.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            Fake reparse_offline;
            Check("reparse cache leaf not followed on read", fetch(reparse_offline, symlink) == CatalogFetchStatus::Failed);
            Fake reparse_write{{{newer}, {new_sig}}};
            Check("reparse cache leaf not replaced on persistence", fetch(reparse_write, symlink) == CatalogFetchStatus::Online && !snapshot.cache_persisted && Bytes(cache) == new_cache);
        } else std::printf("[SKIP] symlink fixture unavailable: %lu\n", GetLastError());
        pulse::app::SetUpdateSource(pulse::app::UpdateSource::Automatic);
        std::printf("Catalog client tests: %d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    } catch (const std::exception& error) { std::printf("[FAIL] fixture: %s\n", error.what()); return 1; }
}
