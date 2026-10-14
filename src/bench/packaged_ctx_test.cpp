// Packaged File Explorer context-menu verbs (#48): manifest parsing, item-type
// matching and the IExplorerCommand -> IContextMenu adapter. `--live` also
// lists the verbs installed on this machine and their titles; `--pipe` queries
// real menus through pulse_shell.exe. Both are read-only: nothing is invoked.
#include "../shell_host/packaged_ctx_handlers.h"
#include "../ipc/shell_client.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using namespace pulse::shell;

namespace {

bool g_ok = true;

void Check(bool result, const char* name) {
    std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
    g_ok &= result;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

// ---- fake IExplorerCommand -------------------------------------------------

class FakeCommand;

class FakeEnum final : public IEnumExplorerCommand {
public:
    explicit FakeEnum(std::vector<FakeCommand*> items);
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IEnumExplorerCommand)) {
            *ppv = static_cast<IEnumExplorerCommand*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return refs;
    }
    IFACEMETHODIMP Next(ULONG count, IExplorerCommand** out, ULONG* fetched) override;
    IFACEMETHODIMP Skip(ULONG) override { return E_NOTIMPL; }
    IFACEMETHODIMP Reset() override { index_ = 0; return S_OK; }
    IFACEMETHODIMP Clone(IEnumExplorerCommand** out) override {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }

private:
    ~FakeEnum();
    LONG refs_ = 1;
    size_t index_ = 0;
    std::vector<FakeCommand*> items_;
};

class FakeCommand final : public IExplorerCommand, public IForegroundTransfer {
public:
    FakeCommand(const wchar_t* title, EXPCMDFLAGS flags = ECF_DEFAULT,
                EXPCMDSTATE state = ECS_ENABLED)
        : title_(title), flags_(flags), state_(state) {}

    void Add(FakeCommand* child) { children_.push_back(child); }  // takes the reference

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IExplorerCommand)) {
            *ppv = static_cast<IExplorerCommand*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IForegroundTransfer) && expose_foreground_transfer) {
            *ppv = static_cast<IForegroundTransfer*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return refs;
    }
    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* out) override {
        return SHStrDupW(title_.c_str(), out);
    }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* out) override {
        *out = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* out) override {
        *out = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetCanonicalName(GUID* out) override {
        *out = GUID_NULL;
        return S_OK;
    }
    IFACEMETHODIMP GetState(IShellItemArray*, BOOL, EXPCMDSTATE* out) override {
        *out = state_;
        return S_OK;
    }
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override {
        ++invoked;
        last_items = items;
        transferred_before_invoke = foreground_transfers > 0;
        return S_OK;
    }
    IFACEMETHODIMP AllowForegroundTransfer(void*) override {
        ++foreground_transfers;
        return foreground_result;
    }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* out) override {
        *out = flags_;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** out) override {
        if (!(flags_ & ECF_HASSUBCOMMANDS)) {
            *out = nullptr;
            return E_NOTIMPL;
        }
        *out = new FakeEnum(children_);
        return S_OK;
    }

    int invoked = 0;
    IShellItemArray* last_items = nullptr;
    bool expose_foreground_transfer = false;
    bool transferred_before_invoke = false;
    int foreground_transfers = 0;
    HRESULT foreground_result = S_OK;

private:
    ~FakeCommand() {
        for (auto* child : children_) child->Release();
    }
    LONG refs_ = 1;
    std::wstring title_;
    EXPCMDFLAGS flags_;
    EXPCMDSTATE state_;
    std::vector<FakeCommand*> children_;
};

FakeEnum::FakeEnum(std::vector<FakeCommand*> items) : items_(std::move(items)) {
    for (auto* item : items_) item->AddRef();
}
FakeEnum::~FakeEnum() {
    for (auto* item : items_) item->Release();
}
HRESULT STDMETHODCALLTYPE FakeEnum::Next(ULONG count, IExplorerCommand** out, ULONG* fetched) {
    ULONG got = 0;
    while (got < count && index_ < items_.size()) {
        out[got] = items_[index_++];
        out[got]->AddRef();
        ++got;
    }
    if (fetched) *fetched = got;
    return got == count ? S_OK : S_FALSE;
}

