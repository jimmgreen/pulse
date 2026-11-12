#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <propsys.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <filesystem>
#include <cstdio>
#include <string>
#include <cwchar>
using Microsoft::WRL::ComPtr;
struct Probe {
 IFileSaveDialog* dialog;
 unsigned attempts=0;
 HANDLE child=nullptr, ready=nullptr;
 bool captured=false, switch_type=false;
 unsigned stage=0;
};
void ReapInspector(Probe& p) {
 if (!p.child) return;
 // Test-only containment: the inspector has flushed its result before signaling.
 // UIA provider teardown may hang; never terminate the STA dialog host.
 if (WaitForSingleObject(p.child, 100) == WAIT_TIMEOUT) TerminateProcess(p.child, 124);
 WaitForSingleObject(p.child, 2000);
 CloseHandle(p.child); p.child=nullptr;
}
void Text(const wchar_t* text) { if(!text)return; char out[4096]{}; WideCharToMultiByte(CP_UTF8,0,text,-1,out,sizeof(out),nullptr,nullptr); std::printf("%s",out); }
int Inspect(HWND hwnd, HANDLE ready) {
 setvbuf(stdout,nullptr,_IONBF,0);
 CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 ComPtr<IUIAutomation> automation; HRESULT hr=CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation));
 ComPtr<IUIAutomationElement> root; ComPtr<IUIAutomationCondition> condition; ComPtr<IUIAutomationElementArray> nodes;
 if(SUCCEEDED(hr))hr=automation->ElementFromHandle(hwnd,&root);
 VARIANT label_id{};label_id.vt=VT_BSTR;label_id.bstrVal=SysAllocString(L"PreviewMetadataLabel");
 if(SUCCEEDED(hr))hr=automation->CreatePropertyCondition(UIA_AutomationIdPropertyId,label_id,&condition);
 VariantClear(&label_id);
 if(SUCCEEDED(hr))hr=root->FindAll(TreeScope_Subtree,condition.Get(),&nodes);
 int count=0;if(nodes)nodes->get_Length(&count);
 std::printf("UIA hr=%08X nodes=%d\n",(unsigned)hr,count);
 for(int i=0;i<count&&i<400;++i){ComPtr<IUIAutomationElement> node;nodes->GetElement(i,&node);CONTROLTYPEID type=0;node->get_CurrentControlType(&type);
  if(type!=UIA_EditControlTypeId&&type!=UIA_TextControlTypeId&&type!=UIA_GroupControlTypeId&&type!=UIA_ComboBoxControlTypeId)continue;
  BSTR name=nullptr,aid=nullptr;node->get_CurrentName(&name);node->get_CurrentAutomationId(&aid);
  std::printf("FIELD type=%d id=",type);Text(aid);std::printf(" name=");Text(name);std::printf("\n");SysFreeString(name);SysFreeString(aid);
 }
 std::printf("CAPTURE_COMPLETE hr=%08X\n",(unsigned)hr);
 std::fflush(stdout);
 if(SUCCEEDED(hr)){SetEvent(ready);Sleep(INFINITE);}
 nodes.Reset();condition.Reset();root.Reset();automation.Reset();CoUninitialize();
 return SUCCEEDED(hr) ? 0 : 1;
}
void CancelDialog(Probe& p) {
 ComPtr<IOleWindow> window; HWND hwnd=nullptr;
 if(SUCCEEDED(p.dialog->QueryInterface(IID_PPV_ARGS(&window))) && SUCCEEDED(window->GetWindow(&hwnd)))
  PostMessageW(hwnd,WM_COMMAND,MAKEWPARAM(IDCANCEL,BN_CLICKED),0);
}
void CALLBACK Capture(HWND owner, UINT, UINT_PTR id, DWORD) {
 auto* p=reinterpret_cast<Probe*>(GetWindowLongPtrW(owner,GWLP_USERDATA)); if(!p)return;
 if(p->child && WaitForSingleObject(p->ready,0)==WAIT_OBJECT_0){
  ReapInspector(*p);
  std::printf("ACK stage=%u\n",p->stage);
  if(p->switch_type && p->stage==0){
   p->stage=1;p->attempts=0;ResetEvent(p->ready);
   const HRESULT switched=p->dialog->SetFileTypeIndex(2);
   std::printf("SWITCH index=2 hr=%08X\n",(unsigned)switched);
   if(SUCCEEDED(switched))return;
  }else p->captured=true;
  KillTimer(owner,id);CancelDialog(*p);return;
 }
 if((p->child && WaitForSingleObject(p->child,0)==WAIT_OBJECT_0)||++p->attempts>24){
  ReapInspector(*p);KillTimer(owner,id);CancelDialog(*p);return;
 }
 if(p->child)return;
 ComPtr<IOleWindow> window; HWND hwnd=nullptr;
 if(FAILED(p->dialog->QueryInterface(IID_PPV_ARGS(&window)))||FAILED(window->GetWindow(&hwnd))||!IsWindowVisible(hwnd))return;
 wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);wchar_t handle[32]{};swprintf_s(handle,L"%llu",static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
 std::wstring command=L"\""+std::wstring(path)+L"\" --inspect "+handle+L" "+std::to_wstring(reinterpret_cast<uintptr_t>(p->ready));
 STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;startup.wShowWindow=SW_HIDE;
 startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
 PROCESS_INFORMATION process{};
 if(CreateProcessW(path,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)){CloseHandle(process.hThread);p->child=process.hProcess;}
}
int wmain(int argc, wchar_t** argv){
 if(argc==4 && wcscmp(argv[1],L"--inspect")==0)return Inspect(
  reinterpret_cast<HWND>(static_cast<uintptr_t>(_wcstoui64(argv[2],nullptr,10))),
  reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(argv[3],nullptr,10))));
 if(argc!=6 || wcscmp(argv[1],L"--case")!=0)return 2;
 const wchar_t* ext=argv[2];const BOOL append=_wtoi(argv[3])!=0;
 const bool empty=_wtoi(argv[4])!=0,switch_type=_wtoi(argv[5])!=0;
 setvbuf(stdout,nullptr,_IONBF,0);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
 const auto directory=std::filesystem::absolute(L"bench_data/property_schema/defaults-owned").make_preferred();std::filesystem::create_directory(directory);
 int failures=0;
 {
  ComPtr<IFileSaveDialog> dialog;
  using FactoryEntry=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);
  static HMODULE module=LoadLibraryExW(L"comdlg32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
  const auto entry=module?reinterpret_cast<FactoryEntry>(GetProcAddress(module,"DllGetClassObject")):nullptr;
  ComPtr<IClassFactory> factory;
  HRESULT hr=entry?entry(CLSID_FileSaveDialog,IID_PPV_ARGS(&factory)):E_FAIL;
  if(SUCCEEDED(hr))hr=factory->CreateInstance(nullptr,IID_PPV_ARGS(&dialog));
  if(FAILED(hr)){std::printf("FACTORY hr=%08X\n",(unsigned)hr);return 1;}
  const std::wstring pattern=std::wstring(L"*.")+ext;COMDLG_FILTERSPEC filters[]={{ext,pattern.c_str()},{L"JPEG",L"*.jpg"}};dialog->SetFileTypes(switch_type?2:1,filters);dialog->SetDefaultExtension(ext);dialog->SetFileName(L"owned-default-probe");
  ComPtr<IShellItem> folder;SHCreateItemFromParsingName(directory.c_str(),nullptr,IID_PPV_ARGS(&folder));dialog->SetFolder(folder.Get());
  FILEOPENDIALOGOPTIONS options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_DEFAULTNOMINIMODE|FOS_DONTADDTORECENT);
  ComPtr<IPropertyDescriptionList> list;HRESULT list_hr=PSGetPropertyDescriptionListFromString(empty?L"prop:":L"prop:System.Title",IID_PPV_ARGS(&list));
  UINT list_count=0;if(list)list->GetCount(&list_count);
  std::printf("LIST empty=%u hr=%08X count=%u\n",empty?1:0,(unsigned)list_hr,list_count);
  if(FAILED(list_hr))return 1;
  hr=dialog->SetCollectedProperties(list.Get(),append);std::printf("CASE ext=");Text(ext);std::printf(" append=%u set_hr=%08X\n",append?1:0,(unsigned)hr);
  HWND owner=CreateWindowExW(0,L"STATIC",L"Owned default properties probe",WS_OVERLAPPED,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
  Probe probe{dialog.Get()};probe.switch_type=switch_type;probe.ready=CreateEventW(&security,TRUE,FALSE,nullptr);
  if(!probe.ready)return 1;SetWindowLongPtrW(owner,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&probe));SetTimer(owner,1,500,Capture);
  hr=dialog->Show(owner);KillTimer(owner,1);DestroyWindow(owner);
  ReapInspector(probe);CloseHandle(probe.ready);
  std::printf("END show_hr=%08X captured=%u\n",(unsigned)hr,probe.captured?1:0);if(!probe.captured)++failures;
 }
 std::filesystem::remove(directory);CoUninitialize();std::printf("failures=%d\n",failures);return failures?1:0;
}
