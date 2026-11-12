#include "../shell_bridge/legacy_com_provider.h"
#include "../shell_bridge/legacy_dialog_error.h"
#include <wrl/client.h>
#include <cstdio>
#include <cderr.h>
#include "legacy_template_fixture.h"
#include <string>
#include <thread>
#if defined(PULSE_LEGACY_HOOK_LOCAL_UI_TEST)
#include "../shell_bridge/file_dialog_ui_provider.h"
#include "../shell_bridge/file_dialog_wrapper.h"
#include "../shell_bridge/file_dialog_runtime.h"
#endif
using namespace pulse::shell_bridge;
struct State { const void* original=nullptr; const wchar_t* expected=nullptr; unsigned waits=0; bool setup_failed=false,undersized=false,early_submit=false; int template_commands=0; bool ansi=false,cancel=false;int init=0,ready=0,fileok=0,compat_fileok=0,destroy=0; };
State* active=nullptr;
template<class O> void BufferTrace(const char* stage,const O& input){printf("BUFFER stage=%s capacity=%lu offset=%u extension=%u bytes=",stage,input.nMaxFile,input.nFileOffset,input.nFileExtension);const auto* bytes=reinterpret_cast<const unsigned char*>(input.lpstrFile);for(size_t i=0;i<input.nMaxFile*sizeof(*input.lpstrFile) && i<96;++i)printf("%02X",bytes[i]);printf("\n");}

