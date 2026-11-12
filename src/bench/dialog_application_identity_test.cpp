#include "../shell_bridge/dialog_application_identity.h"
#include <iostream>
int main() {
    using namespace pulse::shell_bridge::application_identity;
    unsigned failed = 0;
    auto check = [&](bool ok, const char* name) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; failed += !ok; };
    check(ExecutableBasename(L"C:\\private\\app.exe") == L"app.exe", "basename excludes private directory");
    check(ExecutableBasename(L"bad\napp.exe").empty() && ExecutableBasename(std::wstring(256, L'x')).empty(), "control and overlong names stay unknown");
    check(ExecutableBasename(L"").empty() && ImageHash(L"") == 0, "unknown does not invent identity");
    check(ImageHash(L"C:\\A\\app.exe") != ImageHash(L"C:\\B\\app.exe"), "same basename in separate applications remains distinct");
    check(ImageHash(L"APP.EXE") == ImageHash(L"app.exe"), "ASCII image case remains stable");
    const auto& identity = CurrentApplication();
    check(!identity.basename.empty() && identity.creation_time && identity.image_id && identity.instance_id, "running image supplies verified process identity");
    check(&identity == &CurrentApplication(), "identity queries are cached");
    check(identity.json_basename.find("private") == std::string::npos && identity.json_basename.size() == identity.basename.size() * 6, "JSON encodes basename only");
    return failed ? 1 : 0;
}
