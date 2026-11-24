#include "dialog_api_probe.h"

#include "../common/localization.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <array>
#include <cwchar>
#include <new>
#include <string>

namespace pulse::app {
namespace {
using I = l10n::StringId;
using Microsoft::WRL::ComPtr;
using Test = DialogApiScenario;
enum class Surface { Unknown, Pulse, Windows };
constexpr DWORD kTag = 701, kNote = 702, kEncoding = 703, kOptions = 704, kEncodingLabel = 705;
constexpr DWORD kNormal = 1, kCompact = 2;

const wchar_t* Text(I id) { return l10n::Get(id).c_str(); }

struct Report {
    HRESULT code = E_UNEXPECTED;
    const wchar_t* step = L"";
    Surface surface = Surface::Unknown;
    UINT type = 0;
    DWORD mode = 0;
    BOOL tag = FALSE;
    unsigned type_changes = 0, control_changes = 0, vetoes = 0;
    bool options_read = false;
};

HRESULT SetDependentControls(IFileDialogCustomize* controls, UINT type, BOOL tag) {
    const HRESULT mode = controls->SetControlState(kEncoding, type == 2 ? CDCS_ENABLEDVISIBLE : CDCS_VISIBLE);
    return FAILED(mode) ? mode : controls->SetControlState(kNote, tag ? CDCS_ENABLEDVISIBLE : CDCS_VISIBLE);
}

class Events final : public IFileDialogEvents, public IFileDialogControlEvents {
public:
    explicit Events(bool options) : options_(options) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IFileDialogEvents) *out = static_cast<IFileDialogEvents*>(this);
        else if (iid == IID_IFileDialogControlEvents && options_) *out = static_cast<IFileDialogControlEvents*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = InterlockedDecrement(&refs_);
        if (!left) delete this;
        return static_cast<ULONG>(left);
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        const HWND window = Observe(dialog);
        if (!options_) return S_OK;
        ComPtr<IFileDialogCustomize> controls;
        BOOL tag = FALSE;
        HRESULT hr = dialog->QueryInterface(IID_PPV_ARGS(&controls));
        if (SUCCEEDED(hr)) hr = controls->GetCheckButtonState(kTag, &tag);
        if (FAILED(hr)) { update_error = hr; return S_FALSE; }
        if (!tag) return S_OK;
        PWSTR note = nullptr;
        hr = controls->GetEditBoxText(kNote, &note);
        const bool valid = SUCCEEDED(hr) && note && note[std::wcsspn(note, L" \t\r\n")];
        CoTaskMemFree(note);
        if (valid) return S_OK;
        if (FAILED(hr)) update_error = hr;
        ++vetoes;
        MessageBoxW(window, Text(I::DialogApiTestTagRequired), Text(I::DialogApiTestTitle), MB_OK | MB_ICONINFORMATION);
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog* dialog, IShellItem*) override { Observe(dialog); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override { Observe(dialog); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog* dialog) override { Observe(dialog); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override { *response = FDESVR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override { *response = FDEOR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog* dialog) override {
        Observe(dialog);
        if (!options_) return S_OK;
        ++type_changes;
        stage_type = 1;
        BOOL tag = FALSE;
        ComPtr<IFileDialogCustomize> controls;
        HRESULT hr = dialog->GetFileTypeIndex(&stage_type);
        if (SUCCEEDED(hr)) hr = dialog->QueryInterface(IID_PPV_ARGS(&controls));
        if (SUCCEEDED(hr)) hr = controls->GetCheckButtonState(kTag, &tag);
        if (SUCCEEDED(hr)) hr = SetDependentControls(controls.Get(), stage_type, tag);
        if (FAILED(hr)) update_error = hr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnItemSelected(IFileDialogCustomize*, DWORD id, DWORD) override { if (id == kEncoding) ++control_changes; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnButtonClicked(IFileDialogCustomize*, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(IFileDialogCustomize* controls, DWORD id, BOOL checked) override {
        if (id == kTag) {
            ++control_changes;
            const auto hr = SetDependentControls(controls, stage_type, checked);
            if (FAILED(hr)) update_error = hr;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnControlActivating(IFileDialogCustomize*, DWORD) override { return S_OK; }

    Surface surface = Surface::Unknown;
    unsigned type_changes = 0, control_changes = 0, vetoes = 0;
    HRESULT update_error = S_OK;

private:
    HWND Observe(IFileDialog* dialog) {
        HWND hwnd = nullptr;
        ComPtr<IOleWindow> window;
        if (SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&window))) && SUCCEEDED(window->GetWindow(&hwnd))) {
            wchar_t kind[64]{};
            if (GetClassNameW(hwnd, kind, ARRAYSIZE(kind))) {
                if (std::wcscmp(kind, L"PulseFolderPickerWindow") == 0) surface = Surface::Pulse;
                else if (std::wcscmp(kind, L"#32770") == 0) surface = Surface::Windows;
            }
        }
        return hwnd;
    }
    LONG refs_ = 1;
    UINT stage_type = 1;
    bool options_;
};

HRESULT CreateSaveOptions(IFileDialogCustomize* controls) {
    HRESULT hr = controls->StartVisualGroup(kOptions, Text(I::DialogApiTestOptions));
    if (SUCCEEDED(hr)) hr = controls->AddText(kEncodingLabel, Text(I::DialogApiTestMode));
    if (SUCCEEDED(hr)) hr = controls->AddComboBox(kEncoding);
    if (SUCCEEDED(hr)) hr = controls->AddControlItem(kEncoding, kNormal, Text(I::DialogApiTestNormal));
    if (SUCCEEDED(hr)) hr = controls->AddControlItem(kEncoding, kCompact, Text(I::DialogApiTestCompact));
    if (SUCCEEDED(hr)) hr = controls->SetSelectedControlItem(kEncoding, kNormal);
    if (SUCCEEDED(hr)) hr = controls->AddCheckButton(kTag, Text(I::DialogApiTestTag), FALSE);
    if (SUCCEEDED(hr)) hr = controls->AddEditBox(kNote, L"");
    if (SUCCEEDED(hr)) hr = controls->EndVisualGroup();
    if (SUCCEEDED(hr)) hr = SetDependentControls(controls, 1, FALSE);
    return hr;
}

Report RunCommonItem(Test test) {
    Report report;
    const bool save = test == Test::Save, folder = test == Test::Folder;
    ComPtr<IFileDialog> dialog;
    report.step = L"CoCreateInstance";
    report.code = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(report.code)) return report;
    constexpr DWORD common = FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT | FOS_PATHMUSTEXIST;
    report.step = L"IFileDialog::SetOptions";
    // Only select a path. Do not ask the dialog to create a file access probe.
    report.code = dialog->SetOptions(common | (save ? FOS_NOTESTFILECREATE | FOS_OVERWRITEPROMPT : folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (FAILED(report.code)) return report;
    report.step = L"IFileDialog::SetTitle";
    report.code = dialog->SetTitle(Text(save ? I::DialogApiTestSave : folder ? I::DialogApiTestFolder : I::DialogApiTestOpen));
    if (FAILED(report.code)) return report;
    ComPtr<IFileDialogCustomize> controls;
    if (save) {
        const COMDLG_FILTERSPEC filters[]{{L"TXT (*.txt)", L"*.txt"}, {L"JSON (*.json)", L"*.json"}};
        report.step = L"IFileDialog::SetFileTypes";
        report.code = dialog->SetFileTypes(ARRAYSIZE(filters), filters);
        if (SUCCEEDED(report.code)) report.code = dialog->SetFileTypeIndex(1);
        if (SUCCEEDED(report.code)) report.code = dialog->SetDefaultExtension(L"txt");
        if (SUCCEEDED(report.code)) report.code = dialog->SetFileName(L"Pulse-Dialog-Test");
        if (FAILED(report.code)) return report;
        report.step = L"IFileDialogCustomize";
        report.code = dialog.As(&controls);
        if (SUCCEEDED(report.code)) report.code = CreateSaveOptions(controls.Get());
        if (FAILED(report.code)) return report;
    }
    auto* events = new (std::nothrow) Events(save);
    if (!events) { report.code = E_OUTOFMEMORY; return report; }
    ComPtr<IFileDialogEvents> keep_events;
    keep_events.Attach(events);
    DWORD cookie = 0;
    report.step = L"IFileDialog::Advise";
    report.code = dialog->Advise(events, &cookie);
    if (FAILED(report.code)) return report;
    report.step = L"IFileDialog::Show";
    report.code = dialog->Show(nullptr);
    dialog->Unadvise(cookie);
    report.surface = events->surface;
    report.type_changes = events->type_changes;
    report.control_changes = events->control_changes;
    report.vetoes = events->vetoes;
    if (FAILED(report.code)) return report;
    report.step = L"IFileDialog::GetResult";
    ComPtr<IShellItem> item;
    report.code = dialog->GetResult(&item);
    if (FAILED(report.code) || !item) { if (SUCCEEDED(report.code)) report.code = E_UNEXPECTED; return report; }
    PWSTR path = nullptr;
    report.code = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    const bool has_path = path && *path;
    CoTaskMemFree(path);
    if (FAILED(report.code) || !has_path) { if (SUCCEEDED(report.code)) report.code = E_UNEXPECTED; return report; }
    if (save) {
        report.step = L"IFileDialogCustomize: read-back";
        report.code = dialog->GetFileTypeIndex(&report.type);
        if (SUCCEEDED(report.code)) report.code = controls->GetSelectedControlItem(kEncoding, &report.mode);
        if (SUCCEEDED(report.code)) report.code = controls->GetCheckButtonState(kTag, &report.tag);
        PWSTR note = nullptr;
        if (SUCCEEDED(report.code) && report.tag) report.code = controls->GetEditBoxText(kNote, &note);
        CoTaskMemFree(note);
        report.options_read = SUCCEEDED(report.code);
        if (SUCCEEDED(report.code) && FAILED(events->update_error)) {
            report.step = L"IFileDialogCustomize::SetControlState";
            report.code = events->update_error;
        }
    }
    return report;
}

Report RunLegacy() {
    wchar_t name[1024] = L"Pulse-Dialog-Test.txt";
    OPENFILENAMEW input{};
    input.lStructSize = OPENFILENAME_SIZE_VERSION_400W;
    input.lpstrFile = name;
    input.nMaxFile = ARRAYSIZE(name);
    input.lpstrTitle = Text(I::DialogApiTestLegacy);
    input.lpstrFilter = L"TXT (*.txt)\0*.txt\0\0";
    input.nFilterIndex = 1;
    input.lpstrDefExt = L"txt";
    input.Flags = OFN_NOTESTFILECREATE | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    Report report;
    report.step = L"GetSaveFileNameW / OPENFILENAME v4";
    if (GetSaveFileNameW(&input)) report.code = *name ? S_OK : E_UNEXPECTED;
    else if (const DWORD error = CommDlgExtendedError()) report.code = HRESULT_FROM_WIN32(error);
    else report.code = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    return report;
}

void ShowReport(const Report& report, const DialogApiResultPresenter& present = {}) {
    const bool cancelled = report.code == HRESULT_FROM_WIN32(ERROR_CANCELLED);
    std::wstring text = l10n::Get(cancelled ? I::DialogApiTestCancelled : SUCCEEDED(report.code) ? I::DialogApiTestSucceeded : I::DialogApiTestFailed);
    if (!cancelled && FAILED(report.code)) {
        wchar_t error[24]{};
        swprintf_s(error, L"0x%08lX", static_cast<unsigned long>(report.code));
        text += L"\n" + std::wstring(report.step) + L": " + error;
    }
    text += L"\n" + l10n::Get(report.surface == Surface::Pulse ? I::DialogApiTestPulseUi : report.surface == Surface::Windows ? I::DialogApiTestWindowsUi : I::DialogApiTestUnknownUi);
    if (report.options_read) {
        text += L"\n\n" + l10n::Get(I::DialogApiTestReadback) + L": " + (report.type == 2 ? L"JSON" : L"TXT") + L" · " +
            l10n::Get(report.mode == kCompact ? I::DialogApiTestCompact : I::DialogApiTestNormal) + L" · " +
            l10n::Get(report.tag ? I::DialogApiTestTagOn : I::DialogApiTestTagOff);
    }
    if (report.options_read || report.type_changes || report.control_changes || report.vetoes) {
        text += L"\n" + l10n::Get(I::DialogApiTestTypeEvents) + L": " + std::to_wstring(report.type_changes);
        text += L"\n" + l10n::Get(I::DialogApiTestControlEvents) + L": " + std::to_wstring(report.control_changes);
        text += L"\n" + l10n::Get(I::DialogApiTestVetoes) + L": " + std::to_wstring(report.vetoes);
    }
    if (present) present(text, FAILED(report.code) && !cancelled);
    else MessageBoxW(nullptr, text.c_str(), Text(I::DialogApiTestTitle), MB_OK | (FAILED(report.code) && !cancelled ? MB_ICONWARNING : MB_ICONINFORMATION));
}
}

DWORD LaunchDialogApiProbe(HANDLE& process, DialogApiScenario scenario, bool dark) {
    std::wstring executable(32768, L'\0');
    const auto units = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!units) return GetLastError();
    if (units >= executable.size()) return ERROR_FILENAME_EXCED_RANGE;
    executable.resize(units);
    const auto index=static_cast<unsigned>(scenario)-static_cast<unsigned>(Test::Open);
    const wchar_t* names[]{L"open",L"save",L"folder",L"legacy"};
    if(index>=ARRAYSIZE(names)) return ERROR_INVALID_PARAMETER;
    auto command = L"\"" + executable + L"\" " + kDialogApiProbeSwitch + L" " + l10n::LocaleName() + L" " + names[index] + (dark ? L" dark" : L" light");
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) return GetLastError();
    CloseHandle(info.hThread);
    process = info.hProcess;
    return ERROR_SUCCESS;
}

int RunDialogApiProbe(std::wstring_view scenario, const DialogApiResultPresenter& present) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { ShowReport({initialized, L"CoInitializeEx"},present); return 1; }
    struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
    if (!scenario.empty()) {
        if (scenario==L"legacy") ShowReport(RunLegacy(),present);
        else if (scenario==L"open" || scenario==L"save" || scenario==L"folder")
            ShowReport(RunCommonItem(scenario==L"save" ? Test::Save : scenario==L"folder" ? Test::Folder : Test::Open),present);
        else return 2;
        return 0;
    }
    const TASKDIALOG_BUTTON buttons[] = {
        {static_cast<int>(Test::Open), Text(I::DialogApiTestOpen)}, {static_cast<int>(Test::Save), Text(I::DialogApiTestSave)},
        {static_cast<int>(Test::Folder), Text(I::DialogApiTestFolder)}, {static_cast<int>(Test::Legacy), Text(I::DialogApiTestLegacy)},
        {IDCANCEL, Text(I::Cancel)},
    };
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_USE_COMMAND_LINKS_NO_ICON | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = Text(I::DialogApiTestTitle);
    config.pszMainInstruction = Text(I::DialogApiTestTitle);
    config.pszContent = Text(I::DialogApiTestInstructions);
    config.cButtons = ARRAYSIZE(buttons);
    config.pButtons = buttons;
    for (;;) {
        int choice = IDCANCEL;
        const auto shown = TaskDialogIndirect(&config, &choice, nullptr, nullptr);
        if (FAILED(shown)) { ShowReport({shown, L"TaskDialogIndirect"}); return 1; }
        if (choice == IDCANCEL) return 0;
        const auto test = static_cast<Test>(choice);
        ShowReport(test == Test::Legacy ? RunLegacy() : RunCommonItem(test));
    }
}

}