std::wstring ItemText(HMENU menu, int position) {
    wchar_t buf[128]{};
    GetMenuStringW(menu, static_cast<UINT>(position), buf, ARRAYSIZE(buf), MF_BYPOSITION);
    return buf;
}

HRESULT InvokeOffset(IContextMenu* menu, UINT offset) {
    CMINVOKECOMMANDINFO info{ sizeof(info) };
    info.lpVerb = MAKEINTRESOURCEA(offset);
    info.nShow = SW_SHOWNORMAL;
    return menu->InvokeCommand(&info);
}

// ---- tests ------------------------------------------------------------------

void TestForegroundTransfer(IShellItemArray* items) {
    auto* root = new FakeCommand(L"Editor", ECF_HASSUBCOMMANDS);
    auto* first = new FakeCommand(L"First editor");
    auto* second = new FakeCommand(L"Second editor");
    first->expose_foreground_transfer = true;
    second->expose_foreground_transfer = true;
    first->AddRef();
    second->AddRef();
    root->Add(first);
    root->Add(second);
    IContextMenu* menu = nullptr;
    Check(SUCCEEDED(CreateExplorerCommandMenu(root, items, &menu)) && menu,
          "foreground adapter created");
    if (menu) {
        HMENU popup = CreatePopupMenu();
        Check(SUCCEEDED(menu->QueryContextMenu(popup, 0, 100, 150, CMF_NORMAL)) &&
                  first->foreground_transfers == 0 && second->foreground_transfers == 0,
              "querying COM commands does not transfer foreground permission");
        Check(SUCCEEDED(InvokeOffset(menu, 2)) && second->foreground_transfers == 1 &&
                  second->transferred_before_invoke && second->last_items == items &&
                  first->foreground_transfers == 0 && root->foreground_transfers == 0,
              "selected COM command receives foreground permission before invocation");
        Check(FAILED(InvokeOffset(menu, 99)) && second->foreground_transfers == 1 &&
                  first->foreground_transfers == 0,
              "invalid menu offsets do not transfer foreground permission");
        first->foreground_result = E_ACCESSDENIED;
        Check(SUCCEEDED(InvokeOffset(menu, 1)) && first->foreground_transfers == 1 &&
                  first->invoked == 1 && first->last_items == items,
              "foreground transfer failure does not suppress the command");
        second->expose_foreground_transfer = false;
        Check(SUCCEEDED(InvokeOffset(menu, 2)) && second->foreground_transfers == 1 &&
                  second->invoked == 2,
              "in-process commands remain invocable without foreground transfer support");
        DestroyMenu(popup);
        menu->Release();
    }
    root->Release();
    first->Release();
    second->Release();
}

