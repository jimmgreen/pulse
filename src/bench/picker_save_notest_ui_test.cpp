#include "../picker_inprocess/picker_ui_api_v2.h"
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <cstdio>
#include <vector>
struct State { unsigned mode=0,ready=0,file_ok=0,overwrite=0; HWND window=nullptr; bool visible=false,timed=false; };
State* active=nullptr;
void CALLBACK Timeout(HWND,UINT,UINT_PTR,DWORD){if(active&&active->window){active->timed=true;PostMessageW(active->window,WM_CLOSE,0,0);}}
HRESULT WINAPI Event(void* context,const PulsePickerUiV2Event* event){
    auto& s=*static_cast<State*>(context);
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_READY){++s.ready;s.window=event->window;s.visible=IsWindowVisible(s.window)!=FALSE;
        if(s.mode==0)PostMessageW(s.window,WM_CLOSE,0,0);else PostMessageW(GetDlgItem(s.window,102),WM_KEYDOWN,VK_RETURN,0);}
    if(event->kind==PULSE_PICKER_UI_V2_EVENT_FILE_OK){++s.file_ok;if(s.mode==3){PostMessageW(s.window,WM_CLOSE,0,0);return S_FALSE;}}
    return S_OK;
}
HRESULT WINAPI Overwrite(void* context,const PulsePickerUiV2OverwriteEvent*,uint32_t* response){++static_cast<State*>(context)->overwrite;*response=PULSE_PICKER_UI_V2_OVERWRITE_ACCEPT;return S_OK;}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;std::setvbuf(stdout,nullptr,_IONBF,0);if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 2;
    unsigned failures=0;auto check=[&](bool ok,const char* label){std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);if(!ok)++failures;};
    const auto root=std::filesystem::absolute(std::filesystem::path(L"bench_data")/(L"save-notest-ui-"+std::to_wstring(GetCurrentProcessId())));
    if(!CreateDirectoryW(root.c_str(),nullptr))return 2;const auto existing=root/L"owned.txt";
    HANDLE file=CreateFileW(existing.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return 2;CloseHandle(file);
    HMODULE module=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Get=HRESULT(WINAPI*)(uint32_t,uint32_t,PulsePickerUiV2Api*);auto get=module?reinterpret_cast<Get>(GetProcAddress(module,"PulsePickerUiGetApiV2")):nullptr;
    PulsePickerUiV2Api api{};PulsePickerUiV2Session session=0;PulsePickerUiV2Initialize init{};init.size=sizeof(init);init.version=2;
    if(!get||get(2,sizeof(api),&api)!=S_OK||api.Initialize(&init,&session)!=S_OK)return 2;
    for(unsigned mode=0;mode<5;++mode){
        std::wstring folder=root.wstring(),name=mode==2?L"owned.txt":mode==4?L"fresh.txt":L"absent\\leaf.txt";
        PulsePickerUiV2Config config{};config.size=sizeof(config);config.version=2;config.access_policy=PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
        config.save_options=PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT|PULSE_PICKER_UI_V2_SAVE_NO_TEST_CREATE;
        config.initial_folder={folder.c_str(),static_cast<uint32_t>(folder.size())};config.filename={name.c_str(),static_cast<uint32_t>(name.size())};
        State state;state.mode=mode;active=&state;PulsePickerUiV2Callbacks callbacks{};callbacks.size=sizeof(callbacks);callbacks.context=&state;callbacks.callback=Event;callbacks.overwrite=Overwrite;
        std::vector<wchar_t> path(PULSE_PICKER_UI_V2_MAX_UNITS+1);PulsePickerUiV2Result result{};result.size=sizeof(result);result.path={path.data(),static_cast<uint32_t>(path.size()),0};
        const UINT_PTR timer=SetTimer(nullptr,0,10000,Timeout);const HRESULT hr=api.Show(session,&config,&callbacks,&result);KillTimer(nullptr,timer);active=nullptr;
        std::printf("mode=%u hr=%08lx status=%u shown=%u ready=%u FileOk=%u overwrite=%u timed=%u path=%ls\n",mode,hr,result.status,result.flags,state.ready,state.file_ok,state.overwrite,state.timed,path.data());
        check(!state.timed&&state.visible&&state.ready==1&&(result.flags&PULSE_PICKER_UI_V2_UI_SHOWN),"real visible UI reached and completed without watchdog");
        check((mode==0||mode==3)?hr==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&result.path.units==0:hr==S_OK&&std::wstring(path.data())==(root/name).wstring(),"cancel/veto or exact accepted result");
        check(state.file_ok==(mode==0?0u:1u)&&state.overwrite==(mode==2?1u:0u),"FileOk and overwrite preserved");
        check(GetFileAttributesW((root/L"absent").c_str())==INVALID_FILE_ATTRIBUTES && GetFileAttributesW((root/L"fresh.txt").c_str())==INVALID_FILE_ATTRIBUTES,"no missing parent or file created");
    }
    check(api.Shutdown(session)==S_OK,"shutdown");FreeLibrary(module);CoUninitialize();check(DeleteFileW(existing.c_str())&&RemoveDirectoryW(root.c_str()),"owned cleanup");std::printf("failures=%u\n",failures);return failures?1:0;
}
