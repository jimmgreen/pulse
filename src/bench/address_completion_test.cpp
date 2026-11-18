#include "../app/address_completion.h"
#include <iostream>
int wmain() {
    pulse::AddressCompletionQuery query;
    int failures = 0;
    auto check = [&](bool value, const char* name) {
        std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!value) ++failures;
    };
    check(pulse::ParseAddressCompletion(L"C:\\Users\\Do", query) && query.parent == L"C:\\Users\\" && query.prefix == L"Do", "absolute prefix");
    check(pulse::ParseAddressCompletion(L"\"C:\\Some Folder\\file.txt\"", query) && query.prefix == L"file.txt", "quoted file");
    check(pulse::ParseAddressCompletion(L"\\\\host\\share\\dir", query) && query.parent == L"\\\\host\\share\\", "UNC prefix");
    check(!pulse::ParseAddressCompletion(L"\\\\host", query), "bare server stays server suggestions");
    check(!pulse::ParseAddressCompletion(L"C:relative\\file", query), "drive relative rejected");
    check(!pulse::ParseAddressCompletion(L"C:\\dir\\*", query), "wildcard rejected");
    check(pulse::ParseAddressCompletion(L"Doc", query, L"C:\\Users\\User\\Desktop") && query.parent == L"C:\\Users\\User\\Desktop\\" && query.prefix == L"Doc", "relative folder prefix uses current directory");
    check(pulse::ParseAddressCompletion(L"Docs\\sub", query, L"C:\\Users\\User\\Desktop") && query.parent == L"C:\\Users\\User\\Desktop\\Docs\\" && query.prefix == L"sub", "relative nested prefix");
    check(!pulse::ParseAddressCompletion(L"Docs", query, L"pulse:recent"), "virtual location has no relative completion");
    pulse::AddressCompletionWorker worker;
    auto cancelled = std::make_shared<pulse::AddressCompletionResult>();
    cancelled->cancelled = true;
    worker.Submit({L"C:\\", L""}, cancelled);
    auto latest = std::make_shared<pulse::AddressCompletionResult>();
    worker.Submit({L"C:\\", L""}, latest);
    bool ready = false;
    for (int i = 0; i < 200 && !ready; ++i) {
        Sleep(10);
        std::lock_guard lock(latest->mutex);
        ready = latest->ready;
    }
    check(ready, "latest request completes");
    { std::lock_guard lock(cancelled->mutex); check(!cancelled->ready, "cancelled result never published"); }
    return failures ? 1 : 0;
}
