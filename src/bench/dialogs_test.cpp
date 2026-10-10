// Rules of Pulse's own dialogs: confirm spec handling, the folder picker model
// and the off-thread folder reader (on a private temp fixture).

#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/confirm_dialog_view.h"
#include "../ui/folder_picker_loader.h"
#include "../ui/folder_picker_model.h"

#include <windows.h>

#include <cstdio>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace pulse::ui;

int g_failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    g_failures += !ok;
}

std::vector<std::wstring> Names(const std::vector<PickerEntry>& entries) {
    std::vector<std::wstring> names;
    for (const PickerEntry& e : entries) names.push_back(e.name);
    return names;
}

PickerEntry Entry(const wchar_t* name, PickerEntryKind kind, const wchar_t* path = L"") {
    PickerEntry e;
    e.name = name;
    e.kind = kind;
    e.path = path;
    return e;
}

void TestConfirm() {
    ConfirmDialogSpec raw;
    raw.title = L"t";
    raw.message = L"\\\\?\\C:\\a\\b.txt\n\n";
    raw.items = {L"\\\\?\\UNC\\srv\\share\\x"};
    const ConfirmDialogSpec spec = NormalizeConfirmSpec(raw);
    Check(spec.message == L"C:\\a\\b.txt", "confirm message drops long-path prefix and trailing breaks");
    Check(spec.items.size() == 1 && spec.items[0] == L"\\\\srv\\share\\x",
          "confirm items show friendly UNC paths");
    Check(!spec.confirm_text.empty() && !spec.cancel_text.empty(),
          "confirm fills default button texts");
    Check(ResolveConfirmTone(spec) == ConfirmTone::Question, "auto tone is a question");
    ConfirmDialogSpec danger = spec;
    danger.danger = true;
    Check(ResolveConfirmTone(danger) == ConfirmTone::Danger, "danger spec resolves to danger tone");
    danger.tone = ConfirmTone::Warning;
    Check(ResolveConfirmTone(danger) == ConfirmTone::Warning, "explicit tone wins over danger");

    ConfirmDialogSpec many;
    for (int i = 0; i < 7; ++i) many.items.push_back(L"item" + std::to_wstring(i));
    const auto rows = ConfirmItemRows(many);
    Check(rows.size() == kConfirmMaxItemRows && rows[3] == L"item3" &&
              rows.back().find(L'3') != std::wstring::npos,
          "seven items collapse to four rows plus an N-more row");
    many.items.resize(kConfirmMaxItemRows);
    Check(ConfirmItemRows(many).size() == kConfirmMaxItemRows, "five items show without collapsing");

    const auto chars = [](std::wstring_view s) { return static_cast<float>(s.size()); };
    const std::wstring long_path = L"C:\\Users\\Example\\Desktop\\bench\\fx\\delete-me.txt";
    Check(FitConfirmItemText(long_path, 100.0f, chars) == long_path, "a path that fits is unchanged");
    Check(FitConfirmItemText(long_path, 22.0f, chars) == L"C:\\\u2026\\fx\\delete-me.txt",
          "a long path keeps the drive and as many last folders as fit");
    Check(FitConfirmItemText(long_path, 18.0f, chars) == L"C:\\\u2026\\delete-me.txt",
          "a tight path keeps the drive and the item name");
    const std::wstring fitted = FitConfirmItemText(long_path, 8.0f, chars);
    Check(fitted == L"delete-\u2026", "a very tight path shortens the item name itself");
    Check(FitConfirmItemText(L"\\\\server\\share\\docs\\a.txt", 20.0f, chars) ==
              L"\\\\server\\\u2026\\a.txt",
          "a UNC path keeps its server");
    Check(FitConfirmItemText(L"plain item text", 8.0f, chars) == L"plain i\u2026",
          "text that is not a path gets a trailing ellipsis");

    ConfirmDialogSpec two;
    Check(ConfirmFocusOrder(two) == std::vector<int>{kConfirmCancel, kConfirmPrimary},
          "two-button focus order is cancel, primary");
    ConfirmDialogSpec three;
    three.secondary_text = L"No";
    Check(ConfirmFocusOrder(three) ==
              std::vector<int>{kConfirmCancel, kConfirmSecondary, kConfirmPrimary},
          "three-button focus order is cancel, secondary, primary");
    Check(ConfirmControlFor(ConfirmChoice::Confirm, false) == kConfirmPrimary &&
              ConfirmControlFor(ConfirmChoice::Cancel, true) == kConfirmCancel &&
              ConfirmControlFor(ConfirmChoice::Secondary, true) == kConfirmSecondary &&
              ConfirmControlFor(ConfirmChoice::Secondary, false) == kConfirmCancel,
          "default choice maps to its button; missing secondary falls back to cancel");
    ConfirmDialogSpec clip;
    clip.title = L"T";
    clip.message = L"M";
    clip.items = {L"a"};
    clip.note = L"N";
    Check(ConfirmClipboardText(clip) == L"T\r\n\r\nM\r\n  a\r\n\r\nN", "Ctrl+C text lists every part");
}

