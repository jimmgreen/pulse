#pragma once
// Test-only clipboard/keyboard seams: never change the desktop user's clipboard.
bool calculation_test_input = false;
bool calculation_test_edit_diagnostics = false;
std::wstring calculation_test_clipboard;
HGLOBAL calculation_test_clipboard_memory = nullptr;
SHORT calculation_test_keys[256]{};
BOOL WINAPI CalculationTestOpenClipboard(HWND owner) {
    return calculation_test_input ? TRUE : ::OpenClipboard(owner);
}
BOOL WINAPI CalculationTestEmptyClipboard() {
    if (!calculation_test_input) return ::EmptyClipboard();
    calculation_test_clipboard.clear();
    return TRUE;
}
HANDLE WINAPI CalculationTestSetClipboardData(UINT format, HANDLE data) {
    if (!calculation_test_input) return ::SetClipboardData(format, data);
    if (format != CF_UNICODETEXT) return nullptr;
    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (!text) return nullptr;
    calculation_test_clipboard = text;
    GlobalUnlock(data);
    calculation_test_clipboard_memory = data;
    return data;
}
BOOL WINAPI CalculationTestCloseClipboard() {
    if (!calculation_test_input) return ::CloseClipboard();
    if (calculation_test_clipboard_memory) GlobalFree(calculation_test_clipboard_memory);
    calculation_test_clipboard_memory = nullptr;
    return TRUE;
}
SHORT WINAPI CalculationTestGetKeyState(int key) {
    return calculation_test_input && key >= 0 && key < 256 ? calculation_test_keys[key] : ::GetKeyState(key);
}
bool CalculationTestLogEdit(HWND edit, const std::wstring& expected, const char* stage) {
    if (!calculation_test_input || !calculation_test_edit_diagnostics || expected.find(L"to") == std::wstring::npos) return true;
    wchar_t text[2049]{}; GetWindowTextW(edit, text, 2049);
    DWORD start = 0, end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    RECT bounds{}; GetClientRect(edit, &bounds);
    const auto first = SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    const auto p0 = SendMessageW(edit, EM_POSFROMCHAR, 0, 0), p1 = SendMessageW(edit, EM_POSFROMCHAR, 1, 0);
    std::printf("EDIT stage=%s text=[%ls] expected=[%ls] sel=%lu,%lu first=%lld pos0=%lld pos1=%lld size=%ld,%ld\n",
        stage, text, expected.c_str(), start, end, static_cast<long long>(first),
        static_cast<long long>(p0), static_cast<long long>(p1), bounds.right, bounds.bottom);
    return expected == text;
}
