// Included by the runtime-entry fixture; exercise only windows on this thread.
namespace legacy_open_save_compat {
struct CaseState {
    std::wstring root, title, target;
    unsigned ticks = 0, shown_at = 0;
    bool cancel = false;
    bool pulse = false, native = false, explorer = false, cwd_in_folder = false;
    bool filename_set = false, readonly_visible = false;
};
CaseState* active_case = nullptr;
unsigned original_fallback_calls = 0;

template<class Ofn> BOOL WINAPI ExactNativeFallback(Ofn* request) {
    ++original_fallback_calls;
    if (request->nFileOffset != 0 || request->Flags != (OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_FILEMUSTEXIST)) return FALSE;
    request->nFileOffset = 7;
    return TRUE;
}

void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD) {
    auto& state = *active_case;
    ++state.ticks;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
        auto& s = *active_case;
        if (!IsWindowVisible(window) || !IsWindowEnabled(window)) return TRUE;
        wchar_t title[128]{}, cls[64]{};
        GetWindowTextW(window, title, ARRAYSIZE(title));
        GetClassNameW(window, cls, ARRAYSIZE(cls));
        if (s.title != title) return TRUE;
        const bool pulse = wcscmp(cls, L"PulseFolderPickerWindow") == 0;
        const bool native = wcscmp(cls, L"#32770") == 0;
        if (!pulse && !native) return TRUE;
        s.pulse |= pulse;
        s.native |= native;
        if (!s.shown_at) s.shown_at = s.ticks;
        if (s.ticks - s.shown_at < 3) return TRUE;
        if (native) {
            s.readonly_visible |= IsWindowVisible(GetDlgItem(window, chx1)) != FALSE;
            wchar_t folder[32768]{};
            s.explorer |= SendMessageW(window, CDM_GETFOLDERPATH, ARRAYSIZE(folder), reinterpret_cast<LPARAM>(folder)) > 0 && s.root == folder;
            EnumChildWindows(window, [](HWND child, LPARAM value) -> BOOL {
                auto& seen = *reinterpret_cast<CaseState*>(value);
                wchar_t name[64]{}; GetClassNameW(child, name, ARRAYSIZE(name));
                seen.explorer |= wcscmp(name, L"SHELLDLL_DefView") == 0 || wcscmp(name, L"DirectUIHWND") == 0;
                return TRUE;
            }, reinterpret_cast<LPARAM>(&s));
            if (!s.cancel && !s.filename_set) {
                // A shell dialog may restore another location. Enter the test's
                // full path rather than relying on the remembered name edit.
                const HWND combo = GetDlgItem(window, cmb13);
                const HWND edit = combo ? FindWindowExW(combo, nullptr, L"Edit", nullptr) : nullptr;
                if (edit) SetWindowTextW(edit, s.target.c_str());
                else SendMessageW(window, CDM_SETCONTROLTEXT, edt1, reinterpret_cast<LPARAM>(s.target.c_str()));
                s.filename_set = true;
                return TRUE;
            }
        }
        s.cwd_in_folder |= std::filesystem::current_path() == s.root;
        if (s.cancel || s.ticks - s.shown_at > 65) PostMessageW(window, WM_CLOSE, 0, 0);
        else if (pulse) PostMessageW(window, WM_COMMAND, IDOK, 0);
        else if (const HWND button = GetDlgItem(window, IDOK); button && IsWindowEnabled(button)) {
            SetActiveWindow(window);
            PostMessageW(button, BM_CLICK, 0, 0);
        }
        else PostMessageW(window, WM_COMMAND, IDOK, 0);
        return TRUE;
    }, 0);
}

