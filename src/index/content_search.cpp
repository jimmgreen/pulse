#include "content_search.h"
#include "../common/text_decode.h"

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>

namespace pulse::index {
namespace {

struct Candidate {
    std::wstring path;
    std::wstring name;
    uint64_t size = 0;
    uint64_t modified = 0;
    std::array<uint8_t, 16> file_id{};
    uint64_t volume = 0;
};

uint64_t FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

std::wstring FileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

size_t FindText(std::wstring_view text_value, std::wstring_view needle,
                bool case_sensitive) {
    if (needle.empty()) return std::wstring_view::npos;
    auto equal = [case_sensitive](wchar_t left, wchar_t right) {
        return case_sensitive ? left == right : towlower(left) == towlower(right);
    };
    const auto found = std::search(text_value.begin(), text_value.end(),
                                   needle.begin(), needle.end(), equal);
    return found == text_value.end() ? std::wstring_view::npos
                                     : static_cast<size_t>(found - text_value.begin());
}

ContentHit MakeContentHit(const Candidate& file, std::wstring_view content, size_t match) {
    ContentHit hit;
    hit.path = file.path;
    hit.name = file.name;
    hit.size = file.size;
    hit.modified = file.modified;
    hit.line = 1;
    for (size_t i = 0; i < match; ++i) if (content[i] == L'\n') ++hit.line;
    size_t line_start = content.rfind(L'\n', match);
    line_start = line_start == std::wstring_view::npos ? 0 : line_start + 1;
    size_t line_end = content.find(L'\n', match);
    if (line_end == std::wstring_view::npos) line_end = content.size();
    constexpr size_t kSnippetChars = 240;
    if (line_end - line_start > kSnippetChars) {
        const size_t before = (std::min)(match - line_start, kSnippetChars / 3);
        line_start = match - before;
        line_end = (std::min)(content.size(), line_start + kSnippetChars);
    }
    hit.snippet.assign(content.substr(line_start, line_end - line_start));
    for (wchar_t& c : hit.snippet) if (c == L'\r' || c == L'\n') c = L' ';
    return hit;
}

bool ReadCandidate(const std::filesystem::directory_entry& item, Candidate& out) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const std::wstring path = item.path().wstring();
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ||
        text::IsOfflinePlaceholder(data.dwFileAttributes)) return false;
    ULARGE_INTEGER size{data.nFileSizeLow, data.nFileSizeHigh};
    out.path = path;
    out.name = item.path().filename().wstring();
    out.size = size.QuadPart;
    out.modified = FileTimeValue(data.ftLastWriteTime);
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        FILE_ID_INFO info{};
        if (GetFileInformationByHandleEx(file, FileIdInfo, &info, sizeof(info))) {
            out.volume = info.VolumeSerialNumber;
            memcpy(out.file_id.data(), info.FileId.Identifier, out.file_id.size());
        }
        CloseHandle(file);
    }
    return true;
}

