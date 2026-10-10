// Exercise the production drag implementation with only the native modal loop
// replaced, so no mouse gesture or system clipboard is needed.
#include "../ui/drag_drop.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

HRESULT WINAPI AuditNativeDrag(IDataObject*, IDropSource*, DWORD, DWORD*);
#define DoDragDrop AuditNativeDrag
#include "../ui/drag_drop.cpp"
#undef DoDragDrop

namespace {
int failures = 0;
int passed = 0;
int native_calls = 0;
std::vector<std::wstring> native_paths;
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

HRESULT WINAPI AuditNativeDrag(IDataObject* data, IDropSource*, DWORD allowed, DWORD* effect) {
    ++native_calls;
    pulse::ui::ExtractHDropPaths(data, native_paths);
    *effect = allowed & DROPEFFECT_COPY;
    return DRAGDROP_S_DROP;
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
    DeleteFileW(original.c_str());
    DeleteFileW(archived.c_str());
    RemoveDirectoryW(root.c_str());
    if (SUCCEEDED(initialized)) CoUninitialize();
    printf("Drag/drop: %d passed, %d failed\n", passed, failures);
    return failures ? 1 : 0;
}
