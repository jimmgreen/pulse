#pragma once
#include "../app/picker_tray_consume.h"
#include "../common/picker_tray_shared.h"
#include <future>
#include <mutex>

namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }
namespace {
// A genuine StagingTray endpoint on its own hidden thread. No published global
// mapping, installed instance, user shelf or pointer/keyboard injection.
class PickerTrayUiFixture {
public:
    std::atomic<unsigned> requests{0}, removed{0}, retained{0}, rejected{0};
    PickerTrayUiFixture(std::vector<std::wstring> paths, int owned = -1) {
        std::promise<HWND> ready; auto future=ready.get_future();
        thread_=std::thread([this, paths=std::move(paths), owned, ready=std::move(ready)]() mutable {
            WNDCLASSW cls{};cls.lpfnWndProc=Proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"PulsePickerTrayUiFixture";RegisterClassW(&cls);
            window_=CreateWindowExW(0,cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,this);
            if(window_) {
                std::vector<pulse::app::TrayItem> items;
                for(size_t i=0;i<paths.size();++i){pulse::app::TrayItem item;item.path=paths[i];item.size=std::filesystem::file_size(paths[i]);item.exists=true;
                    item.display_name=std::filesystem::path(paths[i]).filename().wstring();item.owned_snippet=static_cast<int>(i)==owned;item.content_kind=item.owned_snippet?2:0;items.push_back(std::move(item));}
                tray_.CollectRichToShelf(tray_.active_shelf_id(),items,false);
            }
            ready.set_value(window_);
            if(window_){MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}}
        });
        window_=future.get();
    }
    ~PickerTrayUiFixture(){if(window_)PostMessageW(window_,WM_CLOSE,0,0);if(thread_.joinable())thread_.join();}
    pulse::picker_places::Snapshot Snapshot(){
        std::lock_guard lock(mutex_);pulse::picker_places::Snapshot s;
        pulse::picker_places::CaptureTray(tray_.active_shelf(),L"Isolated tray",window_,s);
        s.endpoint=window_;s.endpoint_pid=GetCurrentProcessId();s.shared_available=true;s.tray_revision=1;
        return s;
    }
    size_t Count(){return Snapshot().tray_items.size();}
private:
    static LRESULT CALLBACK Proc(HWND w,UINT message,WPARAM wp,LPARAM lp){
        if(message==WM_NCCREATE)SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self=reinterpret_cast<PickerTrayUiFixture*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(message==WM_COPYDATA&&self){
            pulse::picker_places::TrayConsumeRequest request;
            if(!pulse::picker_places::DecodeConsumeTray(reinterpret_cast<COPYDATASTRUCT*>(lp),request))return 0;
            std::lock_guard lock(self->mutex_);
            auto result=pulse::app::ApplyPickerTrayConsume(self->tray_,request,GetCurrentProcessId(),w);
            self->removed+=result.removed;self->retained+=result.skipped_owned;if(!result.accepted)++self->rejected;++self->requests;
            return pulse::picker_places::EncodeConsumeTrayResult(result);
        }
        if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
        return DefWindowProcW(w,message,wp,lp);
    }
    HWND window_=nullptr;
    pulse::app::StagingTray tray_;
    std::mutex mutex_;
    std::thread thread_;
};
}
