#include "virtual_file_drop.h"

#include "../common/path_utils.h"
#include "../common/runtime_log.h"

#include <shldisp.h>
#include <shlguid.h>
#include <wrl/client.h>
#include <process.h>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace pulse::ops {
namespace {

using Microsoft::WRL::ComPtr;
constexpr DWORD kContentsMedia = TYMED_ISTREAM | TYMED_HGLOBAL;
constexpr std::array<DWORD, 2> kMediaByPreference{TYMED_ISTREAM, TYMED_HGLOBAL};
constexpr DWORD kApartmentStartWaitMs = 500;

enum class Stage : uint32_t {
    Allocate = 1, StartThread, Initialize, Marshal, SourceStart, Unmarshal,
    Formats, BindItem, CheckFolder, BindTarget, DragEnter, Drop, SourceEnd,
    ReleaseMarshal,
};

void Report(Stage stage, HRESULT hr, size_t files, DWORD effect = DROPEFFECT_NONE) noexcept {
    using namespace diagnostics::runtime;
    Event("virtual_file_drop", {{"stage", static_cast<uint32_t>(stage)},
        {"hresult", static_cast<uint32_t>(hr)}, {"files", files}, {"effect", effect}},
        FAILED(hr) || hr != S_OK ? Level::Warning : Level::Info);
}

class SnapshotDataObject final : public IDataObject {
public:
    SnapshotDataObject(ComPtr<IDataObject> source, std::vector<FILEDESCRIPTORW> files,
                       CLIPFORMAT descriptor, CLIPFORMAT contents) noexcept
        : source_(std::move(source)), files_(std::move(files)),
          descriptor_(descriptor), contents_(contents) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IDataObject) return E_NOINTERFACE;
        *out = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = InterlockedDecrement(&references_);
        if (!left) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        const HRESULT valid = Validate(format);
        if (FAILED(valid) || format->cfFormat == descriptor_) return valid;
        HRESULT result = DV_E_TYMED;
        for (const DWORD media : kMediaByPreference) {
            if (!(format->tymed & media)) continue;
            auto request = Contents(format->lindex, media);
            result = source_->QueryGetData(&request);
            if (result == S_OK) return result;
        }
        return FAILED(result) ? result : DV_E_TYMED;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* out) override {
        if (!out) return E_POINTER;
        *out = {};
        HRESULT result = Validate(format);
        if (FAILED(result)) return result;
        if (format->cfFormat == descriptor_) return CopyDescriptors(*out);

        for (const DWORD media : kMediaByPreference) {
            if (!(format->tymed & media)) continue;
            auto request = Contents(format->lindex, media);
            STGMEDIUM offered{};
            result = source_->GetData(&request, &offered);
            if (SUCCEEDED(result)) {
                const bool supported = (offered.tymed == TYMED_ISTREAM && offered.pstm) ||
                                       (offered.tymed == TYMED_HGLOBAL && offered.hGlobal);
                if (supported && (offered.tymed & format->tymed)) {
                    *out = offered;
                    return S_OK;
                }
                ReleaseStgMedium(&offered);
                result = DV_E_TYMED;
            }
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (!out) return E_POINTER;
        out->ptd = nullptr;
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM* medium, BOOL release) override {
        if (!medium) return E_POINTER;
        // Shell can set its performed-effect bookkeeping. It must not reach
        // the original source or grant it permission to delete anything.
        if (release) ReleaseStgMedium(medium);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        try {
            std::vector<FORMATETC> formats;
            formats.reserve(files_.size() + 1);
            formats.push_back({descriptor_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
            for (size_t i = 0; i < files_.size(); ++i)
                formats.push_back(Contents(static_cast<LONG>(i), kContentsMedia));
            return SHCreateStdEnumFmtEtc(static_cast<UINT>(formats.size()), formats.data(), out);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    HRESULT Validate(const FORMATETC* format) const noexcept {
        if (!format) return E_POINTER;
        if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        if (format->ptd) return DV_E_DVTARGETDEVICE;
        if (format->cfFormat == descriptor_) {
            if (format->lindex != -1) return DV_E_LINDEX;
            return format->tymed & TYMED_HGLOBAL ? S_OK : DV_E_TYMED;
        }
        if (format->cfFormat != contents_) return DV_E_FORMATETC;
        if (format->lindex < 0 || static_cast<size_t>(format->lindex) >= files_.size()) return DV_E_LINDEX;
        return format->tymed & kContentsMedia ? S_OK : DV_E_TYMED;
    }

    FORMATETC Contents(LONG index, DWORD media) const noexcept {
        return {contents_, nullptr, DVASPECT_CONTENT, index, media};
    }

    HRESULT CopyDescriptors(STGMEDIUM& out) const noexcept {
        constexpr size_t header = offsetof(FILEGROUPDESCRIPTORW, fgd);
        const size_t bytes = header + files_.size() * sizeof(FILEDESCRIPTORW);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!memory) return E_OUTOFMEMORY;
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(memory));
        if (!group) { GlobalFree(memory); return E_OUTOFMEMORY; }
        group->cItems = static_cast<UINT>(files_.size());
        auto* descriptors = reinterpret_cast<FILEDESCRIPTORW*>(reinterpret_cast<BYTE*>(group) + header);
        for (size_t i = 0; i < files_.size(); ++i) {
            const auto& file = files_[i];
            auto& snapshot = descriptors[i];
            snapshot.dwFlags = FD_ATTRIBUTES | static_cast<DWORD>(FD_UNICODE) | (file.dwFlags & FD_FILESIZE);
            snapshot.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            snapshot.nFileSizeLow = file.nFileSizeLow;
            snapshot.nFileSizeHigh = file.nFileSizeHigh;
            wmemcpy(snapshot.cFileName, file.cFileName, ARRAYSIZE(file.cFileName));
        }
        GlobalUnlock(memory);
        out.tymed = TYMED_HGLOBAL;
        out.hGlobal = memory;
        return S_OK;
    }

    LONG references_ = 1;
    ComPtr<IDataObject> source_;
    const std::vector<FILEDESCRIPTORW> files_;
    const CLIPFORMAT descriptor_, contents_;
};

struct CloseHandleDeleter {
    void operator()(void* value) const noexcept { CloseHandle(value); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

struct DropJob {
    std::wstring destination;
    std::vector<FILEDESCRIPTORW> files;
    Handle ready, submitted;
    HRESULT initialize = E_UNEXPECTED;
    IStream* packet = nullptr;  // The only COM pointer transported between apartments.
    bool source_async_started = false;
};

struct DropResult {
    Stage stage;
    HRESULT hr;
    DWORD effect = DROPEFFECT_NONE;
};

DropResult Perform(DropJob& job, const ComPtr<IDataObject>& source) {
    const auto descriptor = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
    const auto contents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
    if (!descriptor || !contents) return {Stage::Formats, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)};
    ComPtr<IShellItem> folder;
    const auto shell_path = path::StripExtendedPathPrefix(job.destination);
    HRESULT hr = SHCreateItemFromParsingName(shell_path.c_str(), nullptr, IID_PPV_ARGS(&folder));
    if (FAILED(hr)) return {Stage::BindItem, hr};
    constexpr SFGAOF directory = SFGAO_FOLDER | SFGAO_FILESYSTEM;
    SFGAOF attrs = 0;
    hr = folder->GetAttributes(directory, &attrs);
    if (FAILED(hr) || (attrs & directory) != directory)
        return {Stage::CheckFolder, FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_DIRECTORY)};
    ComPtr<IDropTarget> target;
    hr = folder->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&target));
    if (FAILED(hr)) return {Stage::BindTarget, hr};
    ComPtr<IDataObject> snapshot;
    snapshot.Attach(new SnapshotDataObject(source, std::move(job.files), descriptor, contents));
    const POINTL point{};
    DWORD effect = DROPEFFECT_COPY;
    hr = target->DragEnter(snapshot.Get(), MK_LBUTTON, point, &effect);
    if (hr != S_OK || !(effect & DROPEFFECT_COPY)) {
        if (SUCCEEDED(hr)) target->DragLeave();
        return {Stage::DragEnter, hr == S_OK ? HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) : hr, effect};
    }
    effect = DROPEFFECT_COPY;
    hr = target->Drop(snapshot.Get(), 0, point, &effect);
    if (hr == S_OK && !(effect & DROPEFFECT_COPY)) hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    return {Stage::Drop, hr, effect};
}

