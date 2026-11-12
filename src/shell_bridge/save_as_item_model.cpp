#include "save_as_item_model.h"
#include <limits>
#include <new>

namespace pulse::shell_bridge {
namespace {
using Microsoft::WRL::ComPtr;
constexpr size_t kMaxText = 32768;
struct Text { PWSTR value = nullptr; ~Text() { CoTaskMemFree(value); } };
HRESULT CopyText(const wchar_t* value, std::wstring& output) {
    if (!value) return E_UNEXPECTED;
    const size_t count = wcsnlen_s(value, kMaxText + 1);
    if (count > kMaxText) return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    output.assign(value, count); return S_OK;
}
HRESULT FilesystemPath(IShellItem* item, bool folder, std::wstring& output) {
    if (!item) return E_POINTER;
    SFGAOF attributes = 0;
    const HRESULT inspected = item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &attributes);
    if (FAILED(inspected)) return inspected;
    // S_FALSE is the documented normal result for a filesystem non-folder.
    if (!(attributes & SFGAO_FILESYSTEM) || bool(attributes & SFGAO_FOLDER) != folder) return E_NOTIMPL;
    Text path;
    const HRESULT named = item->GetDisplayName(SIGDN_FILESYSPATH, &path.value);
    if (named != S_OK) return FAILED(named) ? named : E_NOTIMPL;
    const HRESULT copied = CopyText(path.value, output);
    return FAILED(copied) ? copied : output.empty() ? E_NOTIMPL : S_OK;
}
HRESULT ReadInitial(IFileDialog* native, SaveAsItemSnapshot& candidate) {
    if (!native) return E_POINTER;
    ComPtr<IShellItem> folder;
    const HRESULT located = native->GetFolder(&folder);
    if (located != S_OK) return FAILED(located) ? located : E_NOTIMPL;
    std::wstring path;
    HRESULT hr = FilesystemPath(folder.Get(), true, path);
    if (FAILED(hr)) return hr;
    Text filename;
    const HRESULT named = native->GetFileName(&filename.value);
    std::wstring text;
    if (named == S_OK) {
        hr = CopyText(filename.value, text); if (FAILED(hr)) return hr;
    } else if (!(candidate.cleared && named == E_FAIL && !filename.value)) {
        return FAILED(named) ? named : E_NOTIMPL;
    }
    candidate.initial_folder = std::move(folder);
    candidate.initial_directory = std::move(path);
    candidate.initial_filename = std::move(text);
    candidate.filename_result = named;
    candidate.filename_set = named == S_OK;
    return S_OK;
}
struct Capturing { bool& value; ~Capturing() { value = false; } };
}
HRESULT SaveAsItemModel::Begin(HRESULT native_result, uint64_t& epoch) noexcept {
    if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
    ++epoch_;
    if (capturing_) { capture_result_ = HRESULT_FROM_WIN32(ERROR_BUSY); return capture_result_; }
    capture_result_ = native_result == S_OK ? E_PENDING : FAILED(native_result) ? native_result : E_NOTIMPL;
    if (native_result != S_OK) return capture_result_;
    if (snapshot_.generation == (std::numeric_limits<uint64_t>::max)()) {
        capture_result_ = HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW); return capture_result_;
    }
    epoch = epoch_; capturing_ = true; return S_OK;
}
HRESULT SaveAsItemModel::Commit(SaveAsItemSnapshot&& candidate, uint64_t epoch) noexcept {
    if (epoch != epoch_) { capture_result_ = HRESULT_FROM_WIN32(ERROR_RETRY); return capture_result_; }
    const uint64_t previous_generation = snapshot_.generation;
    candidate.generation = previous_generation + 1;
    snapshot_ = std::move(candidate);
    // Releasing the previous caller-supplied Shell identity can reenter too.
    if (epoch != epoch_) {
        snapshot_.generation = previous_generation;
        capture_result_ = HRESULT_FROM_WIN32(ERROR_RETRY); return capture_result_;
    }
    has_call_ = true; capture_result_ = S_OK;
    return S_OK;
}
HRESULT SaveAsItemModel::ObserveSetSaveAsItem(IShellItem* item, IFileDialog* native, HRESULT native_result) noexcept {
    uint64_t epoch = 0;
    const HRESULT begun = Begin(native_result, epoch); if (begun != S_OK) return begun;
    Capturing capture{capturing_};
    try {
        SaveAsItemSnapshot candidate;
        candidate.cleared = item == nullptr;
        if (item) {
            candidate.original_item = item;
            HRESULT hr = FilesystemPath(item, false, candidate.source_path);
            if (FAILED(hr)) { capture_result_ = hr; return hr; }
            hr = item->GetParent(&candidate.source_parent);
            if (hr != S_OK) { capture_result_ = FAILED(hr) ? hr : E_NOTIMPL; return capture_result_; }
            hr = FilesystemPath(candidate.source_parent.Get(), true, candidate.source_parent_path);
            if (FAILED(hr)) { capture_result_ = hr; return hr; }
            const auto split = candidate.source_path.find_last_of(L"\\/");
            if (split == std::wstring::npos || split + 1 == candidate.source_path.size()) { capture_result_ = E_NOTIMPL; return capture_result_; }
            candidate.source_name = candidate.source_path.substr(split + 1);
        }
        const HRESULT observed = ReadInitial(native, candidate);
        if (FAILED(observed)) { capture_result_ = observed; return observed; }
        return Commit(std::move(candidate), epoch);
    } catch (const std::bad_alloc&) { capture_result_ = E_OUTOFMEMORY; }
    catch (...) { capture_result_ = E_FAIL; }
    return capture_result_;
}
HRESULT SaveAsItemModel::RefreshAfterSetter(IFileDialog* native, HRESULT native_result) noexcept {
    if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
    if (!has_call_ && native_result == S_OK) return S_OK;
    uint64_t epoch = 0;
    const HRESULT begun = Begin(native_result, epoch); if (begun != S_OK) return begun;
    Capturing capture{capturing_};
    try {
        SaveAsItemSnapshot candidate = snapshot_;
        const HRESULT observed = ReadInitial(native, candidate);
        if (FAILED(observed)) { capture_result_ = observed; return observed; }
        return Commit(std::move(candidate), epoch);
    } catch (const std::bad_alloc&) { capture_result_ = E_OUTOFMEMORY; }
    catch (...) { capture_result_ = E_FAIL; }
    return capture_result_;
}
HRESULT SaveAsItemModel::Snapshot(SaveAsItemSnapshot* output) const noexcept {
    if (!output) return E_POINTER;
    if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
    if (capturing_) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if (capture_result_ != S_OK) return capture_result_;
    try { SaveAsItemSnapshot candidate = snapshot_; *output = std::move(candidate); return S_OK; }
    catch (...) { return E_OUTOFMEMORY; }
}
HRESULT SaveAsItemModel::BuildFilesystemSeed(bool property_contract_requested, SaveAsItemSeed* output,
    IPropertyStore* owned_metadata_snapshot) const noexcept {
    if (!output) return E_POINTER;
    if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
    if (capturing_) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if (capture_result_ != S_OK) return capture_result_;
    if (property_contract_requested && !owned_metadata_snapshot) return E_NOTIMPL;
    try {
        SaveAsItemSeed candidate;
        candidate.original_item = snapshot_.original_item;
        if (property_contract_requested) candidate.metadata_snapshot = owned_metadata_snapshot;
        candidate.initial_folder = snapshot_.initial_folder;
        candidate.directory = snapshot_.initial_directory;
        candidate.filename = snapshot_.initial_filename;
        candidate.cleared = snapshot_.cleared; candidate.filename_set = snapshot_.filename_set;
        *output = std::move(candidate); return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}
HRESULT SaveAsItemModel::Reset() noexcept {
    if (GetCurrentThreadId() != thread_) return RPC_E_WRONG_THREAD;
    ++epoch_;
    if (capturing_) { capture_result_ = HRESULT_FROM_WIN32(ERROR_BUSY); return capture_result_; }
    const auto epoch = epoch_;
    capturing_ = true; Capturing capture{capturing_};
    snapshot_ = {}; has_call_ = false; capture_result_ = E_PENDING;
    if (epoch != epoch_) { capture_result_ = HRESULT_FROM_WIN32(ERROR_RETRY); return capture_result_; }
    return S_OK;
}
}