void TestAdapter(IShellItemArray* items) {
    constexpr UINT kFirst = 100;
    {
        auto* flat = new FakeCommand(L"Open in Terminal");
        IContextMenu* menu = nullptr;
        Check(SUCCEEDED(CreateExplorerCommandMenu(flat, items, &menu)) && menu, "adapter created");
        HMENU popup = CreatePopupMenu();
        const HRESULT hr = menu->QueryContextMenu(popup, 0, kFirst, kFirst + 50, CMF_NORMAL);
        Check(HRESULT_CODE(hr) == 1 && GetMenuItemCount(popup) == 1 &&
                  ItemText(popup, 0) == L"Open in Terminal" &&
                  GetMenuItemID(popup, 0) == kFirst,
              "flat command inserts one item with the first id");
        Check(SUCCEEDED(InvokeOffset(menu, 0)) && flat->invoked == 1 && flat->last_items == items,
              "flat command invokes with the bound items");
        Check(FAILED(InvokeOffset(menu, 1)) && flat->invoked == 1, "out-of-range offset rejected");
        CMINVOKECOMMANDINFO by_name{ sizeof(by_name) };
        by_name.lpVerb = "open";
        Check(FAILED(menu->InvokeCommand(&by_name)) && flat->invoked == 1,
              "string verbs are not routed to the command");
        DestroyMenu(popup);
        popup = CreatePopupMenu();
        Check(HRESULT_CODE(menu->QueryContextMenu(popup, 0, kFirst, kFirst + 50, CMF_DEFAULTONLY)) == 0 &&
                  GetMenuItemCount(popup) == 0,
              "CMF_DEFAULTONLY adds nothing");
        DestroyMenu(popup);
        menu->Release();
        flat->Release();
    }
    {
        auto* root = new FakeCommand(L"Bandizip", ECF_HASSUBCOMMANDS);
        auto* a = new FakeCommand(L"Extract here");
        auto* sep = new FakeCommand(L"", ECF_ISSEPARATOR);
        auto* hidden = new FakeCommand(L"Hidden", ECF_DEFAULT, ECS_HIDDEN);
        auto* c = new FakeCommand(L"Test archive", ECF_DEFAULT, ECS_DISABLED);
        a->AddRef();
        c->AddRef();
        root->Add(a);
        root->Add(sep);
        root->Add(hidden);
        root->Add(c);
        IContextMenu* menu = nullptr;
        CreateExplorerCommandMenu(root, items, &menu);
        HMENU popup = CreatePopupMenu();
        AppendMenuW(popup, MF_STRING, 1, L"existing");
        const HRESULT hr = menu->QueryContextMenu(popup, 1, kFirst, kFirst + 50, CMF_NORMAL);
        HMENU sub = GetSubMenu(popup, 1);
        Check(HRESULT_CODE(hr) == 3 && GetMenuItemCount(popup) == 2 && ItemText(popup, 1) == L"Bandizip" && sub,
              "sub-commands become a submenu at the requested position");
        Check(sub && GetMenuItemCount(sub) == 3 && ItemText(sub, 0) == L"Extract here" &&
                  (GetMenuState(sub, 1, MF_BYPOSITION) & MF_SEPARATOR) &&
                  ItemText(sub, 2) == L"Test archive",
              "hidden children skipped, separators kept");
        Check(sub && (GetMenuState(sub, 2, MF_BYPOSITION) & MF_GRAYED) &&
                  GetMenuItemID(sub, 0) == kFirst + 1 && GetMenuItemID(sub, 2) == kFirst + 2,
              "disabled state and ids map to children");
        Check(FAILED(InvokeOffset(menu, 0)), "submenu parent is not invocable");
        Check(SUCCEEDED(InvokeOffset(menu, 1)) && a->invoked == 1 && c->invoked == 0,
              "child offset routes to the right command");
        Check(SUCCEEDED(InvokeOffset(menu, 2)) && c->invoked == 1 && a->invoked == 1,
              "second child offset routes to the right command");
        DestroyMenu(popup);
        menu->Release();
        root->Release();
        a->Release();
        c->Release();
    }
    {
        auto* hidden = new FakeCommand(L"Hidden root", ECF_DEFAULT, ECS_HIDDEN);
        auto* empty = new FakeCommand(L"Empty flyout", ECF_HASSUBCOMMANDS);
        empty->Add(new FakeCommand(L"only hidden", ECF_DEFAULT, ECS_HIDDEN));
        IContextMenu* m1 = nullptr;
        IContextMenu* m2 = nullptr;
        CreateExplorerCommandMenu(hidden, items, &m1);
        CreateExplorerCommandMenu(empty, items, &m2);
        HMENU popup = CreatePopupMenu();
        const HRESULT h1 = m1->QueryContextMenu(popup, 0, kFirst, kFirst + 50, CMF_NORMAL);
        const HRESULT h2 = m2->QueryContextMenu(popup, 0, kFirst, kFirst + 50, CMF_NORMAL);
        Check(HRESULT_CODE(h1) == 0 && HRESULT_CODE(h2) == 0 && GetMenuItemCount(popup) == 0,
              "hidden command and empty flyout add nothing and consume no ids");
        DestroyMenu(popup);
        m1->Release();
        m2->Release();
        hidden->Release();
        empty->Release();
    }
    {
        auto* root = new FakeCommand(L"Many", ECF_HASSUBCOMMANDS);
        for (int i = 0; i < 10; ++i) root->Add(new FakeCommand(L"child"));
        IContextMenu* menu = nullptr;
        CreateExplorerCommandMenu(root, items, &menu);
        HMENU popup = CreatePopupMenu();
        const HRESULT hr = menu->QueryContextMenu(popup, 0, kFirst, kFirst + 4, CMF_NORMAL);
        HMENU sub = GetSubMenu(popup, 0);
        Check(HRESULT_CODE(hr) == 5 && sub && GetMenuItemCount(sub) == 4,
              "id range limit respected");
        DestroyMenu(popup);
        menu->Release();
        root->Release();
    }
}

