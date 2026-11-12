#include "../common/dialog_bundle.h"
#include <filesystem>
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    if(argc!=3) return 2;
    pulse::dialog_bundle::BundleLease lease;
    const bool installed=std::wstring_view(argv[2]).starts_with(L"installed-");
    const DWORD error=installed ? pulse::dialog_bundle::ResolveInstalledBundleForMachine(argv[1],IMAGE_FILE_MACHINE_I386,lease) : pulse::dialog_bundle::VerifyBundle(argv[1],{},lease);
    const bool expected=std::wstring_view(argv[2])==L"valid" || std::wstring_view(argv[2])==L"installed-valid";
    bool ok=expected ? !error && lease : error && !lease;
#ifndef PULSE_THIN_BASELINE
    if(ok && expected && lease.IsThinCompanion()) {
        ok=lease.Machine()==IMAGE_FILE_MACHINE_I386 && lease.UiProvider().empty() &&
            std::filesystem::path(lease.Host()).parent_path()!=std::filesystem::path(lease.Directory()) &&
            !pulse::dialog_bundle::RecheckDirectory(lease);
        HANDLE file=CreateFileW(lease.Host().c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        ok=ok && file==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION;
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    }
#endif
    std::cout<<(ok?"[PASS]":"[FAIL]")<<" expected="<<(expected?"valid":"reject")<<" error="<<error<<" lease="<<bool(lease)<<'\n';
    return ok?0:1;
}
