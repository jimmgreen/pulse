#include "../common/dialog_admin_trust.cpp"
#include <cstdio>
int wmain(int argc,wchar_t** argv) {
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
    std::wstring control;
    const auto own_root=std::filesystem::path(own_path).parent_path().wstring();
    const bool no_create=pulse::dialog_admin::AdminControlRoot(own_root,false,control)!=ERROR_SUCCESS && control.empty() &&
        !std::filesystem::exists(std::filesystem::path(own_root)/L"ManagedAdminSessions");
    std::printf("[%s] read-only admin readiness rejects user-owned root without creating directory\n",no_create?"PASS":"FAIL");
    if(!no_create) ++failures;
    const auto denied_create=pulse::dialog_admin::AdminControlRoot(own_root,true,control);
    const bool create_guard=denied_create!=ERROR_SUCCESS && control.empty();
    std::printf("[%s] creating admin control root requires elevation and protected ancestry\n",create_guard?"PASS":"FAIL");
    if(!create_guard) ++failures;
    const bool bad_machine=pulse::dialog_admin::VerifyAdminLaunchLocation(own_root,own_root,own_path,0)==ERROR_EXE_MACHINE_TYPE_MISMATCH;
    std::printf("[%s] unknown admin target machine rejected\n",bad_machine?"PASS":"FAIL");
    if(!bad_machine) ++failures;
    const bool outside=pulse::dialog_admin::VerifyAdminLaunchLocation(own_root,L"C:\\Other\\bundle",own_path,IMAGE_FILE_MACHINE_AMD64)!=ERROR_SUCCESS;
    std::printf("[%s] user-writable or outside installed launch tree rejected\n",outside?"PASS":"FAIL");
    if(!outside) ++failures;
    wchar_t program_files[32768]{};
    if (GetEnvironmentVariableW(L"ProgramFiles", program_files, 32768))
        std::printf("[INFO] ProgramFiles protected path error=%lu\n",
                    pulse::dialog_admin::VerifyProtectedPath(program_files, true));
    if(argc==4 && std::wstring_view(argv[1])==L"--control-root-readonly") {
        const bool expected=std::wstring_view(argv[3])==L"allowed";
        if(!expected && std::wstring_view(argv[3])!=L"denied") return 2;
        const auto path=std::filesystem::path(argv[2])/L"ManagedAdminSessions";
        const DWORD before=GetFileAttributesW(path.c_str());
        std::wstring chosen;
        const DWORD error=pulse::dialog_admin::AdminControlRoot(argv[2],false,chosen);
        const bool pass=(error==ERROR_SUCCESS)==expected && before==GetFileAttributesW(path.c_str());
        std::printf("[%s] existing protected-root policy checked without creation or ACL repair error=%lu\n",pass?"PASS":"FAIL",error);
        if(!pass) ++failures;
    }
    return failures ? 1 : 0;
}