void TestPickerModel() {
    Check(IsPickerImageName(L"a.JPG") && IsPickerImageName(L"b.webp") &&
              IsPickerImageName(L"c.jfif") && !IsPickerImageName(L"d.gif") &&
              !IsPickerImageName(L"png"),
          "picture names match the old dialog's filter");
    Check(!PickerShowsEntry(FILE_ATTRIBUTE_DIRECTORY, L"..", PickerMode::Folder) &&
              !PickerShowsEntry(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN, L"h",
                                PickerMode::Folder) &&
              !PickerShowsEntry(FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_SYSTEM, L"x.png",
                                PickerMode::Image) &&
              PickerShowsEntry(FILE_ATTRIBUTE_DIRECTORY, L"d", PickerMode::Folder) &&
              !PickerShowsEntry(FILE_ATTRIBUTE_NORMAL, L"x.png", PickerMode::Folder) &&
              PickerShowsEntry(FILE_ATTRIBUTE_NORMAL, L"x.png", PickerMode::Image) &&
              !PickerShowsEntry(FILE_ATTRIBUTE_NORMAL, L"x.txt", PickerMode::Image),
          "hidden, system, dot entries and non-pictures stay out");

    std::vector<PickerEntry> sorted = {
        Entry(L"b.png", PickerEntryKind::Image), Entry(L"item 10", PickerEntryKind::Folder),
        Entry(L"A.png", PickerEntryKind::Image), Entry(L"item 2", PickerEntryKind::Folder),
        Entry(L"Alpha", PickerEntryKind::Folder)};
    SortPickerEntries(sorted);
    Check(Names(sorted) == std::vector<std::wstring>{L"Alpha", L"item 2", L"item 10", L"A.png",
                                                     L"b.png"},
          "folders first, numbers in natural order, case-insensitive");
    std::vector<PickerEntry> drives = {Entry(L"Z label (D:)", PickerEntryKind::Drive, L"D:\\"),
                                       Entry(L"A label (C:)", PickerEntryKind::Drive, L"C:\\")};
    drives[0].name = L"A data (D:)";
    drives[1].name = L"Z system (C:)";
    SortPickerEntries(drives);
    Check(drives[0].path == L"C:\\", "drives keep letter order, not label order");

    Check(PickerParent(L"C:\\a\\b") == L"C:\\a" && PickerParent(L"C:\\a") == L"C:\\" &&
              PickerParent(L"C:\\a\\") == L"C:\\" && PickerParent(L"C:\\").empty() &&
              PickerParent(L"").empty(),
          "parent walks up to the drive root, then This PC");
    Check(PickerParent(L"\\\\srv\\share\\x") == L"\\\\srv\\share" &&
              PickerParent(L"\\\\srv\\share").empty() && PickerParent(L"\\\\srv\\share\\").empty(),
          "a share root's parent is This PC");

    SetEnvironmentVariableW(L"PULSE_PICKER_TEST", L"D:\\data");
    Check(NormalizePickerInput(L"  \"c:/Users/x/\"  ") == L"C:\\Users\\x" &&
              NormalizePickerInput(L"d:") == L"D:\\" &&
              NormalizePickerInput(L"C:\\") == L"C:\\" &&
              NormalizePickerInput(L"%PULSE_PICKER_TEST%\\sub") == L"D:\\data\\sub" &&
              NormalizePickerInput(L"\\\\?\\C:\\long") == L"C:\\long" &&
              NormalizePickerInput(L"\\\\?\\UNC\\srv\\s\\") == L"\\\\srv\\s" &&
              NormalizePickerInput(L"   ").empty(),
          "typed paths are trimmed, unquoted, expanded and normalized");
    Check(SamePickerPath(L"c:\\Users\\", L"C:\\users") && !SamePickerPath(L"C:\\a", L"C:\\ab") &&
              SamePickerPath(L"", L"") && SamePickerPath(L"C:\\", L"c:\\"),
          "path comparison ignores case and trailing separators");

    const PickerEntry folder = Entry(L"f", PickerEntryKind::Folder, L"C:\\cur\\f");
    const PickerEntry image = Entry(L"i.png", PickerEntryKind::Image, L"C:\\cur\\i.png");
    const PickerEntry drive = Entry(L"C", PickerEntryKind::Drive, L"C:\\");
    Check(PickerChosenPath(PickerMode::Folder, L"C:\\cur", nullptr) == L"C:\\cur" &&
              PickerChosenPath(PickerMode::Folder, L"C:\\cur", &folder) == L"C:\\cur\\f" &&
              PickerChosenPath(PickerMode::Folder, L"", &drive) == L"C:\\" &&
              PickerChosenPath(PickerMode::Folder, L"", nullptr).empty(),
          "folder mode picks the selection, else the current folder; This PC alone picks nothing");
    Check(PickerChosenPath(PickerMode::Image, L"C:\\cur", &image) == L"C:\\cur\\i.png" &&
              PickerChosenPath(PickerMode::Image, L"C:\\cur", &folder).empty() &&
              PickerChosenPath(PickerMode::Image, L"C:\\cur", nullptr).empty(),
          "picture mode only picks a selected picture");

    const std::vector<PickerEntry> list = {Entry(L"apple", PickerEntryKind::Folder),
                                           Entry(L"Banana", PickerEntryKind::Folder),
                                           Entry(L"avocado", PickerEntryKind::Folder)};
    Check(PickerTypeAhead(list, -1, L'a') == 0 && PickerTypeAhead(list, 0, L'A') == 2 &&
              PickerTypeAhead(list, 2, L'a') == 0 && PickerTypeAhead(list, 0, L'b') == 1 &&
              PickerTypeAhead(list, 0, L'z') == -1 && PickerTypeAhead({}, -1, L'a') == -1,
          "type-ahead cycles through matching names and wraps");

    PickerHistory history;
    Check(!history.CanGoBack(), "history starts empty");
    history.Navigate(L"C:\\a");
    history.Navigate(L"c:\\A\\");
    history.Navigate(L"C:\\b");
    Check(history.Back() == L"C:\\b" && history.Back() == L"C:\\a" && !history.CanGoBack(),
          "back returns locations newest first and skips repeated ones");
    for (int i = 0; i < 100; ++i) history.Navigate(L"C:\\" + std::to_wstring(i));
    int depth = 0;
    while (history.CanGoBack()) { history.Back(); ++depth; }
    Check(depth == 64, "history depth is capped");
}

