#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <cstdio>
#include <string>
using Microsoft::WRL::ComPtr;
struct Sink final: Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IFileDialogEvents> {
 ComPtr<IShellItem> target; bool issued=false; unsigned depth=0; HRESULT navigation=E_PENDING;
 void Folder(IFileDialog* d,const char* label){ComPtr<IShellItem> i;PWSTR s=nullptr;auto h=d->GetFolder(&i);if(i)i->GetDisplayName(SIGDN_FILESYSPATH,&s);printf("%s hr=%08lX path=%ls depth=%u\n",label,h,s?s:L"",depth);fflush(stdout);CoTaskMemFree(s);}
 HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* d)override {++depth;Folder(d,"changed enter");if(!issued){issued=true;puts("SetFolder begin");navigation=d->SetFolder(target.Get());printf("SetFolder return=%08lX\n",navigation);Folder(d,"immediate");}else{d->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));}puts("changed exit");--depth;return S_OK;}
 HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog* d,IShellItem*)override {Folder(d,"changing");return S_OK;}
 HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*)override{return S_OK;}
 HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*,IShellItem*,FDE_SHAREVIOLATION_RESPONSE*r)override{*r=FDESVR_DEFAULT;return S_OK;}
 HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*,IShellItem*,FDE_OVERWRITE_RESPONSE*r)override{*r=FDEOR_DEFAULT;return S_OK;}
};
int wmain(){CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);wchar_t tmp[MAX_PATH]{};GetTempPathW(MAX_PATH,tmp);std::wstring root=std::wstring(tmp)+L"PulseNavOracle_"+std::to_wstring(GetCurrentProcessId());auto target=root+L"\\target";CreateDirectoryW(root.c_str(),nullptr);CreateDirectoryW(target.c_str(),nullptr);ComPtr<IFileOpenDialog>d;using Entry=HRESULT(WINAPI*)(REFCLSID,REFIID,void**); auto module=LoadLibraryExW(L"comdlg32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);auto entry=reinterpret_cast<Entry>(GetProcAddress(module,"DllGetClassObject"));ComPtr<IClassFactory>factory;auto h=entry(CLSID_FileOpenDialog,IID_PPV_ARGS(&factory));if(SUCCEEDED(h))h=factory->CreateInstance(nullptr,IID_PPV_ARGS(&d));if(FAILED(h))return 2;GUID g{};CoCreateGuid(&g);d->SetClientGuid(g);ComPtr<IShellItem>base;SHCreateItemFromParsingName(root.c_str(),nullptr,IID_PPV_ARGS(&base));auto sink=Microsoft::WRL::Make<Sink>();SHCreateItemFromParsingName(target.c_str(),nullptr,IID_PPV_ARGS(&sink->target));d->SetFolder(base.Get());DWORD cookie=0;d->Advise(sink.Get(),&cookie);h=d->Show(nullptr);printf("Show=%08lX navigation=%08lX\n",h,sink->navigation);d->Unadvise(cookie);d->ClearClientData();RemoveDirectoryW(target.c_str());RemoveDirectoryW(root.c_str());CoUninitialize();return sink->navigation==S_OK?0:1;}