constexpr char kManifest[] = R"(<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
         xmlns:uap3="http://schemas.microsoft.com/appx/manifest/uap/windows10/3"
         xmlns:desktop4="http://schemas.microsoft.com/appx/manifest/desktop/windows10/4"
         xmlns:desktop5="http://schemas.microsoft.com/appx/manifest/desktop/windows10/5"
         xmlns:com="http://schemas.microsoft.com/appx/manifest/com/windows10">
  <Identity Name="Bandizip.Shell" Publisher="CN=Test" Version="1.0.0.0" />
  <Properties>
    <DisplayName>Bandizip Shell</DisplayName>
    <PublisherDisplayName>Bandisoft</PublisherDisplayName>
  </Properties>
  <Applications>
    <Application Id="App">
      <uap:VisualElements DisplayName="Visual name" Description="x" BackgroundColor="transparent"
                          Square150x150Logo="a.png" Square44x44Logo="b.png" />
      <Extensions>
        <uap3:Extension Category="windows.fileTypeAssociation">
          <uap3:FileTypeAssociation Name="zip">
            <uap:SupportedFileTypes><uap:FileType>.zip</uap:FileType></uap:SupportedFileTypes>
            <uap3:SupportedVerbs><uap3:Verb Id="open" Clsid="{11111111-1111-1111-1111-111111111111}">Open</uap3:Verb></uap3:SupportedVerbs>
          </uap3:FileTypeAssociation>
        </uap3:Extension>
        <desktop4:Extension Category="windows.fileExplorerContextMenus">
          <desktop4:FileExplorerContextMenus>
            <desktop5:ItemType Type="*">
              <desktop5:Verb Id="Cmd1" Clsid="0001DEAD-9BF7-4CFA-8A5C-DE8679340001" />
            </desktop5:ItemType>
            <desktop5:ItemType Type="Directory">
              <desktop5:Verb Id="Cmd1" Clsid="0001DEAD-9BF7-4CFA-8A5C-DE8679340001" />
              <desktop5:Verb Id="Bad" Clsid="not-a-guid" />
            </desktop5:ItemType>
            <desktop5:ItemType Type="Directory\Background">
              <desktop5:Verb Id="Cmd2" Clsid="{0001DEAD-9BF7-4CFA-8A5C-DE8679340002}" />
            </desktop5:ItemType>
          </desktop4:FileExplorerContextMenus>
        </desktop4:Extension>
        <com:Extension Category="windows.comServer">
          <com:ComServer><com:SurrogateServer DisplayName="x">
            <com:Class Id="0001DEAD-9BF7-4CFA-8A5C-DE8679340001" Path="x.dll" ThreadingModel="STA" />
          </com:SurrogateServer></com:ComServer>
        </com:Extension>
      </Extensions>
    </Application>
    <Application Id="Second">
      <Extensions>
        <desktop4:Extension Category="windows.fileExplorerContextMenus">
          <desktop4:FileExplorerContextMenus>
            <desktop5:ItemType Type=".TXT">
              <desktop5:Verb Id="Txt" Clsid="9F156763-7844-4DC4-B2B1-901F640F5155" />
            </desktop5:ItemType>
          </desktop4:FileExplorerContextMenus>
        </desktop4:Extension>
      </Extensions>
    </Application>
  </Applications>
</Package>
)";

const PackagedCtxVerb* Find(const std::vector<PackagedCtxVerb>& verbs, const wchar_t* clsid) {
    for (const auto& verb : verbs)
        if (verb.clsid_text == clsid) return &verb;
    return nullptr;
}

