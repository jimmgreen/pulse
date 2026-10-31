#include "content_search.h"
#include "content_directory_listing.h"
#include "document_reader.h"
#include "../common/text_decode.h"

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <deque>
#include <map>
#include <set>
#include <string_view>
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
                bool case_sensitive, bool whole_word, const std::atomic<bool>* cancelled = nullptr) {
    if (needle.empty()) return std::wstring_view::npos;
    std::array<wchar_t, 128> folded{};
    if (!case_sensitive) for (size_t i = 0; i < folded.size(); ++i) folded[i] = static_cast<wchar_t>(towlower(static_cast<wchar_t>(i)));
    auto equal = [case_sensitive, &folded](wchar_t left, wchar_t right) {
        if (left == right) return true;
        if (case_sensitive) return false;
        return (static_cast<unsigned>(left) < folded.size() ? folded[left] : towlower(left)) ==
            (static_cast<unsigned>(right) < folded.size() ? folded[right] : towlower(right));
    };
    auto word_char = [](wchar_t c) {
        return iswalnum(c) || static_cast<unsigned>(c) > 127;
    };
    auto from = text_value.begin();
    while (from <= text_value.end()) {
        if (cancelled && cancelled->load()) return std::wstring_view::npos;
        auto until = text_value.end();
        constexpr size_t slice = 16384;
        if (cancelled && static_cast<size_t>(until - from) > slice + needle.size()) until = from + slice + needle.size() - 1;
        const auto found = std::search(from, until,
                                       needle.begin(), needle.end(), equal);
        if (found == until) {
            if (until == text_value.end()) return std::wstring_view::npos;
            from += slice; continue;
        }
        const size_t pos = static_cast<size_t>(found - text_value.begin());
        if (!whole_word) return pos;
        const bool left_ok = pos == 0 || !word_char(text_value[pos - 1]);
        const bool right_ok = pos + needle.size() >= text_value.size() ||
                              !word_char(text_value[pos + needle.size()]);
        if (left_ok && right_ok) return pos;
        from = found + 1;
    }
    return std::wstring_view::npos;
}

