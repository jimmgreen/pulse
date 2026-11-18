#include "tag_ads_sync.h"
#include "session.h"
#include "../common/utf8_file.h"
#include "../fs/fs_enum.h"
#include "../common/localization.h"
#include <map>
#include <mutex>
#include <cwctype>
#include <set>
#include <memory>
#include <shlwapi.h>
namespace pulse::app {
namespace {
std::mutex mutex;
std::wstring loaded_dir;
std::map<std::wstring, TagAdsUpdate> pending;
bool readable = false;
DWORD load_error = ERROR_SUCCESS;
std::wstring Key(const std::wstring& path) {
    auto key = fs::NormalizePath(path);
    for (auto& ch : key) ch = static_cast<wchar_t>(towlower(ch));
    return key;
}
bool Number(std::wstring_view text, size_t& position, size_t& value) {
    value = 0; const size_t begin = position;
    while (position < text.size() && text[position] >= L'0' && text[position] <= L'9') {
        if (value > 16000000) return false;
        value = value * 10 + static_cast<size_t>(text[position++] - L'0');
    }
    return position > begin && position < text.size() && text[position++] == L':';
}
bool Field(std::wstring_view text, size_t& position, std::wstring& value) {
    size_t size = 0;
    if (!Number(text, position, size) || size > text.size() - position) return false;
    value.assign(text.substr(position, size)); position += size; return true;
}
void Load() {
    const auto dir = GetPulseDataDir();
    if (loaded_dir == dir && !loaded_dir.empty()) return;
    loaded_dir = dir; pending.clear(); readable = false; load_error = ERROR_INVALID_DATA;
    if (dir.empty()) return;
    const auto file = dir + L"\\tag_ads_pending.dat";
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { readable = true; return; }
    }
    std::wstring text;
    constexpr std::wstring_view prefix = L"PULSE_ADS_PENDING_1\n";
    SetLastError(ERROR_SUCCESS);
    if (!ReadUtf8File(file,text)) { load_error = GetLastError(); return; }
    if (!text.starts_with(prefix)) return;
    size_t pos = prefix.size();
    while (pos < text.size()) {
        TagAdsUpdate update; size_t count = 0;
        if (!Field(text,pos,update.path) || update.path.empty() || !Number(text,pos,count) || count > 100000) return;
        for (size_t i=0; i<count; ++i) {
            TagAdsRecord tag; size_t rgb = 0;
            if (!Field(text,pos,tag.id) || !Field(text,pos,tag.name) || !Number(text,pos,rgb) || rgb > 0xFFFFFF) return;
            tag.rgb = static_cast<uint32_t>(rgb); update.tags.push_back(std::move(tag));
        }
        pending[Key(update.path)] = std::move(update);
    }
    readable = true;
}
bool Save(const std::map<std::wstring, TagAdsUpdate>& records) {
    std::wstring text = L"PULSE_ADS_PENDING_1\n";
    auto field = [&](const std::wstring& value) { text += std::to_wstring(value.size()) + L":" + value; };
    for (const auto& [key,update] : records) {
        field(update.path); text += std::to_wstring(update.tags.size()) + L":";
        for (const auto& tag : update.tags) {
            field(tag.id); field(tag.name); text += std::to_wstring(tag.rgb & 0xFFFFFFu) + L":";
        }
    }
    return WriteUtf8FileAtomic(loaded_dir + L"\\tag_ads_pending.dat",text);
}
bool MissingError(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}
bool MissingOnAccessibleVolume(const std::wstring& path, DWORD error) {
    if (!MissingError(error)) return false;
    auto parent = fs::NormalizePath(path);
    while (!PathIsRootW(parent.c_str())) {
        const auto next = fs::ParentPath(parent);
        if (next == parent) return false;
        parent = next;
        const auto attributes = GetFileAttributesW(parent.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
            return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!MissingError(GetLastError())) return false;
    }
    return false;
}
}
bool HasPendingTagAds(const std::wstring& path) {
    std::lock_guard lock(mutex); Load();
    return !readable || pending.contains(Key(path));
}
std::wstring TagAdsFailureMessage(const TagAdsFailure& failure) {
    if (failure.unsupported) {
        auto message = l10n::Get(l10n::StringId::TagMetadataUnsupported);
        const auto marker = message.find(L"{path}");
        if (marker != std::wstring::npos) message.replace(marker, 6, failure.path);
        return message;
    }
    const wchar_t* stage = failure.stage == TagAdsFailureStage::JournalRead ? L"journal-read" :
        failure.stage == TagAdsFailureStage::JournalWrite ? L"journal-write" :
        failure.stage == TagAdsFailureStage::Pending ? L"pending" : L"stream-write";
    return std::wstring(l10n::Get(l10n::StringId::FileTagMetadataSyncIsPendingRetry).c_str()) +
        L" " + failure.path + L" (" + stage + L", " + std::to_wstring(failure.error) + L")";
}
void ApplyTagAdsNotice(TagAdsNoticeState& owned, const std::vector<TagAdsFailure>& failures,
                       std::wstring& title, std::wstring& message, bool show_new,
                       const std::wstring& current_path) {
    const bool owns_banner = !owned.message.empty() && title == owned.title && message == owned.message;
    owned = {};
    const auto current = Key(current_path);
    auto directory = current;
    if (!directory.empty() && directory.back() != L'\\') directory += L'\\';
    const TagAdsFailure* shown = nullptr;
    for (const auto& failure : failures) {
        // Journal errors affect all tag edits; stream errors belong to the folder
        // containing the file, not whichever folder happens to be active now.
        if (!current_path.empty() && failure.stage != TagAdsFailureStage::JournalRead &&
            failure.stage != TagAdsFailureStage::JournalWrite) {
            const auto path = Key(failure.path);
            if (directory.empty() || (path != current && !path.starts_with(directory))) continue;
        }
        if (!shown || failure.stage != TagAdsFailureStage::Pending) shown = &failure;
    }
    if (!shown) {
        if (owns_banner) { title.clear(); message.clear(); }
        return;
    }
    // A later unrelated notification owns its banner; a sync retry cannot erase it.
    if (!owns_banner && (!show_new || !title.empty() || !message.empty())) return;
    owned.title = l10n::Get(l10n::StringId::FileTagSync).c_str();
    owned.message = TagAdsFailureMessage(*shown);
    title = owned.title; message = owned.message;
}
static std::vector<std::wstring> SyncBatch(const std::vector<TagAdsUpdate>& updates,
    std::vector<TagAdsFailure>* diagnostics, std::map<std::wstring, TagAdsFailure>* attempted) {
    std::unique_lock lock(mutex); Load();
    std::vector<std::wstring> failed;
    if (diagnostics) diagnostics->clear();
    auto report = [&](const std::wstring& path, TagAdsFailureStage stage, DWORD error) {
        failed.push_back(path);
        if (!diagnostics) return;
        TagAdsFailure failure{path, stage, error ? error : ERROR_GEN_FAILURE};
        if (stage == TagAdsFailureStage::StreamWrite) {
            wchar_t root[32768]{};
            DWORD flags = 0;
            if (GetVolumePathNameW(path.c_str(), root, ARRAYSIZE(root)) &&
                GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0))
                failure.unsupported = (flags & FILE_NAMED_STREAMS) == 0;
        }
        diagnostics->push_back(std::move(failure));
    };
    if (!readable) {
        for (const auto& update : updates) report(update.path, TagAdsFailureStage::JournalRead, load_error);
        if (failed.empty()) report(loaded_dir, TagAdsFailureStage::JournalRead, load_error);
        return failed;
    }
    auto staged = pending;
    for (const auto& update : updates) staged[Key(update.path)] = update;
    // Publish intent before releasing the short metadata lock.
    pending = staged;
    lock.unlock();
    if (staged.empty()) return failed;
    // Also persist retries: an earlier journal write may have failed while the
    // intended values were retained only in memory.
    if (!Save(staged)) {
        const DWORD error = GetLastError();
        for (const auto& [key,update] : staged) report(update.path, TagAdsFailureStage::JournalWrite, error);
        return failed;
    }
    size_t attempted_count = 0;
    for (auto it = staged.begin(); it != staged.end() && attempted_count < 64;) {
        if (attempted && attempted->contains(it->first)) { ++it; continue; }
        ++attempted_count;
        // CREATE_ALWAYS on an ADS can recreate its missing host file. Retire
        // stale paths only after finding an accessible ancestor; offline shares
        // and inaccessible volumes must retain their durable retry intent.
        const HANDLE host = CreateFileW(it->second.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (host == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (MissingOnAccessibleVolume(it->second.path, error)) {
                it = staged.erase(it);
                continue;
            }
            report(it->second.path, TagAdsFailureStage::StreamWrite, error);
            if (attempted && diagnostics) (*attempted)[it->first] = diagnostics->back();
            ++it;
            continue;
        }
        // Keep the host alive until the stream write finishes, even if a move
        // or delete races this retry after the existence check.
        const std::unique_ptr<void, decltype(&CloseHandle)> keep_host(host, CloseHandle);
        if (WriteTagAdsV2(it->second.path,it->second.tags)) it = staged.erase(it);
        else {
            const DWORD error = GetLastError();
            report(it->second.path, TagAdsFailureStage::StreamWrite, error);
            if (attempted && diagnostics) (*attempted)[it->first] = diagnostics->back();
            ++it;
        }
    }
    const bool saved = Save(staged);
    const DWORD save_error = GetLastError();
    lock.lock();
    if (saved) pending = std::move(staged);
    if (saved) {
        // The result is the complete pending snapshot, including work deferred
        // by the per-batch limit. An empty result is permission to clear the UI.
        const std::set<std::wstring> reported(failed.begin(), failed.end());
        for (const auto& [key, update] : pending) {
            if (!reported.contains(update.path)) {
                if (attempted && attempted->contains(key)) {
                    failed.push_back(update.path);
                    if (diagnostics) diagnostics->push_back(attempted->at(key));
                } else report(update.path, TagAdsFailureStage::Pending, ERROR_IO_PENDING);
            }
        }
    }
    if (!saved) {
        failed.clear();
        if (diagnostics) diagnostics->clear();
        for (const auto& [key,update] : pending) report(update.path, TagAdsFailureStage::JournalWrite, save_error);
    }
    return failed;
}
std::vector<std::wstring> SyncTagAdsUpdates(const std::vector<TagAdsUpdate>& updates,
                                         std::vector<TagAdsFailure>* diagnostics) {
    return SyncBatch(updates, diagnostics, nullptr);
}
namespace {
struct DrainState {
    std::map<std::wstring, TagAdsFailure> attempted;
    std::function<void(std::function<void()>)> enqueue;
    std::function<void(std::vector<TagAdsFailure>)> publish;
};
void ScheduleDrain(const std::shared_ptr<DrainState>& state, std::vector<TagAdsUpdate> updates) {
    state->enqueue([state, updates = std::move(updates)] {
        std::vector<TagAdsFailure> failures;
        SyncBatch(updates, &failures, &state->attempted);
        bool more = false;
        for (const auto& failure : failures)
            more |= failure.stage == TagAdsFailureStage::Pending;
        state->publish(std::move(failures));
        if (more) ScheduleDrain(state, {});
    });
}
}
void QueueTagAdsDrain(std::vector<TagAdsUpdate> updates,
    std::function<void(std::function<void()>)> enqueue,
    std::function<void(std::vector<TagAdsFailure>)> publish) {
    auto state = std::make_shared<DrainState>();
    state->enqueue = std::move(enqueue); state->publish = std::move(publish);
    ScheduleDrain(state, std::move(updates));
}
}
