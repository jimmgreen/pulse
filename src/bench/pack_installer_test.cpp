// pack_installer_test - download, verification and staging of preview packs,
// with an in-memory "server" in place of WinHTTP.
#include "../app/pack_installer.h"
#include "../common/utf8_file.h"
#include "../common/preview_packs.h"
#include <bcrypt.h>
#include <compressapi.h>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace pulse;
using namespace pulse::app;

namespace {
int g_failed = 0, g_passed = 0;
void Check(bool ok, const char* what) {
    if (ok) { ++g_passed; return; }
    ++g_failed;
    std::printf("FAIL: %s\n", what);
}

std::vector<uint8_t> Lzms(const std::vector<uint8_t>& in) {
    COMPRESSOR_HANDLE h = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &h)) return {};
    SIZE_T needed = 0;
    Compress(h, in.data(), in.size(), nullptr, 0, &needed);
    std::vector<uint8_t> out(needed);
    SIZE_T wrote = 0;
    if (!Compress(h, in.data(), in.size(), out.data(), out.size(), &wrote)) out.clear();
    else out.resize(wrote);
    CloseCompressor(h);
    return out;
}

std::wstring Sha256Hex(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t digest[32]{};
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hash, const_cast<uint8_t*>(data.data()), static_cast<ULONG>(data.size()), 0);
    BCryptFinishHash(hash, digest, 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    std::wstring hex;
    for (uint8_t b : digest) { wchar_t t[3]; swprintf(t, 3, L"%02x", b); hex += t; }
    return hex;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::vector<uint8_t> ReadAll(const std::wstring& p) {
    std::vector<uint8_t> out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") || !f) return out;
    uint8_t buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return out;
}

// A pack of two files plus the bytes the fake server hands out.
struct Fixture {
    std::vector<std::vector<uint8_t>> plain, packed;
    std::vector<std::wstring> names, sha, packed_sha;
    std::vector<PackFile> files;
    std::map<std::wstring, std::vector<uint8_t>> server;
    PackRelease release{};
    std::wstring version;

    explicit Fixture(const wchar_t* ver) : version(ver) {
        std::vector<uint8_t> big(3u << 20);
        uint32_t x = 12345;
        for (size_t i = 0; i < big.size(); ++i) {   // half noise, half runs
            x = x * 1664525u + 1013904223u;
            big[i] = (i / 4096) % 2 ? static_cast<uint8_t>(x >> 24) : static_cast<uint8_t>(i / 4096);
        }
        const std::string note = "Pulse FFmpeg preview pack\r\nLicense: LGPL-2.1\r\n";
        plain = {big, std::vector<uint8_t>(note.begin(), note.end())};
        names = {L"ffmpeg.exe", L"SOURCE.txt"};
        for (size_t i = 0; i < plain.size(); ++i) {
            packed.push_back(Lzms(plain[i]));
            sha.push_back(Sha256Hex(plain[i]));
            packed_sha.push_back(Sha256Hex(packed[i]));
        }
        for (size_t i = 0; i < plain.size(); ++i) {
            files.push_back({names[i].c_str(), plain[i].size(), sha[i].c_str(),
                             packed[i].size(), packed_sha[i].c_str()});
            server[L"https://packs.example.invalid/v/" + names[i] + L".lzms"] = packed[i];
        }
        release = {L"ffmpeg", version.c_str(), L"https://packs.example.invalid/v/", files.data(), files.size()};
    }

    // File i gets new contents, as in a later release.
    void Change(size_t i, const std::string& text) {
        plain[i].assign(text.begin(), text.end());
        packed[i] = Lzms(plain[i]);
        sha[i] = Sha256Hex(plain[i]);
        packed_sha[i] = Sha256Hex(packed[i]);
        files[i] = {names[i].c_str(), plain[i].size(), sha[i].c_str(), packed[i].size(), packed_sha[i].c_str()};
        server[L"https://packs.example.invalid/v/" + names[i] + L".lzms"] = packed[i];
    }

    UpdateResponseReader Reader(size_t chunk = 7000, std::atomic<bool>* cancel_after_first = nullptr) {
        return [this, chunk, cancel_after_first](std::wstring_view url, uint64_t maximum,
                   const std::atomic<bool>& cancelled, const std::function<bool(const void*, DWORD)>& consume,
                   UpdateError& category, DWORD& error) {
            auto it = server.find(std::wstring(url));
            if (it == server.end()) { category = UpdateError::HttpStatus; error = 404; return false; }
            const auto& body = it->second;
            if (body.size() > maximum) { category = UpdateError::ResponseTooLarge; error = ERROR_FILE_TOO_LARGE; return false; }
            for (size_t at = 0; at < body.size(); at += chunk) {
                if (cancelled.load()) { error = ERROR_CANCELLED; return false; }
                const DWORD n = static_cast<DWORD>((std::min)(chunk, body.size() - at));
                if (!consume(body.data() + at, n)) { category = UpdateError::LocalIo; error = ERROR_WRITE_FAULT; return false; }
                if (cancel_after_first) cancel_after_first->store(true);
            }
            return true;
        };
    }
};

std::wstring MakeRoot() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring root = std::wstring(tmp) + L"pulse_pack_installer_test_" + std::to_wstring(GetCurrentProcessId());
    RemovePackTree(root);
    CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

int OnlineInstall(const wchar_t* requested_root, const wchar_t* asset_directory = nullptr) {
    // Only own a newly created directory, never delete or reuse caller data.
    wchar_t absolute[32768]{};
    const DWORD length = GetFullPathNameW(requested_root, 32768, absolute, nullptr);
    if (!length || length >= 32768 || !CreateDirectoryW(absolute, nullptr)) {
        std::printf("FAIL: online root must be a new directory with an existing parent (error %lu)\n", GetLastError());
        return 1;
    }
    const std::wstring root = absolute;
    UpdateResponseReader reader = ReadUpdateResponse;
    if (asset_directory) {
        const std::wstring source = asset_directory;
        reader = [source](std::wstring_view url, uint64_t maximum, const std::atomic<bool>& cancelled,
                          const std::function<bool(const void*, DWORD)>& consume,
                          UpdateError& category, DWORD& error) {
            const auto slash = url.find_last_of(L'/');
            if (slash == std::wstring_view::npos) { error = ERROR_INVALID_NAME; return false; }
            const auto bytes = ReadAll(source + L"\\" + std::wstring(url.substr(slash + 1)));
            if (bytes.empty()) { category = UpdateError::LocalIo; error = ERROR_FILE_NOT_FOUND; return false; }
            if (bytes.size() > maximum) { category = UpdateError::ResponseTooLarge; error = ERROR_FILE_TOO_LARGE; return false; }
            for (size_t offset = 0; offset < bytes.size(); offset += 65536) {
                if (cancelled.load()) { error = ERROR_CANCELLED; return false; }
                const auto count = static_cast<DWORD>((std::min)(size_t{65536}, bytes.size() - offset));
                if (!consume(bytes.data() + offset, count)) { error = ERROR_WRITE_FAULT; return false; }
            }
            return true;
        };
        std::printf("Source: downloaded catalog assets (network transport is not under test)\n");
    }
    const PackRelease* releases[] = {&kMediaPackRelease, &kImagePackRelease, &kRawPackRelease};
    const packs::PackId ids[] = {packs::PackId::Media, packs::PackId::Images, packs::PackId::Raw};
    for (size_t p = 0; p < std::size(releases); ++p) {
        const auto& release = *releases[p];
        std::printf("Installing %s: %ls %ls\n", asset_directory ? "downloaded assets" : "online", release.key, release.version);
        std::fflush(stdout);
        PackInstallProgress progress;
        DWORD error = 0;
        // Local asset mode still exercises production verification, decompression and commit.
        const auto outcome = InstallPack(release, root, progress, error, reader);
        Check(outcome == PackInstallOutcome::Installed && error == ERROR_SUCCESS, "online pack installs");
        if (outcome != PackInstallOutcome::Installed) {
            std::printf("FAIL: online install %ls returned error %lu\n", release.key, error);
            break;
        }
        const std::wstring base = root + L"\\" + release.key;
        const std::wstring directory = base + L"\\" + release.version;
        uint64_t total = 0;
        for (size_t i = 0; i < release.file_count; ++i) {
            const auto& file = release.files[i];
            const auto bytes = ReadAll(directory + L"\\" + file.name);
            const bool valid = bytes.size() == file.size && Sha256Hex(bytes) == file.sha256;
            Check(valid, "online installed file size and SHA-256 match catalog");
            if (!valid) std::printf("FAIL: installed file %ls/%ls\n", release.key, file.name);
            total += file.packed_size;
        }
        std::wstring manifest;
        const std::wstring expected = L"{\"version\":\"" + std::wstring(release.version) +
            L"\",\"dir\":\"" + release.version + L"\"}\n";
        Check(ReadUtf8File(base + L"\\installed.json", manifest) && manifest == expected,
              "online manifest identifies the installed version and directory");
        const DWORD tool_attributes = GetFileAttributesW((directory + L"\\" + packs::PackMainTool(ids[p])).c_str());
        Check(tool_attributes != INVALID_FILE_ATTRIBUTES && !(tool_attributes & FILE_ATTRIBUTE_DIRECTORY),
              "online main tool exists as a file");
        Check(!Exists(directory + L".partial"), "online install leaves no staging directory");
        Check(total > 0 && progress.total.load() == total && progress.received.load() == total,
              "online compressed byte count matches catalog");
        if (g_failed) break;
    }
    if (g_failed) Check(RemovePackTree(root) && !Exists(root), "failed online test directory cleaned up");
    else std::printf("Verified packs retained at %ls\n", root.c_str());
    std::printf("pack_installer_test %s: %d passed, %d failed\n",
        asset_directory ? "catalog assets" : "online", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && !wcscmp(argv[1], L"--catalog-assets")) return OnlineInstall(argv[3], argv[2]);
    if (argc == 3 && !wcscmp(argv[1], L"--online")) return OnlineInstall(argv[2]);
    if (argc != 1) {
        std::printf("Usage: pulse_pack_installer_test [--online <new-isolated-root> | --catalog-assets <downloads> <new-isolated-root>]\n");
        return 2;
    }
    const auto defaults = packs::ParsePackSettings(L"{}");
    Check(defaults.enabled[2] && defaults.enabled[3], "new packs default enabled for existing settings");
    auto settings = defaults;
    settings.enabled[2] = false;
    settings.enabled[3] = false;
    const auto roundtrip = packs::ParsePackSettings(packs::SerializePackSettings(settings));
    Check(!roundtrip.enabled[2] && !roundtrip.enabled[3] && roundtrip.enabled[0] && roundtrip.enabled[1],
          "RAW and archive enable states round-trip independently");
    Check(std::wstring(packs::PackMainTool(packs::PackId::Raw)) == L"pulse-rawpack.exe" &&
          std::wstring(packs::PackMainTool(packs::PackId::Archives)).empty(), "retired archive pack has no executable route");

    const std::wstring root = MakeRoot();
    const std::wstring base = root + L"\\ffmpeg";

    {   // A chosen local source never silently falls back to an installed pack.
        wchar_t previous[32768]{};
        GetEnvironmentVariableW(L"LOCALAPPDATA", previous, 32768);
        const std::wstring profile = root + L"\\source-profile";
        SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
        const std::wstring managed = packs::PacksRoot() + L"\\ffmpeg";
        Check(packs::CreateDirectoryChain(managed + L"\\test"), "isolated source profile created");
        WriteUtf8FileAtomic(managed + L"\\test\\ffmpeg.exe", L"fixture");
        WriteUtf8FileAtomic(managed + L"\\installed.json", L"{\"version\":\"test\",\"dir\":\"test\"}");
        packs::PackSettings source;
        Check(packs::SavePackSettings(source) && packs::ResolvePackUncached(packs::PackId::Media).source == packs::ToolSource::Pack,
              "downloaded pack works without a local FFmpeg");
        source.custom_ffmpeg = profile + L"\\ffmpeg.exe";
        WriteUtf8FileAtomic(source.custom_ffmpeg, L"local fixture");
        source.use_custom_ffmpeg = true;
        Check(packs::SavePackSettings(source) && packs::ResolvePackUncached(packs::PackId::Media).source == packs::ToolSource::Custom,
              "explicit local FFmpeg has priority");
        DeleteFileW(source.custom_ffmpeg.c_str());
        Check(packs::ResolvePackUncached(packs::PackId::Media).source == packs::ToolSource::None,
              "missing local source does not silently select managed pack");
        source.use_custom_ffmpeg = false;
        Check(packs::SavePackSettings(source) && packs::ResolvePackUncached(packs::PackId::Media).source == packs::ToolSource::Pack,
              "turning off local source restores downloaded pack");
        source.enabled[0] = false;
        Check(packs::SavePackSettings(source) && packs::ResolvePackUncached(packs::PackId::Media).source == packs::ToolSource::None,
              "media enable switch applies to selected source");
        SetEnvironmentVariableW(L"LOCALAPPDATA", previous[0] ? previous : nullptr);
    }

    {   // Happy path, with an older version and a stale staging folder around.
        Fixture fx(L"7.1.1");
        Check(!fx.packed[0].empty() && fx.packed[0].size() < fx.plain[0].size(), "LZMS shrinks the fixture");
        CreateDirectoryW(base.c_str(), nullptr);
        CreateDirectoryW((base + L"\\7.0").c_str(), nullptr);
        WriteUtf8FileAtomic(base + L"\\7.0\\ffmpeg.exe", L"old");
        CreateDirectoryW((base + L"\\7.1.1.partial").c_str(), nullptr);
        WriteUtf8FileAtomic(base + L"\\7.1.1.partial\\junk", L"junk");
        PackInstallProgress progress;
        DWORD error = 0;
        const auto outcome = InstallPack(fx.release, root, progress, error, fx.Reader());
        Check(outcome == PackInstallOutcome::Installed, "pack installs");
        Check(error == ERROR_SUCCESS, "no error on success");
        Check(ReadAll(base + L"\\7.1.1\\ffmpeg.exe") == fx.plain[0], "big file decompressed byte-exact");
        Check(ReadAll(base + L"\\7.1.1\\SOURCE.txt") == fx.plain[1], "small file decompressed byte-exact");
        Check(!Exists(base + L"\\7.1.1\\ffmpeg.exe.lzms"), "compressed download removed");
        Check(!Exists(base + L"\\7.1.1\\junk"), "stale staging content not carried over");
        Check(!Exists(base + L"\\7.1.1.partial"), "staging folder renamed away");
        Check(!Exists(base + L"\\7.0"), "older version removed");
        std::wstring json;
        Check(ReadUtf8File(base + L"\\installed.json", json) &&
              json.find(L"\"version\":\"7.1.1\"") != std::wstring::npos &&
              json.find(L"\"dir\":\"7.1.1\"") != std::wstring::npos, "installed.json names the version");
        Check(progress.total.load() == fx.packed[0].size() + fx.packed[1].size() &&
              progress.received.load() == progress.total.load(), "progress reaches the total");

        // Reinstalling the same version replaces it in place.
        PackInstallProgress again;
        Check(InstallPack(fx.release, root, again, error, fx.Reader(65536)) == PackInstallOutcome::Installed,
              "same version reinstalls");
        Check(ReadAll(base + L"\\7.1.1\\ffmpeg.exe") == fx.plain[0], "reinstalled file intact");
    }

    {   // Shared release assets can be prefixed without changing installed names.
        Fixture fx(L"renamed");
        fx.files[0].download_name = L"media-ffmpeg.exe.lzms";
        const std::wstring original = L"https://packs.example.invalid/v/ffmpeg.exe.lzms";
        fx.server[L"https://packs.example.invalid/v/media-ffmpeg.exe.lzms"] = fx.server.at(original);
        fx.server.erase(original);
        PackInstallProgress progress;
        DWORD error = 0;
        const std::wstring isolated = root + L"\\renamed-test";
        Check(InstallPack(fx.release, isolated, progress, error, fx.Reader()) == PackInstallOutcome::Installed,
              "renamed release asset downloads successfully");
        const std::wstring target = isolated + L"\\ffmpeg\\renamed";
        Check(ReadAll(target + L"\\ffmpeg.exe") == fx.plain[0] && !Exists(target + L"\\media-ffmpeg.exe.lzms"),
              "renamed asset preserves original installed filename and bytes");
        Check(fx.files[1].download_name == nullptr && ReadAll(target + L"\\SOURCE.txt") == fx.plain[1],
              "legacy five-field catalog entry still uses original download name");
    }

    {   // Invalid asset names must fail before making a network request or directory.
        Fixture fx(L"invalid-asset");
        const wchar_t* invalid[] = {L"", L".", L"..", L"../evil.lzms", L"..\\evil.lzms",
                                    L"https://evil.invalid/a", L"file?query", L"file#fragment", L"file%2fevil", L"C:evil"};
        bool requested = false;
        const UpdateResponseReader reader = [&](std::wstring_view, uint64_t, const std::atomic<bool>&,
            const std::function<bool(const void*, DWORD)>&, UpdateError&, DWORD&) { requested = true; return false; };
        for (const wchar_t* name : invalid) {
            fx.files[0].download_name = name;
            PackInstallProgress progress;
            DWORD error = 0;
            Check(InstallPack(fx.release, root, progress, error, reader) == PackInstallOutcome::Failed &&
                  error == ERROR_INVALID_PARAMETER, "unsafe download asset name rejected");
        }
        Check(!requested && !Exists(base + L"\\invalid-asset.partial") && !Exists(base + L"\\invalid-asset"),
              "unsafe assets cause no downloads or staging changes");
    }

    {   // A tampered download is discarded and the installed pack stays.
        Fixture fx(L"7.2");
        auto& body = fx.server[L"https://packs.example.invalid/v/ffmpeg.exe.lzms"];
        Check(body.size() > 1000, "tamper target is the big file");
        body[body.size() / 2] ^= 0x5a;
        PackInstallProgress progress;
        DWORD error = 0;
        Check(InstallPack(fx.release, root, progress, error, fx.Reader()) == PackInstallOutcome::Failed,
              "tampered download fails");
        Check(error == ERROR_INVALID_DATA, "tampering reports invalid data");
        Check(progress.failure.load() == static_cast<uint32_t>(PackFailure::Mismatch), "tampering is a mismatch");
        Check(!Exists(base + L"\\7.2") && !Exists(base + L"\\7.2.partial"), "nothing of the failed version stays");
        std::wstring json;
        Check(ReadUtf8File(base + L"\\installed.json", json) && json.find(L"7.1.1") != std::wstring::npos,
              "previous installed.json untouched");
        Check(Exists(base + L"\\7.1.1\\ffmpeg.exe"), "previous pack untouched");
    }

    {   // The decompressed file must match its own hash too.
        Fixture fx(L"7.3");
        fx.sha[1][0] = fx.sha[1][0] == L'0' ? L'1' : L'0';
        fx.files[1].sha256 = fx.sha[1].c_str();
        PackInstallProgress progress;
        DWORD error = 0;
        Check(InstallPack(fx.release, root, progress, error, fx.Reader()) == PackInstallOutcome::Failed &&
              error == ERROR_INVALID_DATA, "plain hash mismatch fails");
    }

    {   // A response longer than pinned is cut off.
        Fixture fx(L"7.4");
        fx.files[1].packed_size -= 1;
        PackInstallProgress progress;
        DWORD error = 0;
        Check(InstallPack(fx.release, root, progress, error, fx.Reader()) == PackInstallOutcome::Failed,
              "oversized response fails");
        Check(progress.failure.load() == static_cast<uint32_t>(PackFailure::Mismatch), "oversized response is a mismatch");
        Check(!Exists(base + L"\\7.4.partial"), "oversized response leaves no staging");
    }

    {   // Missing file on the server.
        Fixture fx(L"7.5");
        fx.server.clear();
        PackInstallProgress progress;
        DWORD error = 0;
        Check(InstallPack(fx.release, root, progress, error, fx.Reader()) == PackInstallOutcome::Failed,
              "404 fails");
        Check(progress.failure.load() == static_cast<uint32_t>(PackFailure::Network), "404 is a network failure");
    }

    {   // A release on GitHub: a mirror with another build's file (wrong hash,
        // then too large) is skipped and the next source installs the pack.
        Fixture fx(L"7.45");
        const std::wstring github = L"https://github.com/jimmgreen/pulse/releases/download/v7/";
        std::map<std::wstring, std::vector<uint8_t>> moved;
        for (size_t i = 0; i < fx.names.size(); ++i) {
            const std::wstring asset = github + fx.names[i] + L".lzms";
            auto tampered = fx.packed[i];
            tampered[tampered.size() / 2] ^= 0x5a;
            moved[L"https://ghproxy.net/" + asset] = tampered;
            auto longer = fx.packed[i];
            longer.push_back(0);
            moved[L"https://gh-proxy.com/" + asset] = longer;
            moved[asset] = fx.packed[i];
        }
        fx.server = moved;
        fx.release.base_url = github.c_str();
        std::vector<std::wstring> asked;
        auto inner = fx.Reader();
        UpdateResponseReader reader = [&](std::wstring_view url, uint64_t maximum, const std::atomic<bool>& cancelled,
                                          const std::function<bool(const void*, DWORD)>& consume,
                                          UpdateError& category, DWORD& error) {
            asked.emplace_back(url);
            return inner(url, maximum, cancelled, consume, category, error);
        };
        PackInstallProgress progress;
        DWORD error = 0;
        Check(InstallPack(fx.release, root, progress, error, reader) == PackInstallOutcome::Installed &&
              error == ERROR_SUCCESS, "wrong mirror files fall back to GitHub");
        Check(asked.size() == 6 && ReadAll(base + L"\\7.45\\ffmpeg.exe") == fx.plain[0],
              "each file tries both mirrors once, then GitHub");
        // Every source wrong: a mismatch, not a network failure.
        fx.server[github + fx.names[0] + L".lzms"].push_back(1);
        fx.server.erase(github + fx.names[1] + L".lzms");
        asked.clear();
        PackInstallProgress again;
        Check(InstallPack(fx.release, root, again, error, reader) == PackInstallOutcome::Failed &&
              again.failure.load() == static_cast<uint32_t>(PackFailure::Mismatch) && asked.size() == 3,
              "all sources wrong reports a mismatch after three tries");
        // A fixed source is the only one asked.
        SetUpdateSource(UpdateSource::Mirror2);
        asked.clear();
        PackInstallProgress fixed;
        Check(InstallPack(fx.release, root, fixed, error, reader) == PackInstallOutcome::Failed && asked.size() == 1 &&
              asked[0].starts_with(L"https://gh-proxy.com/"), "a chosen source is the only one tried");
        SetUpdateSource(UpdateSource::Automatic);
    }

    {   // Cancelling mid-download.
        Fixture fx(L"7.6");
        PackInstallProgress progress;
        DWORD error = 0;
        Check(InstallPack(fx.release, root, progress, error, fx.Reader(7000, &progress.cancelled)) ==
              PackInstallOutcome::Cancelled, "cancel reports cancelled");
        Check(error == ERROR_CANCELLED, "cancel error code");
        Check(!Exists(base + L"\\7.6") && !Exists(base + L"\\7.6.partial"), "cancel leaves nothing");
    }

    {   // Catalog values never become paths outside the pack folder.
        Fixture fx(L"..");
        PackInstallProgress progress;
        DWORD error = 0;
        Check(InstallPack(fx.release, root, progress, error, fx.Reader()) == PackInstallOutcome::Failed &&
              error == ERROR_INVALID_PARAMETER, "'..' version rejected");
        Fixture bad_name(L"8.0");
        bad_name.files[0].name = L"..\\evil.exe";
        Check(InstallPack(bad_name.release, root, progress, error, bad_name.Reader()) == PackInstallOutcome::Failed &&
              error == ERROR_INVALID_PARAMETER, "path in file name rejected");
        Fixture http(L"8.1");
        http.release.base_url = L"http://packs.example.invalid/v/";
        Check(InstallPack(http.release, root, progress, error, http.Reader()) == PackInstallOutcome::Failed &&
              error == ERROR_INVALID_PARAMETER, "plain http rejected");
        PackRelease empty{L"ffmpeg", L"9.0", L"https://x.invalid/", nullptr, 0};
        Check(InstallPack(empty, root, progress, error) == PackInstallOutcome::Failed, "unpublished pack rejected");
        Check(!Exists(base + L"\\8.0") && !Exists(base + L"\\8.1"), "rejected releases create nothing");
    }

    {   // Catalog versions are dotted numbers, compared part by part.
        using packs::PackUpdateAvailable;
        Check(PackUpdateAvailable(L"1.0.1", L"1.0.2") && PackUpdateAvailable(L"1.0.9", L"1.0.10") &&
              PackUpdateAvailable(L"1.9", L"1.10"), "newer catalog release is an update");
        Check(!PackUpdateAvailable(L"1.0.2", L"1.0.2") && !PackUpdateAvailable(L"1.0", L"1.0.0") &&
              !PackUpdateAvailable(L"1.0.10", L"1.0.9") && !PackUpdateAvailable(L"2.0", L"1.9.9"),
              "same or older catalog release is no update");
        Check(PackUpdateAvailable(L"", L"1.0.2") && PackUpdateAvailable(L"test", L"1.0.2") &&
              PackUpdateAvailable(L"1.0.", L"1.0.2"), "unreadable installed version is replaced");
        Check(!PackUpdateAvailable(L"1.0.1", L"") && !PackUpdateAvailable(L"1.0.1", L"1.0.2-beta") &&
              !PackUpdateAvailable(L"1.0.1", L"4294967296.0") && !PackUpdateAvailable(L"", L"1.2.3.4.5.6.7.8.9"),
              "unreadable catalog version is never offered");
        bool parses = true;
        for (const PackRelease* release : {&kMediaPackRelease, &kImagePackRelease, &kRawPackRelease})
            parses &= !release->file_count || PackUpdateAvailable(L"", release->version);
        Check(parses, "published catalog versions can be offered as updates");
    }

    {   // Background updates take an installed pack in use, and only a newer release.
        using packs::PackAutoUpdateWanted;
        using packs::PackId;
        const packs::PackSettings on{};
        packs::InstalledPack older;
        older.present = true;
        older.version = L"1.0.1";
        auto current = older;
        current.version = L"1.0.2";
        const packs::InstalledPack missing{};
        Check(PackAutoUpdateWanted(PackId::Raw, on, older, L"1.0.2") &&
              PackAutoUpdateWanted(PackId::Media, on, older, L"1.0.2"), "an older pack in use updates in the background");
        Check(!PackAutoUpdateWanted(PackId::Raw, on, current, L"1.0.2") &&
              !PackAutoUpdateWanted(PackId::Raw, on, missing, L"1.0.2") &&
              !PackAutoUpdateWanted(PackId::Raw, on, older, L""), "current, missing and unpublished packs stay");
        auto off = on;
        off.auto_update = false;
        auto switched_off = on;
        switched_off.enabled[static_cast<uint32_t>(PackId::Raw)] = false;
        auto own_ffmpeg = on;
        own_ffmpeg.use_custom_ffmpeg = true;
        Check(!PackAutoUpdateWanted(PackId::Raw, off, older, L"1.0.2") &&
              !PackAutoUpdateWanted(PackId::Raw, switched_off, older, L"1.0.2") &&
              PackAutoUpdateWanted(PackId::Images, switched_off, older, L"1.0.2") &&
              !PackAutoUpdateWanted(PackId::Media, own_ffmpeg, older, L"1.0.2") &&
              PackAutoUpdateWanted(PackId::Raw, own_ffmpeg, older, L"1.0.2"),
              "the setting, a switched-off pack and the user's own FFmpeg keep packs as they are");
        Check(on.auto_update && packs::ParsePackSettings(L"{}").auto_update &&
              !packs::ParsePackSettings(packs::SerializePackSettings(off)).auto_update,
              "background updates default on and turning them off is saved");
    }

    {   // installed.json names a directory inside its pack, nothing else.
        const std::wstring probe = root + L"\\probe";
        Check(packs::CreateDirectoryChain(probe), "probe pack folder created");
        std::wstring version;
        WriteUtf8FileAtomic(probe + L"\\installed.json", L"{\"version\":\"1.0.1\",\"dir\":\"1.0.1\"}");
        Check(packs::InstalledPackDirectory(probe, &version) == probe + L"\\1.0.1" && version == L"1.0.1",
              "installed directory and version read back");
        bool contained = true;
        for (const wchar_t* dir : {L"..", L".", L"../other", L"C:other", L""}) {
            WriteUtf8FileAtomic(probe + L"\\installed.json",
                                L"{\"version\":\"1.0.1\",\"dir\":\"" + std::wstring(dir) + L"\"}");
            contained &= packs::InstalledPackDirectory(probe).empty();
        }
        Check(contained, "installed.json cannot point outside its pack");
    }

    {   // An update copies what the installed version already has, verified,
        // and downloads only what changed.
        const std::wstring isolated = root + L"\\update-test";
        const std::wstring media = isolated + L"\\ffmpeg";
        std::vector<std::wstring> requested;
        auto counting = [&requested](UpdateResponseReader inner) -> UpdateResponseReader {
            return [&requested, inner](std::wstring_view url, uint64_t maximum, const std::atomic<bool>& cancelled,
                                       const std::function<bool(const void*, DWORD)>& consume,
                                       UpdateError& category, DWORD& error) {
                requested.emplace_back(url);
                return inner(url, maximum, cancelled, consume, category, error);
            };
        };
        auto fetched = [&requested](const wchar_t* name) {
            for (const auto& url : requested) if (url.find(name) != std::wstring::npos) return true;
            return false;
        };
        DWORD error = 0;
        Fixture old_release(L"1.0.1");
        PackInstallProgress first;
        Check(InstallPack(old_release.release, isolated, first, error, counting(old_release.Reader()), true) ==
              PackInstallOutcome::Installed && fetched(L"ffmpeg.exe") && fetched(L"SOURCE.txt"),
              "first install with reuse on downloads every file");

        const std::string changed = "Pulse FFmpeg preview pack 1.0.2\r\nLicense: LGPL-2.1\r\n";
        Fixture fx(L"1.0.2");
        fx.Change(1, changed);
        requested.clear();
        PackInstallProgress progress;
        Check(InstallPack(fx.release, isolated, progress, error, counting(fx.Reader()), true) ==
              PackInstallOutcome::Installed && error == ERROR_SUCCESS, "update installs");
        Check(!fetched(L"ffmpeg.exe") && fetched(L"SOURCE.txt"), "update downloads only the changed file");
        Check(ReadAll(media + L"\\1.0.2\\ffmpeg.exe") == fx.plain[0] &&
              ReadAll(media + L"\\1.0.2\\SOURCE.txt") == fx.plain[1], "updated pack has the new release's bytes");
        Check(!Exists(media + L"\\1.0.1") && !Exists(media + L"\\1.0.2.partial"), "update replaces the old version");
        std::wstring version;
        Check(packs::InstalledPackDirectory(media, &version) == media + L"\\1.0.2" && version == L"1.0.2",
              "installed.json names the update");
        Check(progress.total.load() == fx.packed[0].size() + fx.packed[1].size() &&
              progress.received.load() == progress.total.load(), "copied files count towards progress");

        // An installed file with the right size but other bytes is downloaded again.
        const std::wstring damaged_path = media + L"\\1.0.2\\ffmpeg.exe";
        auto damaged = ReadAll(damaged_path);
        FILE* out = nullptr;
        if (!damaged.empty() && !_wfopen_s(&out, damaged_path.c_str(), L"wb") && out) {
            damaged[damaged.size() / 2] ^= 0x5a;
            fwrite(damaged.data(), 1, damaged.size(), out);
            fclose(out);
        }
        Check(ReadAll(damaged_path).size() == fx.plain[0].size() && ReadAll(damaged_path) != fx.plain[0],
              "installed file damaged for the test");
        Fixture repair(L"1.0.3");
        repair.Change(1, changed);
        requested.clear();
        PackInstallProgress again;
        Check(InstallPack(repair.release, isolated, again, error, counting(repair.Reader()), true) ==
              PackInstallOutcome::Installed, "update over a damaged file installs");
        Check(fetched(L"ffmpeg.exe") && !fetched(L"SOURCE.txt"), "a file failing its hash is downloaded, a matching one copied");
        Check(ReadAll(media + L"\\1.0.3\\ffmpeg.exe") == repair.plain[0], "damaged file replaced by verified bytes");

        // Without reuse every file is downloaded, which the failure cases above rely on.
        Fixture fresh(L"1.0.4");
        requested.clear();
        PackInstallProgress full;
        Check(InstallPack(fresh.release, isolated, full, error, counting(fresh.Reader())) == PackInstallOutcome::Installed &&
              fetched(L"ffmpeg.exe") && fetched(L"SOURCE.txt"), "reusing installed files is opt-in");
    }

    {   // Async wrapper: one outcome, taken once.
        PackRelease empty{L"ffmpeg", L"9.0", L"https://x.invalid/", nullptr, 0};
        PackInstaller installer;
        Check(installer.Start(empty, nullptr), "installer starts");
        for (int i = 0; i < 200 && installer.running(); ++i) Sleep(10);
        Check(!installer.running(), "installer finishes");
        DWORD error = 0;
        Check(installer.TakeOutcome(error) == PackInstallOutcome::Failed, "outcome reported");
        Check(installer.TakeOutcome(error) == PackInstallOutcome::None, "outcome reported once");
    }

    RemovePackTree(root);
    std::printf("pack_installer_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