size_t MatchContent(std::wstring_view text, const ContentSearchRequest& request, const std::atomic<bool>* cancelled = nullptr) {
    for (const auto& excluded : request.excluded_needles) {
        if (FindText(text, excluded, request.case_sensitive, request.whole_word, cancelled) !=
            std::wstring_view::npos)
            return std::wstring_view::npos;
    }
    if (request.match_mode == ContentMatchMode::Phrase) {
        if (!request.needle.empty())
            return FindText(text, request.needle, request.case_sensitive, request.whole_word, cancelled);
        std::wstring phrase;
        for (const auto& word : request.needles) {
            if (!phrase.empty()) phrase.push_back(L' ');
            phrase.append(word);
        }
        return FindText(text, phrase, request.case_sensitive, request.whole_word, cancelled);
    }
    size_t first = std::wstring_view::npos;
    bool has_words = false;
    const auto match_word = [&](std::wstring_view needle) {
        has_words = true;
        const size_t found = FindText(text, needle, request.case_sensitive, request.whole_word, cancelled);
        if (found == std::wstring_view::npos) return request.match_mode == ContentMatchMode::AnyWord;
        first = (std::min)(first, found);
        return true;
    };
    if (!request.needles.empty()) {
        for (const auto& needle : request.needles)
            if (!match_word(needle)) return std::wstring_view::npos;
    } else {
        const std::wstring_view words = request.needle;
        size_t i = 0;
        while (i < words.size()) {
            while (i < words.size() && iswspace(words[i])) ++i;
            size_t j = i;
            while (j < words.size() && !iswspace(words[j])) ++j;
            if (j > i && !match_word(words.substr(i, j - i))) return std::wstring_view::npos;
            i = j;
        }
    }
    return has_words ? first : 0;
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

bool NameEquals(std::wstring_view left, std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool IsSkippedSystemDirectory(std::wstring_view name) {
    return NameEquals(name, L"$Recycle.Bin") ||
           NameEquals(name, L"System Volume Information") ||
           NameEquals(name, L"Windows");
}

bool IsSkippedSystemFile(std::wstring_view name) {
    return NameEquals(name, L"pagefile.sys") ||
           NameEquals(name, L"hiberfil.sys") ||
           NameEquals(name, L"swapfile.sys");
}

std::vector<std::wstring> EffectiveRoots(const ContentSearchRequest& request) {
    if (!request.roots.empty()) return request.roots;
    if (!request.root.empty()) return {request.root};
    return {};
}

std::wstring JoinPath(const std::wstring& dir, const wchar_t* name) {
    std::wstring value = dir;
    if (!value.empty() && value.back() != L'\\' && value.back() != L'/') value.push_back(L'\\');
    value.append(name);
    return value;
}

std::wstring NormalizeWalkRoot(std::wstring path) {
    if (path.size() == 2 && path[1] == L':') path.push_back(L'\\');
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

std::wstring Win32Path(const std::wstring& path) {
    if (path.size() >= 4 && path.compare(0, 4, L"\\\\?\\") == 0) return path;
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
        return L"\\\\?\\UNC" + path.substr(1);
    return L"\\\\?\\" + path;
}

bool LoadCandidatePaths(const ContentSearchRequest& request,
                        std::vector<Candidate>& files, ContentSearchProgress& progress,
                        const ContentBatchCallback& callback) {
    progress.current_root.clear();
    progress.total_files = request.candidate_paths.size();
    if (!callback(progress, {})) return false;
    for (const auto& path : request.candidate_paths) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(Win32Path(path).c_str(), GetFileExInfoStandard, &data))
            continue;
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                     FILE_ATTRIBUTE_DEVICE) ||
            text::IsOfflinePlaceholder(data.dwFileAttributes)) continue;
        ULARGE_INTEGER size{data.nFileSizeLow, data.nFileSizeHigh};
        if (size.QuadPart < request.minimum_file_bytes) continue;
        Candidate candidate;
        candidate.path = path;
        candidate.name = FileName(path);
        candidate.size = size.QuadPart;
        candidate.modified = FileTimeValue(data.ftLastWriteTime);
        files.push_back(std::move(candidate));
    }
    progress.total_files = files.size();
    return callback(progress, {});
}

bool IsDotOrDotDot(const wchar_t* name) {
    return name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

bool ReadCandidate(const std::wstring& path, const WIN32_FIND_DATAW& data,
                   uint64_t minimum_file_bytes, Candidate& out) {
    if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                 FILE_ATTRIBUTE_DEVICE) ||
        text::IsOfflinePlaceholder(data.dwFileAttributes)) return false;
    ULARGE_INTEGER size{data.nFileSizeLow, data.nFileSizeHigh};
    if (size.QuadPart < minimum_file_bytes) return false;
    out.path = path;
    out.name = FileName(path);
    out.size = size.QuadPart;
    out.modified = FileTimeValue(data.ftLastWriteTime);
    return true;
}