void TestManifest() {
    IStream* stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(kManifest),
                                        static_cast<UINT>(sizeof(kManifest) - 1));
    std::wstring identity, display;
    const auto verbs = ParsePackagedContextMenus(stream, &identity, &display);
    if (stream) stream->Release();
    Check(identity == L"Bandizip.Shell" && display == L"Bandizip Shell",
          "manifest identity and package display name");
    Check(verbs.size() == 3, "only context-menu verbs with valid CLSIDs are read");
    const auto* one = Find(verbs, L"{0001dead-9bf7-4cfa-8a5c-de8679340001}");
    const auto* two = Find(verbs, L"{0001dead-9bf7-4cfa-8a5c-de8679340002}");
    const auto* txt = Find(verbs, L"{9f156763-7844-4dc4-b2b1-901f640f5155}");
    Check(one && one->verb_id == L"Cmd1" && one->item_types.size() == 2 &&
              one->item_types[0] == L"*" && one->item_types[1] == L"directory",
          "item types merge per CLSID");
    Check(two && two->item_types.size() == 1 && two->item_types[0] == L"directory\\background",
          "braced CLSID and background type");
    Check(txt && txt->item_types.size() == 1 && txt->item_types[0] == L".txt",
          "extensions from later applications, lower-cased");
    Check(!Find(verbs, L"{11111111-1111-1111-1111-111111111111}"),
          "file type association verbs ignored");
    Check(ParsePackagedContextMenus(nullptr, nullptr, nullptr).empty(), "null stream is safe");
}

void TestItemTypes(const std::wstring& dir, const std::wstring& file) {
    const auto background = PackagedItemTypesFor(true, dir);
    const auto folder = PackagedItemTypesFor(false, dir);
    const auto txt = PackagedItemTypesFor(false, file);
    const auto extended = PackagedItemTypesFor(false, L"\\\\?\\" + file);
    const auto drive = PackagedItemTypesFor(false, L"C:\\");
    auto has = [](const std::vector<std::wstring>& types, const wchar_t* type) {
        for (const auto& t : types)
            if (t == type) return true;
        return false;
    };
    Check(background.size() == 1 && background[0] == L"directory\\background", "background type");
    Check(has(folder, L"directory") && has(folder, L"folder") && !has(folder, L"*"), "folder types");
    Check(has(txt, L"*") && has(txt, L".txt") && !has(txt, L"directory"), "file types with lower-case extension");
    Check(has(extended, L".txt") && has(extended, L"*"), "extended-length file path");
    Check(has(drive, L"drive") && !has(drive, L"directory"), "drive root types");
    PackagedCtxVerb verb;
    verb.item_types = { L"directory\\background" };
    Check(PackagedVerbMatches(verb, background) && !PackagedVerbMatches(verb, txt), "verb matching");
}

void TestDuplicates() {
    const std::vector<std::wstring> classic = { L"压缩为“win.zip”", L"压缩为“win.7z”",
                                                L"新建压缩包 (Bandizip)(&B)...", L"发送到(N)" };
    Check(PackagedRowsDuplicate({ L"压缩为“win.zip”", L"  压缩为“win.7z” ",
                                  L"新建压缩包 (Bandizip)(B)...", L"用 Bandizip 打开..." }, classic),
          "packaged twin of a classic handler is detected");
    Check(!PackagedRowsDuplicate({ L"在记事本中编辑" }, classic), "unique packaged verb kept");
    Check(!PackagedRowsDuplicate({ L"发送到(N)", L"A", L"B" }, classic),
          "one shared generic row does not hide a packaged verb");
    Check(!PackagedRowsDuplicate({}, classic) && !PackagedRowsDuplicate({ L"" }, classic),
          "empty packaged rows are not duplicates");
}

