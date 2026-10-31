#include "../shell_bridge/legacy_com_provider.h"
#include <windows.h>
#include <shlobj.h>
#include <uiautomation.h>
#include <wrl/client.h>
#include <filesystem>
#include <cstdio>
using Microsoft::WRL::ComPtr;
struct State {IFileDialog* dialog=nullptr;bool allowed=false,blocked=false,inspected=false;};State* state=nullptr;
void CALLBACK Inspect(HWND,UINT,UINT_PTR,DWORD){
 ComPtr<IOleWindow> ole;HWND window=nullptr;if(!state||FAILED(state->dialog->QueryInterface(IID_PPV_ARGS(&ole)))||FAILED(ole->GetWindow(&window)))return;
 ComPtr<IUIAutomation> automation;ComPtr<IUIAutomationElement> root;ComPtr<IUIAutomationCondition> all;ComPtr<IUIAutomationElementArray> elements;
 if(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation)))&&SUCCEEDED(automation->ElementFromHandle(window,&root))&&SUCCEEDED(automation->CreateTrueCondition(&all))&&SUCCEEDED(root->FindAll(TreeScope_Subtree,all.Get(),&elements))){
  int count=0;elements->get_Length(&count);for(int i=0;i<count;++i){ComPtr<IUIAutomationElement> item;BSTR name=nullptr;if(SUCCEEDED(elements->GetElement(i,&item))&&SUCCEEDED(item->get_CurrentName(&name))&&name){std::wstring text(name);if(text==L"allowed.zip")state->allowed=true;if(text==L"blocked.txt")state->blocked=true;SysFreeString(name);}}state->inspected=true;
 }
 PostMessageW(window,WM_COMMAND,IDCANCEL,0);
}
int main(){std::setvbuf(stdout,nullptr,_IONBF,0);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);unsigned failed=0;
 const auto root=std::filesystem::absolute(std::filesystem::path(L"bench_data")/(L"native-filter-tail-"+std::to_wstring(GetCurrentProcessId())));if(!CreateDirectoryW(root.c_str(),nullptr))return 2;
 for(const auto* name:{L"allowed.zip",L"blocked.txt"}){HANDLE f=CreateFileW((root/name).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return 2;CloseHandle(f);}
 for(const auto* pattern:{L";*.zip",L"*.zip;;*.rar;"}){
  ComPtr<IUnknown> native;ComPtr<IFileOpenDialog> dialog;pulse::shell_bridge::CreateSystemLegacyDialog(false,&native);if(!native||FAILED(native.As(&dialog)))return 2;
  dialog->SetOptions(FOS_FORCEFILESYSTEM|FOS_NOCHANGEDIR|FOS_FILEMUSTEXIST|FOS_PATHMUSTEXIST);COMDLG_FILTERSPEC filter{L"Owned filter",pattern};const auto set=dialog->SetFileTypes(1,&filter);
  ComPtr<IShellItem> folder;SHCreateItemFromParsingName(root.c_str(),nullptr,IID_PPV_ARGS(&folder));dialog->SetFolder(folder.Get());State run;run.dialog=dialog.Get();state=&run;
  UINT_PTR timer=SetTimer(nullptr,0,2000,Inspect);const auto hr=dialog->Show(nullptr);KillTimer(nullptr,timer);state=nullptr;
  const bool ok=set==S_OK&&hr==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&run.inspected&&run.allowed&&!run.blocked;std::printf("[%s] pattern=%ls set=%08lx show=%08lx inspected=%u allowed=%u blocked=%u\n",ok?"PASS":"FAIL",pattern,set,hr,run.inspected,run.allowed,run.blocked);if(!ok)++failed;
 }
 for(const auto* name:{L"allowed.zip",L"blocked.txt"})if(!DeleteFileW((root/name).c_str()))++failed;if(!RemoveDirectoryW(root.c_str()))++failed;CoUninitialize();std::printf("failures=%u\n",failed);return failed?1:0;}
