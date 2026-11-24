// Drive only the popup owned by this test's picker, and use the application's
// OnTypeChange getter rather than guessing selection from the painted text.
bool StepFileType(HWND window, HWND edit, DWORD ui_thread, DWORD ui_pid, UINT key, UINT expected) {
    const auto start = changed_filter.load();
    const auto send = [](HWND target, UINT message, WPARAM key) {
        DWORD_PTR ignored = 0;
        return SendMessageTimeoutW(target, message, key, 0, SMTO_ABORTIFHUNG, 1000, &ignored) != 0;
    };
    // The filter occupies the rest of the filename row. Use real client
    // coordinates, so repeated opens don't depend on a prior Tab focus or
    // on the STA finishing its focus restoration while handling a sent key.
    RECT field{}, client{};
    if (!GetWindowRect(edit, &field) || !GetClientRect(window, &client)) return false;
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&field), 2);
    const POINT anchor{(field.right + client.right) / 2, (field.top + field.bottom) / 2};
    if (!PtInRect(&client, anchor)) return false;
    const LPARAM point = MAKELPARAM(anchor.x, anchor.y);
    if (!PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point) ||
        !PostMessageW(window, WM_LBUTTONUP, 0, point)) return false;
    HWND menu = nullptr;
    for (unsigned i = 0; i < 200 && !menu && IsWindow(window); ++i) {
        EnumThreadWindows(ui_thread, [](HWND candidate, LPARAM arg) -> BOOL {
            wchar_t name[64]{};
            GetClassNameW(candidate, name, ARRAYSIZE(name));
            if (IsWindowVisible(candidate) && std::wstring_view(name) == L"PulseFluentMenu") {
                *reinterpret_cast<HWND*>(arg) = candidate;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&menu));
        if (!menu) Sleep(10);
    }
    DWORD menu_pid = 0;
    if (!menu || GetWindowThreadProcessId(menu, &menu_pid) != ui_thread || menu_pid != ui_pid) {
        std::printf("FILTER_DRIVER own menu not found, start=%u key=%u\n", start, key);
        return false;
    }
    if ((key && !PostMessageW(menu, WM_KEYDOWN, key, 0)) || !PostMessageW(menu, WM_KEYDOWN, VK_RETURN, 0)) return false;
    for (unsigned i = 0; i < 200 && IsWindow(window); ++i) {
        if ((!key || changed_filter.load() != start) && !IsWindowVisible(menu)) break;
        Sleep(10);
    }
    if (!send(window, WM_NULL, 0)) return false;
    std::printf("FILTER_KEYBOARD start=%u key=%u observed=%u expected=%u\n", start, key, changed_filter.load(), expected);
    return !IsWindowVisible(menu) && changed_filter.load() == expected;
}