void Live() {
    const DWORD start = GetTickCount();
    const auto verbs = PackagedContextMenuVerbs();
    const DWORD scan_ms = GetTickCount() - start;
    const DWORD again_start = GetTickCount();
    const auto again = PackagedContextMenuVerbs();
    std::printf("[INFO] %zu packaged verbs, scan %lu ms, cached %lu ms\n", verbs.size(),
                static_cast<unsigned long>(scan_ms),
                static_cast<unsigned long>(GetTickCount() - again_start));
    Check(again.size() == verbs.size(), "live: cached enumeration stable");

    wchar_t profile[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_WINDOWS, nullptr, 0, profile);
    const std::wstring target = std::wstring(profile) + L"\\win.ini";
    const std::wstring folder = profile;
    for (const auto& verb : verbs) {
        std::string types;
        for (const auto& t : verb.item_types) types += (types.empty() ? "" : ",") + Utf8(t);
        std::printf("[INFO] %s %s (%s)\n", Utf8(verb.clsid_text).c_str(), Utf8(verb.name).c_str(),
                    types.c_str());
        const bool background = PackagedVerbMatches(verb, PackagedItemTypesFor(true, folder));
        const std::wstring path = background ? folder : target;
        IShellItem* item = nullptr;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) continue;
        IShellItemArray* items = nullptr;
        SHCreateShellItemArrayFromShellItem(item, IID_PPV_ARGS(&items));
        item->Release();
        IExplorerCommand* command = nullptr;
        const HRESULT hr = CoCreateInstance(verb.clsid, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
                                            IID_PPV_ARGS(&command));
        if (SUCCEEDED(hr) && command && items) {
            IContextMenu* menu = nullptr;
            CreateExplorerCommandMenu(command, items, &menu);
            HMENU popup = CreatePopupMenu();
            const HRESULT q = menu->QueryContextMenu(popup, 0, 1, 0x7fff, CMF_NORMAL);
            std::printf("[INFO]   %s -> %d item(s):", background ? "background" : "win.ini",
                        HRESULT_CODE(q));
            for (int i = 0; i < GetMenuItemCount(popup); ++i)
                std::printf(" \"%s\"%s", Utf8(ItemText(popup, i)).c_str(),
                            GetSubMenu(popup, i) ? "(submenu)" : "");
            std::printf("\n");
            DestroyMenu(popup);
            menu->Release();
        } else {
            std::printf("[INFO]   CoCreateInstance 0x%08lx\n", static_cast<unsigned long>(hr));
        }
        if (command) command->Release();
        if (items) items->Release();
    }
}

struct PipeResult {
    std::mutex mutex;
    std::condition_variable cv;
    uint32_t id = 0;
    bool done = false;
    std::vector<pulse::ipc::CtxMenuItem> items;
};

// One real context-menu query through pulse_shell.exe (closed, never invoked).
struct VisibleVerb {
    PackagedCtxVerb verb;
    std::vector<std::wstring> rows;  // invokable titles from the in-process adapter
};

void MenuRows(HMENU menu, std::vector<std::wstring>& rows) {
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        if (HMENU sub = GetSubMenu(menu, i)) MenuRows(sub, rows);
        else if (!(GetMenuState(menu, static_cast<UINT>(i), MF_BYPOSITION) & MF_SEPARATOR))
            rows.push_back(ItemText(menu, i));
    }
}

void PipeQuery(PipeResult& result, const std::wstring& path, bool background,
               const std::vector<VisibleVerb>& expected) {
    auto& client = pulse::ipc::ShellClient::Instance();
    {
        std::lock_guard<std::mutex> lock(result.mutex);
        result.done = false;
        result.items.clear();
        result.id = client.QueryContextMenu({ path }, 0, background, false);
    }
    std::unique_lock<std::mutex> lock(result.mutex);
    const bool arrived = result.cv.wait_for(lock, std::chrono::seconds(30), [&] { return result.done; });
    const char* where = background ? "background" : "win.ini";
    std::printf("[INFO] pipe %s: %zu row(s)\n", where, result.items.size());
    for (const auto& item : result.items)
        std::printf("[INFO]   %s%s%s  %s\n", item.child ? "  " : "", Utf8(item.text).c_str(),
                    item.has_children ? " >" : "", Utf8(item.clsid).c_str());
    std::string name = std::string("pipe: ") + where + " query completes";
    Check(arrived, name.c_str());
    for (const auto& visible : expected) {
        bool found = false;
        std::vector<std::wstring> classic_rows;
        for (const auto& item : result.items) {
            bool packaged = false;
            for (const auto& other : expected)
                if (_wcsicmp(item.clsid.c_str(), other.verb.clsid_text.c_str()) == 0) packaged = true;
            if (_wcsicmp(item.clsid.c_str(), visible.verb.clsid_text.c_str()) == 0) found = true;
            if (!packaged && !item.has_children) classic_rows.push_back(item.text);
        }
        const bool twin = PackagedRowsDuplicate(visible.rows, classic_rows);
        name = std::string("pipe: ") + where + (twin ? " folds classic twin " : " shows ") +
               Utf8(visible.verb.name);
        Check(found != twin, name.c_str());
    }
    const uint32_t id = result.id;
    lock.unlock();
    client.CloseContextMenu(id);
}