UINT_PTR CALLBACK Hook(HWND child,UINT msg,WPARAM wp,LPARAM lp){
 auto& s=*active;
 if(msg==WM_INITDIALOG){++s.init;HWND edit=GetDlgItem(child,1001);HWND button=GetDlgItem(child,1002);if(!edit||!button){s.setup_failed=true;return 0;}SetWindowTextW(edit,L"");PostMessageW(edit,WM_CHAR,L'x',1);PostMessageW(button,BM_CLICK,0,0);return 0;}
 if(msg==WM_COMMAND && LOWORD(wp)==1002 && HIWORD(wp)==BN_CLICKED){wchar_t text[16]{};GetDlgItemTextW(child,1001,text,16);if(wcscmp(text,L"x")==0)++s.template_commands;return 0;}
 if(msg==WM_DESTROY)++s.destroy;
 if(msg==WM_TIMER && wp==0x504c){
  bool ready=false;
  LRESULT queried=0;
  if(s.ansi){char candidate[32768]{},expected[32768]{};WideCharToMultiByte(CP_ACP,0,s.expected,-1,expected,32768,nullptr,nullptr);const auto n=SendMessageA(GetParent(child),CDM_GETFILEPATH,32768,reinterpret_cast<LPARAM>(candidate));queried=n;ready=n>0 && std::string(candidate)==expected;}
  else{wchar_t candidate[32768]{};const auto n=SendMessageW(GetParent(child),CDM_GETFILEPATH,32768,reinterpret_cast<LPARAM>(candidate));queried=n;ready=n>0 && std::wstring(candidate)==s.expected;}
  if(s.waits==0 || ready || s.waits==49)printf("READY A=%d cancel=%d poll=%u query=%lld match=%d commands=%d\n",s.ansi,s.cancel,s.waits,static_cast<long long>(queried),ready,s.template_commands);
  ready=ready && s.template_commands==1;
  if(ready || ++s.waits>=50){KillTimer(child,0x504c);s.setup_failed=!ready;const int command=ready?IDOK:IDCANCEL;HWND button=GetDlgItem(GetParent(child),command);printf("ACTION A=%d cancel=%d command=%d button=%d setup_failed=%d waits=%u\n",s.ansi,s.cancel,command,button!=nullptr,s.setup_failed,s.waits);if(button)PostMessageW(button,BM_CLICK,0,0);else PostMessageW(GetParent(child),WM_COMMAND,command,0);}
  return 0;
 }
 if(msg==RegisterWindowMessageW(FILEOKSTRINGW))++s.compat_fileok;
 if(msg==WM_NOTIFY){const auto* h=reinterpret_cast<NMHDR*>(lp);
  if(h->code==CDN_INITDONE){++s.ready;if(s.early_submit){printf("EARLY_IDOK A=%d cancel=%d parent_visible=%d\n",s.ansi,s.cancel,IsWindowVisible(GetParent(child)));PostMessageW(GetParent(child),WM_COMMAND,IDOK,0);}else SetTimer(child,0x504c,100,nullptr);}
  if(h->code==CDN_FILEOK){++s.fileok;
   if(s.undersized){if(s.ansi){const auto* n=reinterpret_cast<OFNOTIFYA*>(lp);const auto* original=static_cast<const OPENFILENAMEA*>(s.original);printf("FILEOK A=1 same_ofn=%d same_buffer=%d notify_capacity=%lu original_capacity=%lu offset=%u extension=%u\n",n->lpOFN==original,n->lpOFN->lpstrFile==original->lpstrFile,n->lpOFN->nMaxFile,original->nMaxFile,n->lpOFN->nFileOffset,n->lpOFN->nFileExtension);}else{const auto* n=reinterpret_cast<OFNOTIFYW*>(lp);const auto* original=static_cast<const OPENFILENAMEW*>(s.original);printf("FILEOK A=0 same_ofn=%d same_buffer=%d notify_capacity=%lu original_capacity=%lu offset=%u extension=%u\n",n->lpOFN==original,n->lpOFN->lpstrFile==original->lpstrFile,n->lpOFN->nMaxFile,original->nMaxFile,n->lpOFN->nFileOffset,n->lpOFN->nFileExtension);}}
if(s.fileok==1 && !s.undersized){SetWindowLongPtrW(child,DWLP_MSGRESULT,1);PostMessageW(GetParent(child),WM_COMMAND,s.cancel?IDCANCEL:IDOK,0);return 1;}}
 }
 return 0;
}
int wmain(int argc,wchar_t** argv){
 std::setvbuf(stdout,nullptr,_IONBF,0);
 bool oracle=false,small_buffer=false,small_preflight=false,early_submit=false;
 for(int i=1;i<argc;++i){oracle|=std::wstring_view(argv[i])==L"--native";small_buffer|=std::wstring_view(argv[i])==L"--small-buffer";small_preflight|=std::wstring_view(argv[i])==L"--small-preflight";early_submit|=std::wstring_view(argv[i])==L"--early-submit";}
 small_buffer|=small_preflight;
#if defined(PULSE_LEGACY_HOOK_LOCAL_UI_TEST)
 std::shared_ptr<const FileDialogInprocessProvider> provider;
 for(int i=1;i+1<argc;++i) if(std::wstring_view(argv[i])==L"--ui") {const HRESULT loaded=LoadFileDialogUiProvider(argv[i+1],provider,true);if(FAILED(loaded)){printf("UI_LOAD=%08lX\n",static_cast<unsigned long>(loaded));return 2;}}
#else
 (void)argc;(void)argv;
#endif
 pulse::bench::LegacyTemplateFixture template_fixture;
 bool named=false;for(int i=1;i<argc;++i)named|=std::wstring_view(argv[i])==L"--resource";
 unsigned pulse_shows=0,factories=0;
 int failures=0;wchar_t temp[32768]{};GetTempPathW(32768,temp);std::wstring path=std::wstring(temp)+L"PulseLegacyHook-"+std::to_wstring(GetCurrentProcessId())+L".txt";HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return 2;CloseHandle(f);
 const auto split=path.find_last_of(L"\\/");const auto leaf=path.substr(split+1);const auto directory=path.substr(0,split);
 LegacyDialogFactory factory=[&](bool save,IFileDialog** out){++factories;Microsoft::WRL::ComPtr<IUnknown> native;HRESULT hr=CreateSystemLegacyDialog(save,&native);if(FAILED(hr))return hr;
#if defined(PULSE_LEGACY_HOOK_LOCAL_UI_TEST)
 if(provider){FileDialogBridgeServices services;services.inprocess_provider=provider;services.validation_verified=[&](const ContractDecision& decision){if(!RuntimeContractCertified(decision)||!decision.request)return false;const auto* request=std::get_if<pulse::ipc::picker::Request>(&decision.request->body);return request&&request->operation==pulse::ipc::picker::Operation::OpenFiles&&request->initial_directory==std::u16string(directory.begin(),directory.end())&&request->filename==std::u16string(leaf.begin(),leaf.end());};services.observe_show=[&](bool begin, const FileDialogShowObservation& observed){printf("SHOW begin=%d fallback=%d started=%d ui=%d result=%08lX reason=%u stage=%u\n",begin,observed.native_fallback,observed.provider_started,observed.ui_shown?(*observed.ui_shown?1:0):-1,static_cast<unsigned long>(observed.result),static_cast<unsigned>(observed.contract_reason),static_cast<unsigned>(observed.fallback));if(!begin&&!observed.native_fallback && observed.provider_started && observed.ui_shown.value_or(false))++pulse_shows;};services.access_policy=AccessPolicy::LocalOpenV1;services.save_access_policy=AccessPolicy::LocalSaveV1;return WrapNativeFileDialog(save?DialogFactory::Save:DialogFactory::Open,native.Get(),services,IID_IFileDialog,reinterpret_cast<void**>(out));}
#endif
 return native->QueryInterface(IID_PPV_ARGS(out));};
 for(bool ansi:{false,true})for(bool cancel:{false,true}){if(small_buffer && cancel)continue;const unsigned factories_before=factories;State s;s.early_submit=early_submit;s.ansi=ansi;s.cancel=cancel;s.expected=path.c_str();s.undersized=small_buffer;active=&s;BOOL result=FALSE;bool consumed=false;HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);const DWORD tid=GetCurrentThreadId();std::thread watchdog([=]{if(WaitForSingleObject(done,10000)==WAIT_TIMEOUT){printf("WATCHDOG tid=%lu\n",tid);EnumThreadWindows(tid,[](HWND w,LPARAM)->BOOL{PostMessageW(w,WM_CLOSE,0,0);return TRUE;},0);}});
 if(ansi){char name[32768]{};WideCharToMultiByte(CP_ACP,0,leaf.c_str(),-1,name,32768,nullptr,nullptr);char initial[32768]{};WideCharToMultiByte(CP_ACP,0,directory.c_str(),-1,initial,32768,nullptr,nullptr);OPENFILENAMEA o{};o.lpstrInitialDir=initial;o.lStructSize=sizeof(o);o.lpstrFile=name;o.nMaxFile=small_buffer?static_cast<DWORD>(leaf.size()+1):32768;o.Flags=OFN_EXPLORER|OFN_HIDEREADONLY|OFN_NOCHANGEDIR|OFN_FILEMUSTEXIST|OFN_ENABLEHOOK;o.Flags|=named?OFN_ENABLETEMPLATE:OFN_ENABLETEMPLATEHANDLE;o.hInstance=named?GetModuleHandleW(nullptr):template_fixture.Handle();o.lpTemplateName=MAKEINTRESOURCEA(101);o.lpfnHook=Hook;o.lCustData=reinterpret_cast<LPARAM>(&s);s.original=&o;if(small_buffer)BufferTrace("before",o);if(oracle){consumed=true;result=GetOpenFileNameA(&o);}else consumed=RunLegacyComDialog(&o,false,factory,result,{},true);if(small_buffer)BufferTrace("after",o);}
 else{wchar_t name[32768]{};wcscpy_s(name,leaf.c_str());OPENFILENAMEW o{};o.lpstrInitialDir=directory.c_str();o.lStructSize=sizeof(o);o.lpstrFile=name;o.nMaxFile=small_buffer?static_cast<DWORD>(leaf.size()+1):32768;o.Flags=OFN_EXPLORER|OFN_HIDEREADONLY|OFN_NOCHANGEDIR|OFN_FILEMUSTEXIST|OFN_ENABLEHOOK;o.Flags|=named?OFN_ENABLETEMPLATE:OFN_ENABLETEMPLATEHANDLE;o.hInstance=named?GetModuleHandleW(nullptr):template_fixture.Handle();o.lpTemplateName=MAKEINTRESOURCEW(101);o.lpfnHook=Hook;o.lCustData=reinterpret_cast<LPARAM>(&s);s.original=&o;if(small_buffer)BufferTrace("before",o);if(oracle){consumed=true;result=GetOpenFileNameW(&o);}else consumed=RunLegacyComDialog(&o,false,factory,result,{},true);if(small_buffer)BufferTrace("after",o);}
 SetEvent(done);watchdog.join();CloseHandle(done);const DWORD error=result?0:oracle?CommDlgExtendedError():legacy_dialog_error::Query(&CommDlgExtendedError);
 bool ok=small_preflight ? !consumed&&!result&&s.init==0&&s.ready==0&&s.fileok==0&&s.destroy==0&&factories==factories_before : small_buffer ? consumed&&!result&&error==FNERR_BUFFERTOOSMALL : !s.setup_failed&&s.template_commands==1&&consumed&&result==!cancel&&s.init==1&&s.ready==1&&s.fileok==(cancel?1:2)&&s.compat_fileok==(cancel?0:1)&&s.destroy==1;
 if(small_preflight)printf("PREFLIGHT A=%d factories=%u callbacks=%d consumed=%d\n",ansi,factories-factories_before,s.init+s.ready+s.fileok,consumed);
 else if(small_buffer)printf("SMALL A=%d error=%lu expected=%u fileok=%d\n",ansi,error,FNERR_BUFFERTOOSMALL,s.fileok);printf("TEMPLATE named=%d commands=%d waits=%u setup_failed=%d ",named,s.template_commands,s.waits,s.setup_failed);printf("A=%d cancel=%d consumed=%d result=%d init=%d ready=%d fileok=%d destroy=%d pass=%d\n",ansi,cancel,consumed,result,s.init,s.ready,s.fileok,s.destroy,ok);failures+=!ok;
 }
#if defined(PULSE_LEGACY_HOOK_LOCAL_UI_TEST)
 if(provider){printf("PULSE_SHOWS=%u expected=%u\n",pulse_shows,small_preflight?0u:small_buffer?2u:4u);failures+=pulse_shows!=(small_preflight?0u:small_buffer?2u:4u);}
#else
 (void)pulse_shows;
#endif
 DeleteFileW(path.c_str());printf("failures=%d\n",failures);return failures?1:0;}

