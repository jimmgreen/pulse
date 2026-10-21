// Exercise the production drag implementation with only the native modal loop
// replaced, so no mouse gesture or system clipboard is needed.
#include "../ui/drag_drop.h"
#include "../ui/drag_preview_layout.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

HRESULT WINAPI AuditNativeDrag(IDataObject*, IDropSource*, DWORD, DWORD*);
HRESULT WINAPI AuditCreateDragHelper(REFCLSID, IUnknown*, DWORD, REFIID, void**);
#define DoDragDrop AuditNativeDrag
#define CoCreateInstance AuditCreateDragHelper
#include "../ui/drag_drop.cpp"
#undef CoCreateInstance
#undef DoDragDrop

namespace {
int failures = 0;
int passed = 0;
int native_calls = 0;
std::vector<std::wstring> native_paths;
DWORD native_allowed = 0;
DWORD native_preferred = 0;
HRESULT native_result = DRAGDROP_S_DROP;
std::function<void(IDropSource*)> native_source_audit;
struct DragImageAudit {
    unsigned creates = 0;
    unsigned live = 0;
    unsigned initializes = 0;
    unsigned source_cleanups = 0;
    unsigned enters = 0;
    unsigned overs = 0;
    unsigned leaves = 0;
    unsigned drops = 0;
    HRESULT create_result = S_OK;
    HRESULT initialize_result = S_OK;
    HRESULT enter_result = S_OK;
    HRESULT over_result = S_OK;
    HRESULT drop_result = S_OK;
    HBITMAP bitmap = nullptr;
    bool valid_image = false;
    POINT point{};
    DWORD effect = DROPEFFECT_NONE;
} image_audit;
void Check(bool ok, const char* label) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (ok) ++passed; else ++failures;
}
std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}
}

// Pulse's own data object with a different Preferred DropEffect, the way
// browser download lists, staged Electron drags and archive tools publish one.
class PreferringDataObject final : public IDataObject {
public:
    PreferringDataObject(IDataObject* inner, DWORD preferred)
        : inner_(inner), preferred_(preferred) { inner_->AddRef(); }
    ~PreferringDataObject() { inner_->Release(); }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG left = --ref_;
        if (!left) delete this;
        return left;
    }
    IFACEMETHODIMP GetData(FORMATETC* fmt, STGMEDIUM* out) override {
        if (fmt && out && fmt->cfFormat ==
                (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)) {
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            if (!h) return E_OUTOFMEMORY;
            *static_cast<DWORD*>(GlobalLock(h)) = preferred_;
            GlobalUnlock(h);
            out->tymed = TYMED_HGLOBAL;
            out->hGlobal = h;
            out->pUnkForRelease = nullptr;
            return S_OK;
        }
        return inner_->GetData(fmt, out);
    }
    IFACEMETHODIMP GetDataHere(FORMATETC* f, STGMEDIUM* m) override { return inner_->GetDataHere(f, m); }
    IFACEMETHODIMP QueryGetData(FORMATETC* f) override { return inner_->QueryGetData(f); }
    IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC* a, FORMATETC* b) override {
        return inner_->GetCanonicalFormatEtc(a, b);
    }
    IFACEMETHODIMP SetData(FORMATETC* f, STGMEDIUM* m, BOOL r) override { return inner_->SetData(f, m, r); }
    IFACEMETHODIMP EnumFormatEtc(DWORD d, IEnumFORMATETC** e) override { return inner_->EnumFormatEtc(d, e); }
    IFACEMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    IFACEMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ULONG ref_ = 1;
    IDataObject* inner_;
    DWORD preferred_;
};