template<class Ofn> bool OneCase(const std::filesystem::path& folder, const std::filesystem::path& before,
    bool ui, bool save, bool cancel, unsigned variant) {
    using C = std::remove_pointer_t<decltype(Ofn{}.lpstrFile)>;
    constexpr bool unicode = std::is_same_v<C, wchar_t>;
    const auto convert = [](const std::wstring& wide) {
        if constexpr (unicode) return wide;
        else return std::filesystem::path(wide).string();
    };
    CaseState state{folder.wstring(), L"Pulse open-save contract fixture",
        (folder / (save ? L"export.txt" : L"probe.txt")).wstring(), 0, 0, cancel};
    const auto leaf = convert(save ? L"export.txt" : L"probe.txt");
    const auto expected = convert((folder / (save ? L"export.txt" : L"probe.txt")).wstring());
    const auto title = convert(state.title);
    const auto initial = convert(folder.wstring());
    const auto extension = convert(L"txt");
    const C* filter = nullptr;
    if constexpr (unicode) filter = L"Text\0*.txt\0\0";
    else filter = "Text\0*.txt\0\0";
    std::array<C, 1024> name{};
    std::array<C, 64> file_title{};
    const auto& initial_name = variant == 7 ? leaf : expected;
    std::copy(initial_name.begin(), initial_name.end(), name.begin());
    const DWORD short_size = unicode ? OPENFILENAME_SIZE_VERSION_400W : OPENFILENAME_SIZE_VERSION_400A;
    const DWORD length = variant == 1 ? short_size : static_cast<DWORD>(sizeof(Ofn));
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const size_t page = info.dwPageSize;
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!memory) return false;
    DWORD prev = 0;
    if (!VirtualProtect(memory + page, page, PAGE_NOACCESS, &prev)) { VirtualFree(memory, 0, MEM_RELEASE); return false; }
    // Match the caller's actual advertised allocation; reading a v5 tail for
    // the v4 packet would hit the next, inaccessible page.
    auto* input = reinterpret_cast<Ofn*>(memory + page - length);
    Ofn data{};
    data.lStructSize = length;
    data.lpstrFile = name.data(); data.nMaxFile = static_cast<DWORD>(name.size());
    data.lpstrFileTitle = file_title.data(); data.nMaxFileTitle = static_cast<DWORD>(file_title.size());
    data.lpstrTitle = title.c_str(); data.lpstrInitialDir = initial.c_str();
    data.lpstrFilter = filter; data.nFilterIndex = 1; data.lpstrDefExt = extension.c_str();
    data.Flags = OFN_DONTADDTORECENT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
        (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (variant == 2 || variant == 3) data.Flags &= ~OFN_HIDEREADONLY;
    if (variant == 3 || variant == 6) data.Flags |= OFN_READONLY;
    if (variant == 4 || variant == 7) data.Flags &= ~OFN_NOCHANGEDIR;
    if (variant == 7) data.nMaxFile = 12;
    if (variant == 5) data.nMaxCustFilter = 128; // No custom-filter buffer.
    std::memcpy(input, &data, length);
    std::filesystem::current_path(before);
    active_case = &state;
    const UINT_PTR timer = SetTimer(nullptr, 0, 120, Drive);
    BOOL accepted = FALSE;
    if (timer) {
        if constexpr (unicode) accepted = ui ? (save ? ObserveSaveW(input) : ObserveOpenW(input)) : (save ? GetSaveFileNameW(input) : GetOpenFileNameW(input));
        else accepted = ui ? (save ? ObserveSaveA(input) : ObserveOpenA(input)) : (save ? GetSaveFileNameA(input) : GetOpenFileNameA(input));
        KillTimer(nullptr, timer);
    }
    active_case = nullptr;
    const DWORD error = ui ? shell::legacy_dialog_error::Query(&CommDlgExtendedError) : CommDlgExtendedError();
    const bool cwd_before = std::filesystem::current_path() == before;
    const bool cwd_selected = std::filesystem::current_path() == folder;
    WORD required = 0;
    std::memcpy(&required, name.data(), sizeof(required));
    const bool short_output = variant == 7 && !cancel;
    const bool correct_output = short_output ? required == expected.size() + 1 :
        cancel ? std::basic_string<C>(name.data()) == initial_name && !input->nFileOffset && !input->nFileExtension :
        std::basic_string<C>(name.data()) == expected && std::basic_string<C>(file_title.data()) == leaf &&
        input->nFileOffset == expected.size() - leaf.size() && input->nFileExtension == expected.size() - 3;
    // Microsoft documents NOCHANGEDIR as ineffective on native Open. This
    // expansion leaves Pulse's existing Open directory policy unchanged.
    const bool cwd_ok = ((variant == 4 || variant == 7) && !cancel ? cwd_selected : (!ui && !save && variant < 2) || cwd_before) &&
        (variant < 2 || state.cwd_in_folder);
    const bool flags_ok = variant < 2 || (!state.readonly_visible && !(input->Flags & OFN_READONLY));
    const bool result_ok = short_output ? !accepted && error == FNERR_BUFFERTOOSMALL : !error && (!!accepted != cancel);
    const bool original_expected = !ui || (save && !unicode && variant == 3);
    const bool ok = timer && result_ok && correct_output && cwd_ok && flags_ok &&
        (original_expected ? (state.native && !state.pulse && state.explorer) : (state.pulse && !state.native));
    std::printf("LEGACY_CASE op=%s charset=%s variant=%u cancel=%u accepted=%u error=%lu explorer=%u pulse=%u original=%u cwd_during=%u cwd_before=%u cwd_selected=%u output=%u pass=%u\n",
        save ? "save" : "open", unicode ? "W" : "A", variant, cancel, !!accepted, error,
        state.explorer, state.pulse, state.native, state.cwd_in_folder, cwd_before, cwd_selected, correct_output, ok);
    if (variant >= 2) std::printf("FLAGS visible_readonly=%u readonly_before=%u readonly_after=%u cwd_before=%u cwd_selected=%u\n",
        state.readonly_visible, !!(data.Flags & OFN_READONLY), !!(input->Flags & OFN_READONLY), cwd_before, cwd_selected);
    VirtualFree(memory, 0, MEM_RELEASE);
    return ok;
}