unsigned __stdcall RunDrop(void* context) noexcept {
    std::unique_ptr<std::shared_ptr<DropJob>> argument(static_cast<std::shared_ptr<DropJob>*>(context));
    const auto job = std::move(*argument);
    argument.reset();
    const HRESULT init = OleInitialize(nullptr);
    job->initialize = init;
    SetEvent(job->ready.get());
    if (FAILED(init)) return 0;
    struct Apartment { ~Apartment() { OleUninitialize(); } } apartment;
    if (WaitForSingleObject(job->submitted.get(), INFINITE) != WAIT_OBJECT_0 || !job->packet) return 0;

    const size_t count = job->files.size();
    ComPtr<IDataObject> source;
    const HRESULT hr = CoGetInterfaceAndReleaseStream(std::exchange(job->packet, nullptr), IID_PPV_ARGS(&source));
    if (FAILED(hr)) { Report(Stage::Unmarshal, hr, count); return 0; }

    DropResult result{Stage::Drop, E_UNEXPECTED};
    try {
        result = Perform(*job, source);
    } catch (const std::bad_alloc&) {
        result = {Stage::Allocate, E_OUTOFMEMORY};
    } catch (...) {
        result = {Stage::Drop, E_UNEXPECTED};
    }
    Report(result.stage, result.hr, count, result.effect);
    if (job->source_async_started) {
        ComPtr<IDataObjectAsyncCapability> async;
        HRESULT finish = source.As(&async);
        if (SUCCEEDED(finish)) {
            const bool copied = result.hr == S_OK && (result.effect & DROPEFFECT_COPY);
            finish = async->EndOperation(copied ? S_OK : result.hr, nullptr,
                copied ? DROPEFFECT_COPY : DROPEFFECT_NONE);
        }
        if (FAILED(finish)) Report(Stage::SourceEnd, finish, count);
    }
    return 0;
}

} // namespace