void EnsureFileId(Candidate& file) {
    if (file.volume) return;
    HANDLE handle = CreateFileW(Win32Path(file.path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    FILE_ID_INFO info{};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) {
        file.volume = info.VolumeSerialNumber;
        memcpy(file.file_id.data(), info.FileId.Identifier, file.file_id.size());
    }
    CloseHandle(handle);
}

HANDLE OpenDirectoryListing(const std::wstring& dir, WIN32_FIND_DATAW& data) {
    const std::wstring query = JoinPath(Win32Path(dir), L"*");
    HANDLE find = content_listing::First(query, data, FIND_FIRST_EX_LARGE_FETCH);
    if (find != INVALID_HANDLE_VALUE) return find;
    return content_listing::First(query, data, 0);
}

bool EnumerateCandidates(const ContentSearchRequest& request,
                         const std::atomic<bool>& cancelled,
                         std::vector<Candidate>& files, ContentSearchProgress& progress,
                         const ContentBatchCallback& callback) {
    const auto roots = EffectiveRoots(request);
    if (roots.empty()) {
        progress.error = ERROR_INVALID_PARAMETER;
        return false;
    }
    auto last_progress = std::chrono::steady_clock::now();
    uint64_t visited = 0;
    uint64_t last_published = 0;
    auto publish_progress = [&](bool force) {
        if (request.mode == ContentSearchMode::Duplicates)
            progress.scanned_files = visited;
        const auto now = std::chrono::steady_clock::now();
        if (!force && visited - last_published < 32 &&
            now - last_progress < std::chrono::milliseconds(100)) return true;
        last_progress = now;
        last_published = visited;
        return callback(progress, {});
    };
    auto record_failure = [&](DWORD error) {
        if (!progress.error) progress.error = error ? error : ERROR_GEN_FAILURE;
    };
    for (const auto& root : roots) {
        if (cancelled.load()) return false;
        progress.current_root = root;
        if (!publish_progress(true)) return false;
        std::deque<std::pair<std::wstring, int>> queue;
        queue.push_back({NormalizeWalkRoot(root), 0});
        while (!queue.empty() && !cancelled.load()) {
            const auto [dir, depth] = queue.front();
            queue.pop_front();
            WIN32_FIND_DATAW data{};
            HANDLE find = OpenDirectoryListing(dir, data);
            if (find == INVALID_HANDLE_VALUE) {
                const DWORD enumeration_error = GetLastError();
                if (!content_listing::EmptyDirectoryResult(Win32Path(dir), enumeration_error))
                    record_failure(enumeration_error);
                continue;
            }
            do {
                if (cancelled.load()) {
                    content_listing::Close(find);
                    return false;
                }
                if (IsDotOrDotDot(data.cFileName)) continue;
                const DWORD attributes = data.dwFileAttributes;
                const bool is_dir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (is_dir) {
                    if (request.skip_system_locations && depth == 0 &&
                        IsSkippedSystemDirectory(data.cFileName)) continue;
                    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    if (request.recursive)
                        queue.push_back({JoinPath(dir, data.cFileName), depth + 1});
                    continue;
                }
                if (request.skip_system_locations && depth == 0 &&
                    IsSkippedSystemFile(data.cFileName)) continue;
                ++visited;
                Candidate candidate;
                if (ReadCandidate(JoinPath(dir, data.cFileName), data,
                                  request.minimum_file_bytes, candidate))
                    files.push_back(std::move(candidate));
                if (!publish_progress(false)) {
                    content_listing::Close(find);
                    return false;
                }
            } while (content_listing::Next(find, data));
            const DWORD enumeration_error = GetLastError();
            if (enumeration_error != ERROR_NO_MORE_FILES) record_failure(enumeration_error);
            content_listing::Close(find);
            if (!publish_progress(false)) return false;
        }
    }
    if (request.mode == ContentSearchMode::Duplicates)
        progress.scanned_files = visited;
    if (!publish_progress(true)) return false;
    return !cancelled.load();
}

uint64_t SampleHash(const Candidate& file) {
    HANDLE handle = CreateFileW(Win32Path(file.path).c_str(), GENERIC_READ,
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
    HANDLE handle = CreateFileW(Win32Path(file.path).c_str(), GENERIC_READ,
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
                        std::vector<Candidate>& files,
                        ContentSearchProgress& progress,
                        const ContentBatchCallback& callback) {
    std::map<uint64_t, std::vector<Candidate*>> by_size;
    uint64_t hash_work = 0;
    for (auto& file : files) by_size[file.size].push_back(&file);
    for (const auto& [size, candidates] : by_size)
        if (candidates.size() >= 2) hash_work += candidates.size();
    progress.phase = ContentSearchPhase::Hashing;
    progress.total_files = progress.error ? 0 : hash_work;
    progress.scanned_files = 0;
    if (!callback(progress, {})) return false;
    auto last_progress = std::chrono::steady_clock::now();
    auto publish_progress = [&] {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_progress < std::chrono::milliseconds(250)) return true;
        last_progress = now;
        return callback(progress, {});
    };
    uint32_t group_id = 1;
    std::vector<ContentHit> batch;
    size_t total_hits = 0;
    for (auto& [size, candidates] : by_size) {
        if (cancelled.load()) return false;
        if (candidates.size() < 2) continue;
        std::map<uint64_t, std::vector<Candidate*>> by_sample;
        for (Candidate* candidate : candidates)
            by_sample[SampleHash(*candidate)].push_back(candidate);
        for (auto& [sample, sampled] : by_sample) {
            if (sampled.size() < 2 || sample == 0) {
                progress.scanned_files += sampled.size();
                if (!publish_progress()) return false;
                continue;
            }
            std::map<std::wstring, std::vector<Candidate*>> exact;
            for (Candidate* candidate : sampled) {
                std::array<uint8_t, 32> digest{};
                if (FullSha256(*candidate, cancelled, digest))
                    exact[DigestKey(digest)].push_back(candidate);
                progress.scanned_files++;
                progress.scanned_bytes += candidate->size;
                if (!publish_progress()) return false;
            }
            for (auto& [digest, matches] : exact) {
                if (matches.size() < 2) continue;
                std::set<std::wstring> identities;
                std::vector<Candidate*> unique;
                for (Candidate* match : matches) {
                    EnsureFileId(*match);
                    if (identities.insert(FileIdentity(*match)).second) unique.push_back(match);
                }
                if (unique.size() < 2) continue;
                for (const Candidate* match : unique) {
                    ContentHit hit;
                    hit.path = match->path;
                    hit.name = match->name;
                    hit.size = match->size;
                    hit.modified = match->modified;
                    hit.group = group_id;
                    batch.push_back(std::move(hit));
                    if (++total_hits >= request.maximum_hits && request.maximum_hits) progress.truncated = true;
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

bool MatchContentFilename(const std::wstring& path, uint64_t size, uint64_t modified, const CompiledQuery& q) {
    if (!q.path_prefix.empty()) {
        std::wstring prefix = q.path_prefix;
        std::replace(prefix.begin(), prefix.end(), L'/', L'\\');
        while (prefix.size() > 3 && prefix.back() == L'\\') prefix.pop_back();
        if (path.size() < prefix.size() || CompareStringOrdinal(path.data(), static_cast<int>(prefix.size()), prefix.data(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL ||
            (path.size() > prefix.size() && prefix.back() != L'\\' && path[prefix.size()] != L'\\')) return false;
    }
    if (q.groups.empty()) return true;
    const auto name = path.substr(path.find_last_of(L"\\/") + 1);
    for (const auto& group : q.groups) {
        bool match = true;
        for (const auto& term : group) {
            if (!MatchTerm(path, name, false, size, modified, term)) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}
size_t MatchCachedContent(std::wstring_view text, const ContentSearchRequest& request, const std::atomic<bool>* cancelled) {
    return MatchContent(text, request, cancelled);
}
ContentHit MakeCachedContentHit(const std::wstring& path, uint64_t size, uint64_t modified,
                               std::wstring_view content, size_t match) {
    Candidate file;
    file.path = path;
    file.name = FileName(path);
    file.size = size;
    file.modified = modified;
    return MakeContentHit(file, content, match);
}
bool RunContentSearch(const ContentSearchRequest& request, const std::atomic<bool>& cancelled,
                      ContentBatchCallback callback) {
    ContentSearchProgress progress;
    progress.generation = request.generation;
    progress.phase = ContentSearchPhase::Enumerating;
    const bool has_candidates = !request.candidate_paths.empty();
    if ((!has_candidates && EffectiveRoots(request).empty()) || !callback) {
        progress.done = true;
        progress.error = ERROR_INVALID_PARAMETER;
        if (callback) callback(progress, {});
        return false;
    }
    std::vector<Candidate> files;
    if (has_candidates) {
        if (!LoadCandidatePaths(request, files, progress, callback)) {
            progress.done = true;
            if (cancelled.load()) progress.error = ERROR_CANCELLED;
            callback(progress, {});
            return false;
        }
    } else if (!EnumerateCandidates(request, cancelled, files, progress, callback)) {
        progress.done = true;
        if (cancelled.load()) progress.error = ERROR_CANCELLED;
        callback(progress, {});
        return false;
    }
    // A failed subtree still leaves useful candidates. Finish processing them,
    // then publish one terminal partial result with the original enumeration error.
    const DWORD enumeration_error = progress.error;
    if (request.mode == ContentSearchMode::Duplicates) {
        const bool ok = RunDuplicateSearch(request, cancelled, files, progress, callback);
        progress.done = true;
        if (!ok && cancelled.load()) progress.error = ERROR_CANCELLED;
        callback(progress, {});
        return ok && enumeration_error == ERROR_SUCCESS;
    }

    DocumentReadSession document_session;
    progress.total_files = enumeration_error ? 0 : files.size();
    progress.scanned_files = 0;
    if (!callback(progress, {})) return false;
    if(request.sort!=ContentResultSort::Index) std::sort(files.begin(),files.end(),[&](const Candidate& a,const Candidate& b) {
        int cmp=0;
        if(request.sort==ContentResultSort::Size) cmp=a.size<b.size ? -1:a.size>b.size ? 1:0;
        else if(request.sort==ContentResultSort::Mtime) cmp=a.modified<b.modified ? -1:a.modified>b.modified ? 1:0;
        else {
            const auto key = [&](const Candidate& file) -> const wchar_t* {
                if (request.sort == ContentResultSort::Path) return file.path.c_str();
                if (request.sort == ContentResultSort::Type) {
                    const auto dot = file.name.find_last_of(L'.');
                    return dot == std::wstring::npos ? L"" : file.name.c_str() + dot + 1;
                }
                return file.name.c_str();
            };
            cmp = CompareStringOrdinal(key(a), -1, key(b), -1, TRUE) - CSTR_EQUAL;
        }
        if(!cmp) cmp=CompareStringOrdinal(a.path.c_str(),-1,b.path.c_str(),-1,TRUE)-CSTR_EQUAL;
        return request.sort_desc ? cmp>0:cmp<0;
    });
    std::vector<ContentHit> batch;
    size_t total_hits = 0;
    auto last_update=GetTickCount64();
    const auto filename_query = ParseQuery(request.filename_query);
    for (const auto& file : files) {
        if (cancelled.load()) break;
        if(GetTickCount64()-last_update>=100) {
            if(!callback(progress,std::move(batch))) return false;
            batch.clear();last_update=GetTickCount64();
        }
        ++progress.scanned_files;
        const auto dot = file.name.find_last_of(L'.');
        const auto extension = dot == std::wstring::npos ? std::wstring_view{} : std::wstring_view(file.name).substr(dot);
        const auto maximum = IsExtractedDocumentExtension(extension) ? request.maximum_document_bytes : request.maximum_file_bytes;
        if (file.size > maximum || !MatchContentFilename(file.path, file.size, file.modified, filename_query)) continue;
        std::wstring content;
        uint64_t bytes = 0;
        DWORD read_error = ERROR_SUCCESS;
        if (!ReadSearchableDocument(Win32Path(file.path), maximum, content, bytes,
                &read_error, text::Encoding::Auto, [&] { return cancelled.load(); })) {
            if (read_error == ERROR_CANCELLED) break;
            if (IsIndexedContentExtension(extension))
                progress.error = read_error;
            continue;
        }
        progress.scanned_bytes += bytes;
        const size_t match = MatchContent(content, request, &cancelled);
        if (match != std::wstring::npos) {
            batch.push_back(MakeContentHit(file, content, match));
            if (++total_hits >= request.maximum_hits && request.maximum_hits) progress.truncated = true;
        }
        if ((!batch.empty() && total_hits==1) || batch.size() >= 64) {
            if (!callback(progress, std::move(batch))) return false;
            batch.clear(); last_update=GetTickCount64();
        }
        if (progress.truncated) break;
    }
    if (!batch.empty() && !callback(progress, std::move(batch))) return false;
    progress.done = true;
    if (cancelled.load()) progress.error = ERROR_CANCELLED;
    callback(progress, {});
    return !cancelled.load() && enumeration_error == ERROR_SUCCESS;
}

} // namespace pulse::index