class AuditedDragHelper final : public IDragSourceHelper, public IDropTargetHelper {
public:
    explicit AuditedDragHelper(bool source) : source_(source) { ++image_audit.live; }
    ~AuditedDragHelper() {
        if (owned_bitmap_) DeleteObject(owned_bitmap_);
        --image_audit.live;
    }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDragSourceHelper)
            *out = static_cast<IDragSourceHelper*>(this);
        else if (riid == IID_IDropTargetHelper)
            *out = static_cast<IDropTargetHelper*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG left = --ref_;
        if (!left) delete this;
        return left;
    }
    IFACEMETHODIMP InitializeFromBitmap(SHDRAGIMAGE* image, IDataObject* data) override {
        ++image_audit.initializes;
        image_audit.bitmap = image->hbmpDragImage;
        DIBSECTION dib{};
        std::vector<std::wstring> paths;
        const bool got_bitmap = GetObjectW(image->hbmpDragImage, sizeof(dib), &dib) ==
            static_cast<int>(sizeof(dib));
        bool transparent = false, opaque = false;
        if (got_bitmap && dib.dsBm.bmBits) {
            const auto* pixels = static_cast<const uint32_t*>(dib.dsBm.bmBits);
            const size_t count = static_cast<size_t>(dib.dsBm.bmWidth) * dib.dsBm.bmHeight;
            for (size_t i = 0; i < count; ++i) {
                transparent = transparent || pixels[i] == 0;
                opaque = opaque || (pixels[i] >> 24) == 255;
            }
        }
        image_audit.valid_image = got_bitmap && transparent && opaque &&
            image->sizeDragImage.cx == dib.dsBm.bmWidth &&
            image->sizeDragImage.cy == dib.dsBm.bmHeight &&
            image->sizeDragImage.cx <= 304 && image->sizeDragImage.cy <= 224 &&
            image->ptOffset.x == 0 && image->ptOffset.y == 0 &&
            image->crColorKey == CLR_NONE && pulse::ui::ExtractHDropPaths(data, paths);
        if (SUCCEEDED(image_audit.initialize_result)) owned_bitmap_ = image->hbmpDragImage;
        return image_audit.initialize_result;
    }
    IFACEMETHODIMP InitializeFromWindow(HWND, POINT*, IDataObject*) override { return E_NOTIMPL; }
    IFACEMETHODIMP DragEnter(HWND, IDataObject*, POINT* point, DWORD effect) override {
        ++image_audit.enters;
        image_audit.point = *point;
        image_audit.effect = effect;
        return image_audit.enter_result;
    }
    IFACEMETHODIMP DragOver(POINT* point, DWORD effect) override {
        ++image_audit.overs;
        image_audit.point = *point;
        image_audit.effect = effect;
        return image_audit.over_result;
    }
    IFACEMETHODIMP DragLeave() override {
        if (source_) ++image_audit.source_cleanups;
        else ++image_audit.leaves;
        return S_OK;
    }
    IFACEMETHODIMP Drop(IDataObject*, POINT* point, DWORD effect) override {
        ++image_audit.drops;
        image_audit.point = *point;
        image_audit.effect = effect;
        return image_audit.drop_result;
    }
    IFACEMETHODIMP Show(BOOL) override { return S_OK; }

private:
    ULONG ref_ = 1;
    HBITMAP owned_bitmap_ = nullptr;
    bool source_ = false;
};

HRESULT WINAPI AuditCreateDragHelper(REFCLSID clsid, IUnknown* outer, DWORD context,
                                    REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    ++image_audit.creates;
    if (clsid != CLSID_DragDropHelper || outer || context != CLSCTX_INPROC_SERVER)
        return E_INVALIDARG;
    if (FAILED(image_audit.create_result)) return image_audit.create_result;
    auto* helper = new AuditedDragHelper(riid == IID_IDragSourceHelper);
    const HRESULT hr = helper->QueryInterface(riid, out);
    helper->Release();
    return hr;
}

HRESULT WINAPI AuditNativeDrag(IDataObject* data, IDropSource* source, DWORD allowed, DWORD* effect) {
    ++native_calls;
    pulse::ui::ExtractHDropPaths(data, native_paths);
    native_allowed = allowed;
    native_preferred = pulse::ui::PreferredDropEffect(data);
    if (native_source_audit) native_source_audit(source);
    *effect = native_result == DRAGDROP_S_DROP ? allowed & DROPEFFECT_COPY : DROPEFFECT_NONE;
    return native_result;
}

