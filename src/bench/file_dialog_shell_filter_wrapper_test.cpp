#include "../shell_bridge/file_dialog_wrapper.h"
#include "../shell_bridge/file_dialog_inprocess_session.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
using namespace pulse::shell_bridge;
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0,failures=0;
void Check(bool ok,const char* label){++checks;failures+=!ok;std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);}
struct State{HWND window=nullptr;std::wstring folder,visible,blocked;bool active=true,close=false,accept=false;HRESULT closed=S_OK;unsigned shown=0,initialized=0,native=0;HRESULT visible_verdict=S_OK,blocked_verdict=S_FALSE;};State* state=nullptr;
HRESULT WINAPI Initialize(const PulsePickerUiV2Initialize*,PulsePickerUiV2Session* out){++state->initialized;*out=1;return S_OK;}
HRESULT WINAPI Validate(PulsePickerUiV2Session,const PulsePickerUiV2Config* config){Check(bool(config->open_options&PULSE_PICKER_UI_V2_OPEN_SHELL_ITEM_FILTER)==state->active,"config retains only the effective filter state");return S_OK;}
HRESULT WINAPI Window(PulsePickerUiV2Session,uint64_t,HWND* out){*out=state->window;return S_OK;}
HRESULT WINAPI Close(PulsePickerUiV2Session,uint64_t,HRESULT result){state->close=true;state->closed=result;return S_OK;}
HRESULT WINAPI Shutdown(PulsePickerUiV2Session){return S_OK;}
HRESULT WINAPI Query(PulsePickerUiV2Session,uint64_t show,uint64_t generation,PulsePickerUiV2Snapshot* out){
 if(show!=1||(generation&&generation!=1))return HRESULT_FROM_WIN32(ERROR_RETRY);
 const std::wstring name=L"blocked.txt";const std::wstring* text[]{&state->folder,&name,&state->blocked,&state->blocked};PulsePickerUiV2Buffer* buffer[]{&out->folder,&out->filename,&out->selection,&out->candidate};
 out->show_id=1;out->generation=1;out->window=state->window;out->flags=PULSE_PICKER_UI_V2_STATE_CANDIDATE_READY;out->filter_index=0;bool fits=true;
 for(size_t i=0;i<4;++i){buffer[i]->units=static_cast<uint32_t>(text[i]->size());fits&=buffer[i]->data&&buffer[i]->capacity>text[i]->size();}
 if(!fits)return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
 for(size_t i=0;i<4;++i)wcscpy_s(buffer[i]->data,buffer[i]->capacity,text[i]->c_str());return S_OK;
}
HRESULT WINAPI Show(PulsePickerUiV2Session,const PulsePickerUiV2Config*,const PulsePickerUiV2Callbacks* callbacks,PulsePickerUiV2Result* out){
 ++state->shown;state->close=false;state->window=CreateWindowExW(0,L"STATIC",L"isolated hidden filter fixture",WS_OVERLAPPED,0,0,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 if(!state->window)return E_FAIL;
 out->show_id=1;out->generation=1;out->filter_index=0;out->flags=PULSE_PICKER_UI_V2_UI_SHOWN;out->path.units=0;out->paths.units=0;out->path_count=0;out->path.data[0]=0;
 auto emit=[&](uint32_t kind,const std::wstring& path)->HRESULT{PulsePickerUiV2Event e{};e.size=sizeof(e);e.kind=kind;e.show_id=1;e.generation=1;e.window=state->window;e.proposed_folder={path.data(),static_cast<uint32_t>(path.size())};return callbacks->callback(callbacks->context,&e);};
 HRESULT hr=emit(PULSE_PICKER_UI_V2_EVENT_READY,L"");
 if(hr==S_OK&&state->active){hr=emit(PULSE_PICKER_UI_V2_EVENT_INCLUDE_ITEM,state->visible);Check(hr==state->visible_verdict,"allowed item preserves caller HRESULT");hr=S_OK;if(!state->close){hr=emit(PULSE_PICKER_UI_V2_EVENT_INCLUDE_ITEM,state->blocked);Check(hr==state->blocked_verdict,"filtered item preserves caller HRESULT across the event reply");hr=S_OK;}}
 if(hr==S_OK&&!state->close&&state->accept){hr=emit(PULSE_PICKER_UI_V2_EVENT_FILE_OK,L"");Check(hr==S_OK,"typed excluded path reaches FileOk without a second filter call");}
 if(state->close){out->status=PULSE_PICKER_UI_V2_CLOSED;hr=state->closed;}
 else if(hr==S_OK&&state->accept){out->status=PULSE_PICKER_UI_V2_ACCEPTED;wcscpy_s(out->path.data,out->path.capacity,state->blocked.c_str());out->path.units=static_cast<uint32_t>(state->blocked.size());}
 else if(hr==S_OK){out->status=PULSE_PICKER_UI_V2_CANCELLED;hr=HRESULT_FROM_WIN32(ERROR_CANCELLED);}else out->status=PULSE_PICKER_UI_V2_FAILED;
 out->terminal_hr=hr;DestroyWindow(state->window);state->window=nullptr;return hr;
}
struct Filter final:IShellItemFilter{
 std::atomic<ULONG> refs{1};unsigned includes=0,enums=0;DWORD thread=GetCurrentThreadId();bool same=true,close=false;IFileDialog* dialog=nullptr;
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown&&iid!=IID_IShellItemFilter)return E_NOINTERFACE;*out=static_cast<IShellItemFilter*>(this);AddRef();return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{return --refs;}
 HRESULT STDMETHODCALLTYPE IncludeItem(IShellItem* item)override{++includes;same&=thread==GetCurrentThreadId();if(!state->window){Check(false,"unexpected native callback outside hidden provider fixture");dialog->Close(E_ABORT);return E_ABORT;}PWSTR path=nullptr;const HRESULT hr=item->GetDisplayName(SIGDN_FILESYSPATH,&path);const bool blocked=path&&state->blocked==path;Check(hr==S_OK&&path&&(blocked||state->visible==path),"callback receives real owned filesystem Shell item");CoTaskMemFree(path);if(close)dialog->Close(E_ABORT);return blocked?state->blocked_verdict:state->visible_verdict;}
 HRESULT STDMETHODCALLTYPE GetEnumFlagsForItem(IShellItem*,SHCONTF*)override{++enums;return E_NOTIMPL;}
};
ComPtr<IFileDialog> Make(FileDialogBridgeServices services){
 using Factory=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);static HMODULE module=LoadLibraryExW(L"comdlg32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);const auto factory=module?reinterpret_cast<Factory>(GetProcAddress(module,"DllGetClassObject")):nullptr;ComPtr<IClassFactory> cf;ComPtr<IFileDialog> native,dialog;HRESULT hr=factory?factory(CLSID_FileOpenDialog,IID_PPV_ARGS(&cf)):E_NOINTERFACE;if(SUCCEEDED(hr))hr=cf->CreateInstance(nullptr,IID_PPV_ARGS(&native));if(SUCCEEDED(hr))hr=WrapNativeFileDialog(DialogFactory::Open,native.Get(),services,IID_PPV_ARGS(&dialog));Check(hr==S_OK&&dialog,"isolated native factory backs the tested wrapper");if(!dialog)return dialog;
 ComPtr<IShellItem> folder;SHCreateItemFromParsingName(state->folder.c_str(),nullptr,IID_PPV_ARGS(&folder));Check(dialog->SetOptions(DefaultDialogOptions(DialogFactory::Open)|FOS_FORCEFILESYSTEM|FOS_DONTADDTORECENT)==S_OK&&dialog->SetFolder(folder.Get())==S_OK,"configure owned filesystem wrapper");return dialog;
}
}
int wmain(){
 std::setvbuf(stdout,nullptr,_IONBF,0);std::puts("SETUP filter wrapper start");
 if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 2;
 const auto root=std::filesystem::absolute(L"bench_data")/(L"filter-wrapper-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));std::filesystem::create_directories(root);for(const auto* leaf:{L"visible.txt",L"blocked.txt"}){std::ofstream file(root/leaf);file<<"owned";}
 for(unsigned scenario=0;scenario<10;++scenario){
  std::printf("SCENARIO %u\\n",scenario);
  State s;state=&s;s.folder=root.wstring();s.visible=(root/L"visible.txt").wstring();s.blocked=(root/L"blocked.txt").wstring();s.active=scenario!=2;s.accept=scenario==6;if(scenario==8)s.blocked_verdict=E_ACCESSDENIED;if(scenario==9)s.visible_verdict=HRESULT(2);
  Filter first,second;auto provider=std::make_shared<FileDialogInprocessProvider>();provider->module_lifetime=std::make_shared<int>(1);provider->allow_unverified_proof=true;auto& api=provider->api;api.size=sizeof(api);api.version=2;api.Initialize=Initialize;api.ValidateConfig=Validate;api.Show=Show;api.GetWindow=Window;api.Query=Query;api.Close=Close;api.Shutdown=Shutdown;if(scenario!=2&&scenario!=3)api.implemented_features=PULSE_PICKER_UI_V2_FEATURE_SHELL_ITEM_FILTER;
  FileDialogBridgeServices services;services.access_policy=AccessPolicy::LocalOpenV1;services.validation_verified=[](const ContractDecision& d){return d.validation_category==ValidationCategory::LocalOpenAccessV1;};services.show_native=[&](IFileDialog*,HWND){++s.native;return E_UNEXPECTED;};
  if(scenario==4)services.resolve_on_show=[&](FileDialogBridgeServices& resolved){resolved.inprocess_provider=provider;resolved.access_policy=services.access_policy;resolved.validation_verified=services.validation_verified;resolved.show_native=services.show_native;return true;};else services.inprocess_provider=provider;
  auto dialog=Make(services);if(!dialog)return 3;first.dialog=dialog.Get();second.dialog=dialog.Get();first.close=scenario==5;
  Check(dialog->SetFilter(&first)==S_OK&&first.refs==2,"SetFilter retains exactly the native single reference");
  if(scenario==2)Check(dialog->SetFilter(nullptr)==S_OK&&first.refs==1,"null filter releases immediately before provider negotiation");
  if(scenario==7){Check(dialog->SetFilter(&second)==S_OK&&first.refs==1&&second.refs==2,"replacement releases only prior filter and retains current filter");HRESULT wrong=S_OK;std::thread worker([&]{wrong=dialog->SetFilter(nullptr);});worker.join();Check(wrong==RPC_E_WRONG_THREAD&&second.refs==2,"wrong-thread setter does not clear the retained filter");}
  const auto result=dialog->Show(nullptr);
  if(scenario==3)Check(result==E_UNEXPECTED&&s.native==1&&s.initialized==0&&first.includes==0,"old provider falls back once before UI or callbacks");
  else if(scenario==5)Check(result==E_ABORT&&s.native==0&&first.includes==1,"callback Close HRESULT is terminal and never replays native UI");
  else if(scenario==6){ComPtr<IShellItem> item;PWSTR path=nullptr;const auto hr=dialog->GetResult(&item);if(item)item->GetDisplayName(SIGDN_FILESYSPATH,&path);Check(result==S_OK&&hr==S_OK&&path&&s.blocked==path&&first.includes==2&&s.native==0,"native-oracle typed excluded file is still returned exactly without re-filtering");CoTaskMemFree(path);}
  else Check(result==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&s.native==0&&s.shown==1,"cancel returns without second native window");
  if(scenario==1){Check(dialog->Show(nullptr)==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&first.includes==4&&s.shown==2,"repeated Show invokes the retained filter again without stale session state");}
  Check(first.same&&second.same&&first.enums==0&&second.enums==0,"IncludeItem stays on caller STA and GetEnumFlagsForItem remains unused");
  dialog.Reset();Check(first.refs==1&&second.refs==1,"wrapper final Release balances native filter lifetime");
 }
 std::filesystem::remove(root/L"visible.txt");std::filesystem::remove(root/L"blocked.txt");std::filesystem::remove(root);CoUninitialize();std::printf("SHELL_FILTER_WRAPPER checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
