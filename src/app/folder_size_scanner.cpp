#include "folder_size_scanner.h"
#include "../fs/cloud_state.h"
#include <cwchar>
#include <utility>

namespace pulse::app::folder_size {
namespace {
constexpr size_t kBufferBytes = 64 * 1024;
std::wstring ChildPath(const std::wstring& parent, const wchar_t* name, size_t length) {
    std::wstring child;
    child.reserve(parent.size() + 1 + length);
    child = parent;
    if (child.back() != L'\\') child += L'\\';
    child.append(name, length);
    return child;
}
bool AddBytes(uint64_t& total, uint64_t bytes) {
    if (bytes > UINT64_MAX - total) return false;
    total += bytes;
    return true;
}
bool Dots(const wchar_t* name, size_t length) {
    return name[0] == L'.' && (length == 1 || (length == 2 && name[1] == L'.'));
}
uint32_t ErrorIssue(DWORD error) { return error == ERROR_ACCESS_DENIED ? SizeAccessDenied : SizeIoError; }
// Same directory object (volume + file id): no link was swapped in between two opens.
bool SameObject(HANDLE a, HANDLE b) {
    BY_HANDLE_FILE_INFORMATION x{}, y{};
    return GetFileInformationByHandle(a, &x) && GetFileInformationByHandle(b, &y) &&
        x.dwVolumeSerialNumber == y.dwVolumeSerialNumber &&
        x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
}
}
Descend ClassifyDirectory(DWORD attrs, DWORD reparse_tag, bool manual) {
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) && (reparse_tag == 0 || IsReparseTagNameSurrogate(reparse_tag)))
        return Descend::Link;
    if (!manual && (attrs & (FILE_ATTRIBUTE_OFFLINE | ::pulse::fs::kAttrRecallOnOpen))) return Descend::Offline;
    return Descend::Yes;
}
Scan::Scan(std::wstring input, uint64_t input_revision, uint64_t input_request_epoch, bool manual)
    : path(std::move(input)), revision(input_revision), request_epoch(input_request_epoch), manual_(manual) {
    stack_.push_back(Frame{path});
}
Scan::~Scan() {
    for (auto& frame : stack_) {
        if (frame.dir != INVALID_HANDLE_VALUE) CloseHandle(frame.dir);
        if (frame.find != INVALID_HANDLE_VALUE) FindClose(frame.find);
    }
}
FolderSizeValue Scan::Progress() const {
    if (done) return result;
    FolderSizeValue value{FolderSizeState::Calculating};
    value.source = FolderSizeSource::Scan;
    value.has_value = entries_scanned != 0;
    value.partial = true;
    // An unfinished child has not yet contributed to its parent's bytes.
    // Summing the open frames therefore counts every visited file once.
    for (const auto& frame : stack_) {
        if (!AddBytes(value.bytes, frame.bytes)) { value.bytes = UINT64_MAX; break; }
    }
    return value;
}
void Scan::Skip(Frame& frame, uint32_t issue) {
    frame.partial = true; ++frame.skipped; frame.issues |= issue;
}
void Scan::FinishFrame() {
    auto frame = std::move(stack_.back()); stack_.pop_back();
    if (frame.dir != INVALID_HANDLE_VALUE) CloseHandle(frame.dir);
    if (frame.find != INVALID_HANDLE_VALUE) FindClose(frame.find);
    FolderSizeValue value;
    value.bytes = frame.bytes;
    value.has_value = frame.read_any;
    value.partial = frame.partial && frame.read_any;
    value.source = FolderSizeSource::Scan;
    value.skipped = frame.skipped; value.issues = frame.issues;
    value.state = !frame.read_any ? FolderSizeState::Unavailable :
        frame.partial ? FolderSizeState::Partial : FolderSizeState::Ready;
    // Unmonitored children cannot be reused; only keep the root snapshot.
    // Watched subtrees stay staged until the job's revision is validated.
    if (stack_.empty() || frame.watch_generation != 0) {
        if (completed.size() == 1024) completed.pop_front();
        completed.push_back({std::move(frame.path), value, frame.watch_generation});
    }
    if (stack_.empty()) { result = value; done = true; }
    else {
        auto& parent = stack_.back();
        const bool added = AddBytes(parent.bytes, frame.bytes);
        parent.partial |= frame.partial || !added;
        parent.skipped += frame.skipped; parent.issues |= frame.issues;
    }
}
// Opens the directory itself (never a link target) and checks what was opened,
// so a folder replaced by a junction since its parent was listed is not followed.
bool Scan::Open(Frame& frame) {
    constexpr DWORD kAccess = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE dir = CreateFileW(frame.path.c_str(), kAccess, kShare, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (dir == INVALID_HANDLE_VALUE) { Skip(frame, ErrorIssue(GetLastError())); return false; }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(dir, FileAttributeTagInfo, &tag, sizeof(tag))) {
        const DWORD error = GetLastError();
        CloseHandle(dir); Skip(frame, ErrorIssue(error)); return false;
    }
    if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { CloseHandle(dir); Skip(frame, SizeIoError); return false; }
    switch (ClassifyDirectory(tag.FileAttributes, tag.ReparseTag, manual_)) {
    case Descend::Link: CloseHandle(dir); Skip(frame, SizeLink); return false;
    case Descend::Offline: CloseHandle(dir); Skip(frame, SizeOffline); return false;
    case Descend::Yes: break;
    }
    // A cloud/sync folder (non-surrogate reparse point) is listed through a
    // normal open, as the shell does: its provider merges the listing there.
    // Automatic walks reach this only when the listing is already on this
    // device (Offline/recall-on-open folders were skipped above).
    if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        const HANDLE provider = CreateFileW(frame.path.c_str(), kAccess, kShare, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (provider != INVALID_HANDLE_VALUE) {
            if (SameObject(dir, provider)) { CloseHandle(dir); dir = provider; }
            else CloseHandle(provider);
        }
    }
    frame.dir = dir;
    return true;
}
void Scan::AddEntry(Frame& frame, const wchar_t* name, size_t length, DWORD attrs, DWORD tag, uint64_t bytes) {
    ++entries_scanned;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        // No artificial depth cap: Win32 path limits and I/O errors apply.
        switch (ClassifyDirectory(attrs, tag, manual_)) {
        case Descend::Link: Skip(frame, SizeLink); return;
        case Descend::Offline: Skip(frame, SizeOffline); return;
        case Descend::Yes: frame.children.emplace_back(name, length); return;
        }
    }
    // Logical length; cloud placeholders report it without being downloaded.
    frame.partial |= !AddBytes(frame.bytes, bytes);
}
// One 64 KiB batch of FILE_FULL_DIR_INFO records. Returns the entries read.
unsigned Scan::ReadListing(Frame& frame) {
    if (frame.find != INVALID_HANDLE_VALUE) return ReadFindListing(frame);
    auto* buffer = reinterpret_cast<uint8_t*>(buffer_.get());
    if (!GetFileInformationByHandleEx(frame.dir, FileFullDirectoryInfo, buffer, static_cast<DWORD>(kBufferBytes))) {
        const DWORD error = GetLastError();
        if (error == ERROR_NO_MORE_FILES || (!frame.queried && error == ERROR_FILE_NOT_FOUND)) {
            frame.read_any = true;  // end of the listing (or an empty root without dot entries)
        } else if (!frame.queried && (error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_FUNCTION ||
                                      error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_LEVEL)) {
            // A file system without this information class: list it the classic way.
            frame.find = FindFirstFileExW(ChildPath(frame.path, L"*", 1).c_str(), FindExInfoBasic,
                &frame.data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            CloseHandle(frame.dir); frame.dir = INVALID_HANDLE_VALUE;
            if (frame.find != INVALID_HANDLE_VALUE) {
                frame.queried = true; frame.read_any = true;
                const auto& d = frame.data;
                const size_t length = wcslen(d.cFileName);
                if (!Dots(d.cFileName, length))
                    AddEntry(frame, d.cFileName, length, d.dwFileAttributes,
                             (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? d.dwReserved0 : 0,
                             (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow);
                return 1;
            }
            const DWORD find_error = GetLastError();
            frame.read_any = find_error == ERROR_FILE_NOT_FOUND || find_error == ERROR_NO_MORE_FILES;
            if (!frame.read_any) Skip(frame, ErrorIssue(find_error));
            frame.listed = true;
            return 0;
        } else {
            // Entries already read stay counted; the rest of this folder is missing.
            frame.read_any |= frame.queried;
            Skip(frame, ErrorIssue(error));
        }
        CloseHandle(frame.dir); frame.dir = INVALID_HANDLE_VALUE;
        frame.listed = true;
        return 0;
    }
    frame.queried = true; frame.read_any = true;
    unsigned count = 0;
    for (size_t offset = 0;;) {
        const auto* info = reinterpret_cast<const FILE_FULL_DIR_INFO*>(buffer + offset);
        const size_t length = info->FileNameLength / sizeof(wchar_t);
        if (length && !Dots(info->FileName, length)) {
            // With FILE_ATTRIBUTE_REPARSE_POINT, EaSize carries the reparse tag (MS-FSCC 2.4.17).
            AddEntry(frame, info->FileName, length, info->FileAttributes,
                     (info->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? info->EaSize : 0,
                     static_cast<uint64_t>(info->EndOfFile.QuadPart));
            ++count;
        }
        if (!info->NextEntryOffset) break;
        offset += info->NextEntryOffset;
        if (offset >= kBufferBytes) break;
    }
    return count;
}
unsigned Scan::ReadFindListing(Frame& frame) {
    unsigned count = 0;
    while (count < 512) {
        if (!FindNextFileW(frame.find, &frame.data)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_FILES) Skip(frame, ErrorIssue(error));
            FindClose(frame.find); frame.find = INVALID_HANDLE_VALUE;
            frame.listed = true;
            break;
        }
        const auto& d = frame.data;
        const size_t length = wcslen(d.cFileName);
        if (Dots(d.cFileName, length)) continue;
        AddEntry(frame, d.cFileName, length, d.dwFileAttributes,
                 (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? d.dwReserved0 : 0,
                 (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow);
        ++count;
    }
    return count;
}
void Scan::Step(const std::atomic<bool>& stopping, const Lookup& lookup, const Coverage& coverage) {
    if (!buffer_) buffer_ = std::make_unique<uint64_t[]>(kBufferBytes / sizeof(uint64_t));
    const auto deadline = GetTickCount64() + 12;
    const unsigned budget = manual_ ? 8192u : 1024u;
    unsigned processed = 0;
    while (!done && !stopping && GetTickCount64() < deadline && processed < budget) {
        auto& frame = stack_.back();
        if (!frame.started) {
            frame.started = true;
            frame.watch_generation = coverage(frame.path);
            ++processed;
            if (!Open(frame)) FinishFrame();
            continue;
        }
        if (!frame.listed) {
            if (frame.dir == INVALID_HANDLE_VALUE && frame.find == INVALID_HANDLE_VALUE) { frame.listed = true; continue; }
            processed += 1 + ReadListing(frame);
            continue;
        }
        if (frame.next_child < frame.children.size()) {
            auto& name = frame.children[frame.next_child++];
            auto child = ChildPath(frame.path, name.c_str(), name.size());
            std::wstring().swap(name);
            ++processed;
            std::optional<uint64_t> cached;
            if (frame.watch_generation != 0) cached = lookup(child);
            if (cached) {
                frame.partial |= !AddBytes(frame.bytes, *cached);
                ++subtree_hits;
            } else stack_.push_back(Frame{std::move(child)}); // invalidates `frame`
            continue;
        }
        FinishFrame();
    }
}
} // namespace pulse::app::folder_size