// Packaged verbs whose adapter yields at least one row for `path` (in-process).
std::vector<VisibleVerb> VisibleVerbs(const std::wstring& path, bool background) {
    std::vector<VisibleVerb> out;
    const auto types = PackagedItemTypesFor(background, path);
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return out;
    IShellItemArray* items = nullptr;
    SHCreateShellItemArrayFromShellItem(item, IID_PPV_ARGS(&items));
    item->Release();
    for (const auto& verb : PackagedContextMenuVerbs()) {
        if (!items || !PackagedVerbMatches(verb, types)) continue;
        IExplorerCommand* command = nullptr;
        if (FAILED(CoCreateInstance(verb.clsid, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
                                    IID_PPV_ARGS(&command))) || !command)
            continue;
        IContextMenu* menu = nullptr;
        CreateExplorerCommandMenu(command, items, &menu);
        HMENU popup = CreatePopupMenu();
        if (menu && HRESULT_CODE(menu->QueryContextMenu(popup, 0, 1, 0x7fff, CMF_NORMAL)) > 0) {
            VisibleVerb visible{ verb, {} };
            MenuRows(popup, visible.rows);
            out.push_back(std::move(visible));
        }
        DestroyMenu(popup);
        if (menu) menu->Release();
        command->Release();
    }
    if (items) items->Release();
    return out;
}

void Pipe() {
    static PipeResult result;
    pulse::ipc::ShellClient::Callbacks callbacks;
    callbacks.ctx_items = [](uint32_t id, std::vector<pulse::ipc::CtxMenuItem> items, bool partial,
                             std::vector<std::wstring>) {
        std::lock_guard<std::mutex> lock(result.mutex);
        if (id != result.id) return;
        result.items = std::move(items);
        if (!partial) {
            result.done = true;
            result.cv.notify_all();
        }
    };
    callbacks.done = [](uint32_t, uint32_t, bool, std::wstring) {};
    auto& client = pulse::ipc::ShellClient::Instance();
    client.Start(std::move(callbacks));
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring folder = windows;
    const std::wstring file = folder + L"\\win.ini";
    PipeQuery(result, file, false, VisibleVerbs(file, false));
    PipeQuery(result, folder, true, VisibleVerbs(folder, true));
    client.Stop();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    if (argc == 3 && wcscmp(argv[1], L"--foreground-proxy") == 0) {
        CLSID clsid{};
        IExplorerCommand* command = nullptr;
        IForegroundTransfer* transfer = nullptr;
        HRESULT hr = CLSIDFromString(argv[2], &clsid);
        if (SUCCEEDED(hr)) hr = CoCreateInstance(clsid, nullptr,
            CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&command));
        Check(SUCCEEDED(hr) && command, "installed Explorer command activates");
        if (command) hr = command->QueryInterface(IID_PPV_ARGS(&transfer));
        Check(SUCCEEDED(hr) && transfer, "installed COM proxy supports foreground transfer");
        if (FAILED(hr)) std::printf("  COM result: 0x%08lX\n", hr);
        if (transfer) transfer->Release();
        if (command) command->Release();
        CoUninitialize();
        return g_ok ? 0 : 1;
    }

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"pulse_packaged_ctx_test";
    const std::wstring file = dir + L"\\Readme.TXT";
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    IShellItem* item = nullptr;
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        SHCreateShellItemArrayFromShellItem(item, IID_PPV_ARGS(&items));
        item->Release();
    }
    Check(items != nullptr, "fixture shell item array");

    const bool foreground_only = argc == 2 && wcscmp(argv[1], L"--foreground") == 0;
    TestForegroundTransfer(items);
    if (!foreground_only) {
        TestAdapter(items);
        TestManifest();
        TestItemTypes(dir, file);
        TestDuplicates();
    }
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--live") == 0) Live();
        if (wcscmp(argv[i], L"--pipe") == 0) Pipe();
    }

    if (items) items->Release();
    DeleteFileW(file.c_str());
    RemoveDirectoryW(dir.c_str());
    CoUninitialize();
    std::printf("%s\n", g_ok ? "ALL PASS" : "SOME FAILED");
    return g_ok ? 0 : 1;
}