int Run(bool ui, bool flag_probe = false, bool interactive = false, bool ansi_fallback_only = false) {
    if (flag_probe && !interactive) {
        const auto desktop = CreateDesktopW((L"PulseLegacyFlags-" + std::to_wstring(GetCurrentProcessId())).c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop || !SetThreadDesktop(desktop)) { std::printf("private desktop error=%lu\n", GetLastError()); return 2; }
    }
    const auto cwd = std::filesystem::current_path();
    const auto root = cwd / L"bench_data" / (L"open-save-api-" + std::to_wstring(GetCurrentProcessId()));
    const auto before = root / L"before", folder = root / L"selected", probe = folder / L"probe.txt";
    if (!std::filesystem::create_directory(root) || !std::filesystem::create_directory(before) || !std::filesystem::create_directory(folder)) return 2;
    std::ofstream(probe) << "probe";
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", root.c_str());
    Configuration config;
    unsigned case_count = 0, failed = 0;
    if (ui) {
        wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        config.host = (std::filesystem::path(path).parent_path() / L"pulse.exe").wstring();
        config.control = root.wstring(); config.routing_enabled = true;
        current = &config;
        legacy_test_services = [](shell::FileDialogBridgeServices& services) {
            services.validation_verified = [](const auto& decision) {
                return shell::RuntimeContractCertified(decision) || shell::RuntimeLocalSaveContractCertified(decision);
            };
            services.on_native_reason = [](const char* reason, const IID*) { std::printf("LEGACY_ROUTE %s\n", reason); };
        };
    }
    if (ui && !flag_probe) {
        const auto saved_services = legacy_test_services;
        unsigned fallback_observations = 0;
        legacy_test_services = [&](shell::FileDialogBridgeServices& services) {
            services.inprocess_provider.reset();
            services.load_inprocess_provider = {};
            services.executable.clear();
            services.observe_show = [&](bool begin, const shell::FileDialogShowObservation& observation) {
                if (!begin && observation.native_fallback && !observation.provider_started) ++fallback_observations;
            };
        };
        wchar_t wide[128] = L"export.txt"; char ansi[128] = "export.txt";
        OPENFILENAMEW w{}; OPENFILENAMEA a{};
        const auto narrow_folder = folder.string();
        w.lStructSize = sizeof(w); w.lpstrFile = wide; w.nMaxFile = ARRAYSIZE(wide); w.lpstrInitialDir = folder.c_str();
        a.lStructSize = sizeof(a); a.lpstrFile = ansi; a.nMaxFile = ARRAYSIZE(ansi); a.lpstrInitialDir = narrow_folder.c_str();
        w.Flags = a.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_FILEMUSTEXIST;
        const auto open_w = original_open_w, save_w = original_save_w;
        const auto open_a = original_open_a, save_a = original_save_a;
        original_open_w = original_save_w = ExactNativeFallback<OPENFILENAMEW>;
        original_open_a = original_save_a = ExactNativeFallback<OPENFILENAMEA>;
        const bool accepted_open_w = ObserveOpenW(&w) && w.nFileOffset == 7;
        const bool accepted_open_a = ObserveOpenA(&a) && a.nFileOffset == 7;
        w.nFileOffset = a.nFileOffset = 0;
        const bool accepted_save_w = ObserveSaveW(&w) && w.nFileOffset == 7;
        const bool accepted_save_a = ObserveSaveA(&a) && a.nFileOffset == 7;
        original_open_w = open_w; original_save_w = save_w;
        original_open_a = open_a; original_save_a = save_a;
        ++case_count;
        const bool exact = accepted_open_w && accepted_open_a && accepted_save_w && accepted_save_a && original_fallback_calls == 4 && fallback_observations == 4;
        if (!exact) ++failed;
        std::printf("LEGACY_EXACT_FALLBACK open_w=%u open_a=%u save_w=%u save_a=%u original_calls=%u declined_before_ui=%u pass=%u\n",
            accepted_open_w, accepted_open_a, accepted_save_w, accepted_save_a, original_fallback_calls, fallback_observations, exact);
        legacy_test_services = saved_services;
    }
    for (const bool save : {false, true}) for (const bool cancel : {false, true}) {
        for (unsigned variant = flag_probe ? 2u : 0u; variant <= (flag_probe ? 7u : 1u); ++variant) {
            if (flag_probe && !save && variant != 5) continue;
            if (ansi_fallback_only && (!save || variant != 3)) continue;
            if (!ansi_fallback_only) {
                ++case_count; if (!OneCase<OPENFILENAMEW>(folder, before, ui, save, cancel, variant)) ++failed;
            }
            ++case_count; if (!OneCase<OPENFILENAMEA>(folder, before, ui, save, cancel, variant)) ++failed;
        }
    }
    legacy_test_services = {}; current = nullptr;
    std::filesystem::current_path(cwd);
    DeleteFileW(probe.c_str());
    DeleteFileW((root / L"runtime-dialog-shows.jsonl").c_str());
    RemoveDirectoryW(folder.c_str()); RemoveDirectoryW(before.c_str());
    // The provider can leave isolated app state inside this fixture; do not
    // delete outside the fixture or touch the normal profile.
    std::printf("LEGACY_%s checks=%u failures=%u\n", ui ? "UI" : "NATIVE", case_count, failed);
    return failed ? 1 : 0;
}
}