std::wstring MakeFixture() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring root = std::wstring(temp) + L"pulse_dialogs_test_" +
                              std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(root.c_str(), nullptr);
    for (const wchar_t* dir : {L"b", L"a10", L"a2", L"hidden", L"empty"})
        CreateDirectoryW((root + L"\\" + dir).c_str(), nullptr);
    SetFileAttributesW((root + L"\\hidden").c_str(), FILE_ATTRIBUTE_HIDDEN);
    for (const wchar_t* file : {L"x.png", L"y.txt", L"Z.JPG"}) {
        HANDLE h = CreateFileW((root + L"\\" + file).c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, "1234", 4, &written, nullptr);
            CloseHandle(h);
        }
    }
    return root;
}

void RemoveFixture(const std::wstring& root) {
    SetFileAttributesW((root + L"\\hidden").c_str(), FILE_ATTRIBUTE_NORMAL);
    for (const wchar_t* file : {L"x.png", L"y.txt", L"Z.JPG"})
        DeleteFileW((root + L"\\" + file).c_str());
    for (const wchar_t* dir : {L"b", L"a10", L"a2", L"hidden", L"empty"})
        RemoveDirectoryW((root + L"\\" + dir).c_str());
    RemoveDirectoryW(root.c_str());
}

void TestReader() {
    const std::wstring root = MakeFixture();
    const PickerListing folders = ReadPickerListing(root, PickerMode::Folder);
    Check(folders.error == ERROR_SUCCESS &&
              Names(folders.entries) == std::vector<std::wstring>{L"a2", L"a10", L"b", L"empty"},
          "folder listing: visible folders only, natural order");
    Check(!folders.entries.empty() && folders.entries[0].path == root + L"\\a2",
          "entries carry full paths");
    const PickerListing images = ReadPickerListing(root, PickerMode::Image);
    Check(images.error == ERROR_SUCCESS &&
              Names(images.entries) ==
                  std::vector<std::wstring>{L"a2", L"a10", L"b", L"empty", L"x.png", L"Z.JPG"},
          "picture listing adds pictures after folders, skips other files");
    Check(images.entries.size() == 6 && images.entries[4].size == 4 &&
              images.entries[4].kind == PickerEntryKind::Image,
          "pictures carry their size");
    const PickerListing empty = ReadPickerListing(root + L"\\empty", PickerMode::Folder);
    Check(empty.error == ERROR_SUCCESS && empty.entries.empty(), "empty folder is not an error");
    const PickerListing missing = ReadPickerListing(root + L"\\missing", PickerMode::Folder);
    Check(missing.error == ERROR_FILE_NOT_FOUND || missing.error == ERROR_PATH_NOT_FOUND,
          "missing folder reports not found");
    const PickerListing pc = ReadPickerListing(L"", PickerMode::Folder);
    bool has_system = false;
    for (const PickerEntry& e : pc.entries) {
        wchar_t windows[MAX_PATH]{};
        GetWindowsDirectoryW(windows, MAX_PATH);
        if (e.kind == PickerEntryKind::Drive && e.path.size() == 3 &&
            towupper(e.path[0]) == towupper(windows[0]) && e.size > 0 && e.free <= e.size)
            has_system = true;
    }
    Check(pc.error == ERROR_SUCCESS && has_system, "This PC lists the system drive with its size");

    // The loader posts back to a window and drops results once destroyed.
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    constexpr UINT kMessage = WM_APP + 7;
    auto loader = std::make_unique<PickerLoader>(window, kMessage);
    loader->Load(42, root, PickerMode::Folder);
    std::unique_ptr<PickerListing> received;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (!received && GetTickCount64() < deadline) {
        MSG msg{};
        if (PeekMessageW(&msg, window, kMessage, kMessage, PM_REMOVE)) {
            received = PickerLoader::Take(msg.lParam);
        } else {
            Sleep(5);
        }
    }
    Check(received && received->generation == 42 && received->entries.size() == 4,
          "loader posts the listing with its generation");
    loader->Load(43, root, PickerMode::Folder);
    loader.reset();
    Sleep(300);
    MSG late{};
    int late_count = 0;
    while (PeekMessageW(&late, window, kMessage, kMessage, PM_REMOVE)) {
        PickerLoader::Take(late.lParam);
        ++late_count;
    }
    Check(late_count <= 1, "nothing is posted after the loader is gone (beyond a queued one)");
    DestroyWindow(window);
    RemoveFixture(root);
    Check(GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES, "fixture removed");
}

} // namespace

int main(int argc, char** argv) {
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (!(argc == 2 && std::string(argv[1]) == "--picker")) TestConfirm();
    TestPickerModel();
    TestReader();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