int wmain() {
    using namespace pulse::ui;
    setvbuf(stdout, nullptr, _IONBF, 0);
    for (DWORD allowed : { DWORD(DROPEFFECT_NONE), DWORD(DROPEFFECT_COPY),
                           DWORD(DROPEFFECT_MOVE), DWORD(DROPEFFECT_COPY | DROPEFFECT_MOVE) }) {
        for (DWORD keys : { DWORD(0), DWORD(MK_CONTROL), DWORD(MK_SHIFT), DWORD(MK_CONTROL | MK_SHIFT) }) {
            for (bool cross_volume : { false, true }) {
                auto* data = FileDataObject::Create({ L"C:\\fixture\\source.txt" });
                unsigned over_calls = 0, drop_calls = 0;
                DWORD observed = 0;
                bool preference_seen = true;
                DropTargetCallbacks callbacks;
                // Pulse's own drags publish COPY|MOVE, which must keep the
                // same-volume move: the expectations below are unchanged.
                const auto compute = [&](const std::vector<std::wstring>& paths, POINT,
                                         DWORD state, DWORD mask, DWORD preferred) {
                    observed = mask;
                    preference_seen = preference_seen &&
                        preferred == (DROPEFFECT_COPY | DROPEFFECT_MOVE);
                    return ComputeDropEffect(state, paths.front(),
                        cross_volume ? L"D:\\target" : L"C:\\target", mask, preferred);
                };
                callbacks.drag_over = [&](const auto& paths, POINT point, DWORD state, DWORD mask,
                                          DWORD preferred) {
                    ++over_calls; return compute(paths, point, state, mask, preferred);
                };
                callbacks.drop = [&](const auto& paths, POINT point, DWORD state, DWORD mask,
                                     DWORD preferred) {
                    ++drop_calls; return compute(paths, point, state, mask, preferred);
                };
                auto* target = new WindowDropTarget(nullptr, std::move(callbacks));
                DWORD expected = allowed;
                if (allowed == (DROPEFFECT_COPY | DROPEFFECT_MOVE)) {
                    expected = keys == MK_SHIFT ? DROPEFFECT_MOVE :
                        (keys != 0 || cross_volume ? DROPEFFECT_COPY : DROPEFFECT_MOVE);
                }
                DWORD effect = allowed;
                target->DragEnter(data, keys, POINTL{}, &effect);
                const bool entered = effect == expected;
                effect = allowed;
                target->DragOver(keys, POINTL{}, &effect);
                const bool hovered = effect == expected;
                effect = allowed;
                target->Drop(data, keys, POINTL{}, &effect);
                const bool callbacks_correct = allowed
                    ? observed == allowed && over_calls == 2 && drop_calls == 1 && preference_seen
                    : over_calls == 0 && drop_calls == 0;
                Check(entered && hovered && effect == expected &&
                      data->PerformedEffect() == expected && callbacks_correct,
                      "source effect mask survives enter/over/drop and modifiers");
                target->Release();
                data->Release();
            }
        }
    }
    {
        auto* data = FileDataObject::Create({L"C:\\fixture\\source.txt"});
        DWORD observed = 0;
        DropTargetCallbacks callbacks;
        callbacks.drop = [&](const auto&, POINT, DWORD, DWORD mask, DWORD) {
            observed = mask; return DROPEFFECT_MOVE;
        };
        auto* target = new WindowDropTarget(nullptr, std::move(callbacks));
        DWORD effect = DROPEFFECT_COPY;
        target->Drop(data, 0, POINTL{}, &effect);
        Check(observed == DROPEFFECT_COPY && effect == DROPEFFECT_NONE &&
              data->PerformedEffect() == DROPEFFECT_NONE,
              "forbidden callback effect is not reported to source");
        target->Release();
        data->Release();
    }
    {
        auto* data = FileDataObject::Create({L"C:\\fixture\\source.txt"});
        unsigned drop_calls = 0, leave_calls = 0;
        DropTargetCallbacks callbacks;
        callbacks.drop = [&](const auto&, POINT, DWORD, DWORD, DWORD) { ++drop_calls; return DROPEFFECT_COPY; };
        callbacks.drag_leave = [&] { ++leave_calls; };
        auto* target = new WindowDropTarget(nullptr, std::move(callbacks));
        DWORD effect = DROPEFFECT_COPY;
        target->Drop(nullptr, 0, POINTL{}, &effect);
        Check(effect == DROPEFFECT_NONE && drop_calls == 0 && leave_calls == 1,
              "invalid data cannot submit a drop and clears feedback");
        target->Release();
        data->Release();
    }
    // A source's exact preference reaches the callbacks and wins with no
    // modifier; COPY|MOVE and no preference keep the same-volume move, and
    // Ctrl/Shift still override.
    struct PreferenceCase { DWORD preferred; DWORD keys; DWORD expected; const char* label; };
    const PreferenceCase preference_cases[] = {
        { DROPEFFECT_COPY, 0, DROPEFFECT_COPY, "source asking for copy is copied on one volume" },
        { DROPEFFECT_MOVE, 0, DROPEFFECT_MOVE, "source asking for move is moved" },
        { DROPEFFECT_COPY | DROPEFFECT_MOVE, 0, DROPEFFECT_MOVE,
          "COPY|MOVE preference keeps the same-volume move" },
        { 0, 0, DROPEFFECT_MOVE, "no preference keeps the same-volume move" },
        { DROPEFFECT_COPY | DROPEFFECT_LINK, 0, DROPEFFECT_COPY, "COPY|LINK preference is a copy" },
        { DROPEFFECT_COPY, MK_SHIFT, DROPEFFECT_MOVE, "Shift overrides a copy preference" },
        { DROPEFFECT_MOVE, MK_CONTROL, DROPEFFECT_COPY, "Ctrl overrides a move preference" },
    };
    for (const auto& c : preference_cases) {
        auto* inner = FileDataObject::Create({ L"C:\\fixture\\source.txt" });
        auto* data = new PreferringDataObject(inner, c.preferred);
        inner->Release();
        DWORD over_preferred = 0xFFFF, drop_preferred = 0xFFFF;
        DropTargetCallbacks callbacks;
        callbacks.drag_over = [&](const auto& paths, POINT, DWORD state, DWORD mask, DWORD preferred) {
            over_preferred = preferred;
            return ComputeDropEffect(state, paths.front(), L"C:\\target", mask, preferred);
        };
        callbacks.drop = [&](const auto& paths, POINT, DWORD state, DWORD mask, DWORD preferred) {
            drop_preferred = preferred;
            return ComputeDropEffect(state, paths.front(), L"C:\\target", mask, preferred);
        };
        auto* target = new WindowDropTarget(nullptr, std::move(callbacks));
        DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        target->DragEnter(data, c.keys, POINTL{}, &effect);
        const bool shown = effect == c.expected;
        effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        target->Drop(data, c.keys, POINTL{}, &effect);
        Check(shown && effect == c.expected && over_preferred == c.preferred &&
              drop_preferred == c.preferred, c.label);
        target->Release();
        data->Release();
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto root = std::filesystem::current_path() / L"bench_data" / L"drag_drop_test" /
        std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(root);
    const auto original = root / L"original.txt";
    const auto archived = root / L"$Rfixture";
    { std::ofstream out(original, std::ios::binary); out << "new file at original path"; }
    { std::ofstream out(archived, std::ios::binary); out << "old recycled version"; }
    const auto before_original = ReadBytes(original);
    const auto before_archived = ReadBytes(archived);
    Check(!before_original.empty() && !before_archived.empty(), "create isolated recycle identity fixtures");
    const DWORD rejected = DoFileDragDrop({original.wstring()}, DROPEFFECT_COPY | DROPEFFECT_MOVE,
                                         {}, L"pulse:recycle");
    Check(rejected == DROPEFFECT_NONE && native_calls == 0 && native_paths.empty(),
          "recycle row cannot expose recreated original to a native drag");
    Check(ReadBytes(original) == before_original && ReadBytes(archived) == before_archived,
          "rejected recycle drag preserves both versions");
    const DWORD ordinary = DoFileDragDrop({original.wstring()}, DROPEFFECT_COPY, {}, root.wstring());
    Check(ordinary == DROPEFFECT_COPY && native_calls == 1 &&
          native_paths == std::vector<std::wstring>{original.wstring()},
          "ordinary drag still reaches native boundary with selected path");
    Check(image_audit.initializes == 1 && image_audit.valid_image &&
          image_audit.source_cleanups == 1 && image_audit.live == 0,
          "shell drag initializes valid drag image and cleans up helper on exit");
    {
        image_audit = {};
        DropTargetCallbacks callbacks;
        callbacks.drag_over = [](const auto&, POINT, DWORD, DWORD mask, DWORD) { return mask; };
        callbacks.drop = [](const auto&, POINT, DWORD, DWORD mask, DWORD) { return mask; };
        auto* target = new WindowDropTarget(reinterpret_cast<HWND>(1), std::move(callbacks));
        Check(image_audit.creates == 1 && image_audit.live == 1,
              "drop target creates drop target helper when window handle is provided");
        auto* data = FileDataObject::Create({ original.wstring() });
        DWORD effect = DROPEFFECT_COPY;
        target->DragEnter(data, 0, POINTL{ 100, 200 }, &effect);
        Check(image_audit.enters == 1 && image_audit.point.x == 100 && image_audit.point.y == 200,
              "drop target helper receives DragEnter with coordinates");
        effect = DROPEFFECT_COPY;
        target->DragOver(0, POINTL{ 150, 250 }, &effect);
        Check(image_audit.overs == 1 && image_audit.point.x == 150 && image_audit.point.y == 250,
              "drop target helper receives DragOver updates");
        effect = DROPEFFECT_COPY;
        target->Drop(data, 0, POINTL{ 160, 260 }, &effect);
        Check(image_audit.drops == 1 && image_audit.point.x == 160 && image_audit.point.y == 260,
              "drop target helper receives Drop event");
        target->Release();
        data->Release();
        Check(image_audit.live == 0, "drop target helper released with drop target");
    }
    {
        const D2D1_RECT_F bounds = D2D1::RectF(0, 0, 1000, 800);
        const auto r1 = ItemDragPreviewRect(bounds, 100, 100, 200, 50, 1.0f);
        Check(r1.left == 116.0f && r1.top == 120.0f && r1.right == 316.0f && r1.bottom == 170.0f,
              "drag preview offsets down-right from cursor within bounds");
        const auto r2 = ItemDragPreviewRect(bounds, 950, 780, 200, 50, 1.0f);
        Check(r2.right <= bounds.right && r2.bottom <= bounds.bottom &&
              r2.left < 950.0f && r2.top < 780.0f,
              "drag preview flips left and upward when approaching viewport bounds");
    }
    Check(DropIntoOwnFolder({L"E:\\work\\.minecraft"}, L"E:\\work\\.minecraft") &&
          DropIntoOwnFolder({L"E:\\work\\.minecraft"}, L"e:\\WORK\\.minecraft\\") &&
          DropIntoOwnFolder({L"E:\\work\\a.txt", L"E:\\work\\.minecraft"}, L"E:\\work\\.minecraft\\mods") &&
          DropIntoOwnFolder({L"\\\\?\\E:\\work\\pack"}, L"E:/work/pack/sub"),
          "a folder dropped onto itself or into its own subtree is not a target");
    Check(!DropIntoOwnFolder({L"E:\\work\\.minecraft"}, L"E:\\work") &&
          !DropIntoOwnFolder({L"E:\\work\\pack"}, L"E:\\work\\pack2") &&
          !DropIntoOwnFolder({L"E:\\work\\pack"}, L"E:\\work\\other\\pack") &&
          !DropIntoOwnFolder({}, L"E:\\work"),
          "parents, siblings with a shared prefix and other folders remain targets");
    Check(IsDropLaunchProgram(L"C:\\Tools\\App.EXE") && IsDropLaunchProgram(L"D:\\run.bat") &&
          IsDropLaunchProgram(L"D:\\run.cmd") && IsDropLaunchProgram(L"\\\\?\\C:\\old.com") &&
          !IsDropLaunchProgram(L"D:\\notes.txt") && !IsDropLaunchProgram(L"D:\\tool.exe\\readme") &&
          !IsDropLaunchProgram(L"D:\\App.lnk") && !IsDropLaunchProgram(L"D:\\noext"),
          "programs that open dropped items are recognized by extension only");
    Check(DropLaunchArguments({L"\\\\?\\C:\\a.txt", L"D:\\my files\\b c.txt", L"\\\\?\\UNC\\srv\\share\\d.txt",
                               L"E:\\with space\\"}) ==
              L"C:\\a.txt \"D:\\my files\\b c.txt\" \\\\srv\\share\\d.txt \"E:\\with space\\\\\"",
          "dropped items become one argument each, quoted only when they contain blanks");
    Check(FirstDroppableFolder({original.wstring()}).empty(),
          "regular file is not recognized as a droppable folder for tab bar or pane drops");
    Check(FirstDroppableFolder({root.wstring()}) == root.wstring(),
          "folder directory is recognized as a droppable folder for new tab");
    Check(FirstDroppableFolder({original.wstring(), root.wstring()}) == root.wstring(),
          "first droppable folder is identified in a mixed file and folder list");
    Check(LooksLikeFolderShortcut(L"C:\\shortcuts\\folder.lnk") &&
          LooksLikeFolderShortcut(L"D:\\TARGET.LNK") &&
          !LooksLikeFolderShortcut(L"C:\\folder\\target") &&
          !LooksLikeFolderShortcut(L"C:\\file.txt"),
          "folder shortcuts (.lnk) are recognized for header and new tab drop");
    DeleteFileW(original.c_str());
    DeleteFileW(archived.c_str());
    RemoveDirectoryW(root.c_str());
    if (SUCCEEDED(initialized)) CoUninitialize();
    printf("Drag/drop: %d passed, %d failed\n", passed, failures);
    return failures ? 1 : 0;
}
