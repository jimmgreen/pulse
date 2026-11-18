#include "../common/dialog_admin_trust.cpp"
#include <cstdio>
int main() {
    int failures = 0;
    auto check = [&](const wchar_t *sddl, bool ancestor, bool accepted, const char *name) {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        bool pass =
            ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr) != FALSE;
        if (pass) {
            PSID owner = nullptr;
            PACL acl = nullptr;
            BOOL ignored = FALSE, present = FALSE;
            pass = GetSecurityDescriptorOwner(descriptor, &owner, &ignored) &&
                   GetSecurityDescriptorDacl(descriptor, &present, &acl, &ignored);
            if (pass)
                pass = (pulse::dialog_admin::CheckAcl(owner, present ? acl : nullptr, ancestor) == ERROR_SUCCESS) ==
                       accepted;
            LocalFree(descriptor);
        }
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
        if (!pass)
            ++failures;
    };
    check(L"O:BAG:BAD:P(A;;FA;;;BA)(A;;FA;;;SY)(A;;FRFX;;;BU)", false, true, "admin write and user read");
    check(L"O:BAG:BAD:P(A;;FA;;;BA)(A;;FW;;;BU)", false, false, "user write rejected");
    check(L"O:BAG:BAD:P(A;;0x00000002;;;BU)", true, true, "ancestor sibling creation allowed");
    check(L"O:BAG:BAD:P(A;;0x00000040;;;BU)", true, false, "ancestor delete child rejected");
    check(L"O:BAG:BAD:P(A;;0x00040000;;;BU)", false, false, "user ACL write rejected");
    check(L"O:BUG:BAD:P(A;;FA;;;BA)", false, false, "untrusted owner rejected");
    check(L"O:BAG:BAD:NO_ACCESS_CONTROL", false, false, "null DACL rejected");
    const bool invalid = pulse::dialog_admin::VerifyProtectedPath(L"relative.dll", false) == ERROR_INVALID_NAME;
    std::printf("[%s] relative path rejected\n", invalid ? "PASS" : "FAIL");
    if (!invalid)
        ++failures;
    wchar_t own_path[32768]{};
    if (GetModuleFileNameW(nullptr, own_path, 32768)) {
        const DWORD own = pulse::dialog_admin::VerifyProtectedPath(own_path, false);
        std::printf("[%s] user-writable test binary rejected (error=%lu)\n", own != ERROR_SUCCESS ? "PASS" : "FAIL",
                    own);
        if (!own)
            ++failures;
    } else
        ++failures;
    wchar_t program_files[32768]{};
    if (GetEnvironmentVariableW(L"ProgramFiles", program_files, 32768))
        std::printf("[INFO] ProgramFiles protected path error=%lu\n",
                    pulse::dialog_admin::VerifyProtectedPath(program_files, true));
    return failures ? 1 : 0;
}