bool QueueVirtualFileDrop(std::wstring destination, IDataObject* source,
                          std::vector<FILEDESCRIPTORW> files) {
    const size_t count = files.size();
    if (!source || destination.empty() || !count ||
        count > static_cast<size_t>((std::numeric_limits<LONG>::max)()) ||
        count > ((std::numeric_limits<size_t>::max)() - offsetof(FILEGROUPDESCRIPTORW, fgd)) / sizeof(FILEDESCRIPTORW))
        return false;

    try {
        auto job = std::make_shared<DropJob>();
        job->destination = std::move(destination);
        job->files = std::move(files);
        job->ready.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        job->submitted.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!job->ready || !job->submitted) {
            Report(Stage::StartThread, HRESULT_FROM_WIN32(ERROR_NO_SYSTEM_RESOURCES), count);
            return false;
        }
        auto* argument = new std::shared_ptr<DropJob>(job);
        Handle thread(reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, RunDrop, argument, 0, nullptr)));
        if (!thread) {
            delete argument;
            Report(Stage::StartThread, HRESULT_FROM_WIN32(ERROR_NO_SYSTEM_RESOURCES), count);
            return false;
        }
        struct Publish { HANDLE event; ~Publish() { SetEvent(event); } } publish{job->submitted.get()};
        // Wait only for apartment initialization; binding and all file I/O
        // stay on the worker. Do not marshal if this apartment cannot start.
        const DWORD wait = WaitForSingleObject(job->ready.get(), kApartmentStartWaitMs);
        if (wait != WAIT_OBJECT_0 || FAILED(job->initialize)) {
            const HRESULT error = wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) :
                wait == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : job->initialize;
            Report(Stage::Initialize, error, count);
            return false;
        }
        ComPtr<IStream> packet;
        const HRESULT hr = CoMarshalInterThreadInterfaceInStream(IID_IDataObject, source, &packet);
        if (FAILED(hr)) { Report(Stage::Marshal, hr, count); return false; }

        BOOL async_mode = FALSE;
        ComPtr<IDataObjectAsyncCapability> async;
        if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&async))) &&
            SUCCEEDED(async->GetAsyncMode(&async_mode)) && async_mode) {
            const HRESULT start = async->StartOperation(nullptr);
            if (FAILED(start)) {
                Report(Stage::SourceStart, start, count);
                const HRESULT release = CoReleaseMarshalData(packet.Get());
                if (FAILED(release)) Report(Stage::ReleaseMarshal, release, count);
                return false;
            }
            job->source_async_started = true;
        }
        job->packet = packet.Detach();
        return true;
    } catch (const std::bad_alloc&) {
        Report(Stage::Allocate, E_OUTOFMEMORY, count);
        return false;
    }
}

} // namespace pulse::ops
