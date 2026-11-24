bool selection_cycle = false;
std::atomic_int cycle_result{0};
std::filesystem::path cycle_png, cycle_other;
constexpr DWORD kSequenceId = 1020;

HRESULT ApplySelectionCycle(IFileDialog* dialog) {
    ComPtr<IShellItem> item;
    const HRESULT got = dialog->GetCurrentSelection(&item);
    if (FAILED(got) || !item) {
        std::printf("SELECTION empty hr=%08lx\n", static_cast<unsigned long>(got));
        return S_OK; // A folder transition can have no selected file.
    }
    PWSTR display = nullptr;
    const HRESULT named = item->GetDisplayName(SIGDN_FILESYSPATH, &display);
    if (FAILED(named) || !display) return E_FAIL;
    const std::filesystem::path selected(display);
    CoTaskMemFree(display);
    const bool png = selected == cycle_png;
    if (!png && selected != cycle_other) {
        Check(false, "callback selected an unexpected filesystem item");
        return E_UNEXPECTED;
    }
    ComPtr<IFileDialogCustomize> custom;
    HRESULT hr = dialog->QueryInterface(IID_PPV_ARGS(&custom));
    if (FAILED(hr)) return hr;
    const auto state = png ? CDCS_ENABLEDVISIBLE : CDCS_VISIBLE;
    const HRESULT label = custom->SetControlLabel(kSequenceId, png ? L"PNG sequence" : L"Sequence unavailable");
    const HRESULT enabled = custom->SetControlState(kSequenceId, state);
    CDCONTROLSTATEF observed{};
    const HRESULT read = custom->GetControlState(kSequenceId, &observed);
    const bool ok = label == S_OK && enabled == S_OK && read == S_OK && observed == state;
    std::printf("SELECTION file=%s GetCurrentSelection=%08lx SetControlLabel=%08lx SetControlState=%08lx GetControlState=%08lx state=%u\n",
        png ? "01-frame.png" : "02-notes.txt", static_cast<unsigned long>(got), static_cast<unsigned long>(label),
        static_cast<unsigned long>(enabled), static_cast<unsigned long>(read), static_cast<unsigned>(observed));
    Check(ok, "host selection callback updates checkbox state for exact path");
    cycle_result.store(ok ? (png ? 1 : 2) : -1);
    return ok ? S_OK : E_FAIL;
}

void DriveSelectionCycle(HWND window) {
    // Address only this test's live picker HWND; no foreground or global key injection.
    DWORD_PTR ignored = 0;
    const UINT keys[] = {VK_HOME, VK_END, VK_HOME};
    const wchar_t* shots[] = {L"01-png-enabled.png", L"02-other-disabled.png", L"03-png-restored.png"};
    for (size_t step = 0; step < std::size(keys); ++step) {
        const int expected = step == 1 ? 2 : 1;
        for (unsigned attempt = 0; attempt < 100 && cycle_result.load() != expected && cycle_result.load() >= 0; ++attempt) {
            if (!SendMessageTimeoutW(window, WM_KEYDOWN, keys[step], 0, SMTO_ABORTIFHUNG, 1500, &ignored)) break;
            Sleep(20); // Folder enumeration and out-of-process delivery can complete asynchronously.
        }
        if (cycle_result.load() != expected) {
            std::printf("[FAIL] expected selection callback step=%zu actual=%d\n", step + 1, cycle_result.load());
            PostMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
        const std::wstring shot = (snapshot_directory / shots[step]).wstring();
        const auto request = SnapshotRequest(shot);
        if (request && SetPropW(window, L"Pulse.PickerTestSnapshot", request)) {
            SendMessageTimeoutW(window, WM_PAINT, 0, 0, SMTO_ABORTIFHUNG, 3000, &ignored);
            RemovePropW(window, L"Pulse.PickerTestSnapshot");
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(shot, ec) || std::filesystem::file_size(shot, ec) == 0 || ec) {
            std::printf("[FAIL] missing real picker snapshot step=%zu\n", step + 1);
            PostMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
    }
    PostMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
}