bool EnumerateCandidates(const ContentSearchRequest& request,
                         const std::atomic<bool>& cancelled,
                         std::vector<Candidate>& files, ContentSearchProgress& progress,
                         const ContentBatchCallback& callback) {
    namespace fsys = std::filesystem;
    std::error_code error;
    const auto options = fsys::directory_options::skip_permission_denied;
    auto last_progress = std::chrono::steady_clock::now();
    auto publish_progress = [&] {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_progress < std::chrono::milliseconds(250)) return true;
        last_progress = now;
        return callback(progress, {});
    };
    if (request.recursive) {
        fsys::recursive_directory_iterator it(fsys::path(request.root), options, error);
        fsys::recursive_directory_iterator end;
        if (error) { progress.error = error.value(); return false; }
        for (; it != end && !cancelled.load(); it.increment(error)) {
            if (error) { error.clear(); continue; }
            if (it->is_directory(error)) {
                const DWORD attributes = GetFileAttributesW(it->path().c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES &&
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            Candidate candidate;
            if (ReadCandidate(*it, candidate)) files.push_back(std::move(candidate));
            if (!publish_progress()) return false;
        }
    } else {
        fsys::directory_iterator it(fsys::path(request.root), options, error);
        fsys::directory_iterator end;
        if (error) { progress.error = error.value(); return false; }
        for (; it != end && !cancelled.load(); it.increment(error)) {
            if (error) { error.clear(); continue; }
            Candidate candidate;
            if (ReadCandidate(*it, candidate)) files.push_back(std::move(candidate));
            if (!publish_progress()) return false;
        }
    }
    return !cancelled.load();
}

uint64_t SampleHash(const Candidate& file) {
    HANDLE handle = CreateFileW(file.path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    constexpr DWORD kSample = 64 * 1024;
    std::vector<uint8_t> bytes(kSample * 2);
    DWORD first = 0;
    bool ok = ReadFile(handle, bytes.data(), kSample, &first, nullptr) != FALSE;
    DWORD last = 0;
    if (ok && file.size > kSample) {
        LARGE_INTEGER offset{};
        offset.QuadPart = static_cast<LONGLONG>(file.size - kSample);
        ok = SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN) &&
             ReadFile(handle, bytes.data() + kSample, kSample, &last, nullptr);
    }
    CloseHandle(handle);
    if (!ok) return 0;
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < static_cast<size_t>(first + last); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool FullSha256(const Candidate& file, const std::atomic<bool>& cancelled,
                std::array<uint8_t, 32>& output) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD returned = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &returned, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::vector<uint8_t> object(object_size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    HANDLE handle = CreateFileW(file.path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    bool ok = handle != INVALID_HANDLE_VALUE;
    std::vector<uint8_t> bytes(1024 * 1024);
    while (ok && !cancelled.load()) {
        DWORD read = 0;
        if (!ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (!read) break;
        if (BCryptHashData(hash, bytes.data(), read, 0) < 0) { ok = false; break; }
    }
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    if (ok && !cancelled.load() && BCryptFinishHash(hash, output.data(),
        static_cast<ULONG>(output.size()), 0) < 0) ok = false;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok && !cancelled.load();
}

std::wstring DigestKey(const std::array<uint8_t, 32>& digest) {
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring key;
    key.reserve(digest.size() * 2);
    for (uint8_t byte : digest) {
        key.push_back(digits[byte >> 4]);
        key.push_back(digits[byte & 15]);
    }
    return key;
}

std::wstring FileIdentity(const Candidate& file) {
    if (!file.volume) return file.path;
    std::wstring value = std::to_wstring(file.volume) + L":";
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    for (uint8_t byte : file.file_id) {
        value.push_back(digits[byte >> 4]);
        value.push_back(digits[byte & 15]);
    }
    return value;
}

bool RunDuplicateSearch(const ContentSearchRequest& request,
                        const std::atomic<bool>& cancelled,
                        const std::vector<Candidate>& files,
                        ContentSearchProgress& progress,
                        const ContentBatchCallback& callback) {
    std::map<uint64_t, std::vector<const Candidate*>> by_size;
    for (const auto& file : files) by_size[file.size].push_back(&file);
    uint32_t group_id = 1;
    std::vector<ContentHit> batch;
    size_t total_hits = 0;
    for (auto& [size, candidates] : by_size) {
        if (cancelled.load()) return false;
        if (candidates.size() < 2) continue;
        std::map<uint64_t, std::vector<const Candidate*>> by_sample;
        for (const Candidate* candidate : candidates)
            by_sample[SampleHash(*candidate)].push_back(candidate);
        for (auto& [sample, sampled] : by_sample) {
            if (sampled.size() < 2 || sample == 0) continue;
            std::map<std::wstring, std::vector<const Candidate*>> exact;
            for (const Candidate* candidate : sampled) {
                std::array<uint8_t, 32> digest{};
                if (FullSha256(*candidate, cancelled, digest))
                    exact[DigestKey(digest)].push_back(candidate);
                progress.scanned_files++;
                progress.scanned_bytes += candidate->size;
            }
            for (auto& [digest, matches] : exact) {
                if (matches.size() < 2) continue;
                std::set<std::wstring> identities;
                std::vector<const Candidate*> unique;
                for (const Candidate* match : matches)
                    if (identities.insert(FileIdentity(*match)).second) unique.push_back(match);
                if (unique.size() < 2) continue;
                for (const Candidate* match : unique) {
                    ContentHit hit;
                    hit.path = match->path;
                    hit.name = match->name;
                    hit.size = match->size;
                    hit.modified = match->modified;
                    hit.group = group_id;
                    batch.push_back(std::move(hit));
                    if (++total_hits >= request.maximum_hits) progress.truncated = true;
                    if (batch.size() >= 64) {
                        if (!callback(progress, std::move(batch))) return false;
                        batch.clear();
                    }
                    if (progress.truncated) break;
                }
                ++group_id;
                if (progress.truncated) break;
            }
            if (progress.truncated) break;
        }
        if (progress.truncated) break;
    }
    if (!batch.empty() && !callback(progress, std::move(batch))) return false;
    return !cancelled.load();
}

} // namespace

bool RunContentSearch(const ContentSearchRequest& request, const std::atomic<bool>& cancelled,
                      ContentBatchCallback callback) {
    ContentSearchProgress progress;
    progress.generation = request.generation;
    if (request.root.empty() || !callback) {
        progress.done = true;
        progress.error = ERROR_INVALID_PARAMETER;
        if (callback) callback(progress, {});
        return false;
    }
    std::vector<Candidate> files;
    if (!EnumerateCandidates(request, cancelled, files, progress, callback)) {
        progress.done = true;
        if (cancelled.load()) progress.error = ERROR_CANCELLED;
        callback(progress, {});
        return false;
    }
    if (request.mode == ContentSearchMode::Duplicates) {
        const bool ok = RunDuplicateSearch(request, cancelled, files, progress, callback);
        progress.done = true;
        if (!ok && cancelled.load()) progress.error = ERROR_CANCELLED;
        callback(progress, {});
        return ok;
    }

    std::vector<ContentHit> batch;
    size_t total_hits = 0;
    for (const auto& file : files) {
        if (cancelled.load()) break;
        if (file.size > request.maximum_file_bytes) continue;
        std::wstring content;
        uint64_t bytes = 0;
        if (!text::ReadFile(file.path, request.maximum_file_bytes, content, bytes)) continue;
        ++progress.scanned_files;
        progress.scanned_bytes += bytes;
        const size_t match = FindText(content, request.needle, request.case_sensitive);
        if (match != std::wstring::npos) {
            batch.push_back(MakeContentHit(file, content, match));
            if (++total_hits >= request.maximum_hits) progress.truncated = true;
        }
        if (batch.size() >= 64) {
            if (!callback(progress, std::move(batch))) return false;
            batch.clear();
        }
        if (progress.truncated) break;
    }
    if (!batch.empty() && !callback(progress, std::move(batch))) return false;
    progress.done = true;
    if (cancelled.load()) progress.error = ERROR_CANCELLED;
    callback(progress, {});
    return !cancelled.load();
}

} // namespace pulse::index
