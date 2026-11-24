#include "plugin/preview_dispatch.h"
#include <cstdio>
using namespace pulse::desktop::plugin;
struct Backend {
    int calls=0; uint32_t mode=0; std::string path; PulseScreenRect rect{}; bool anchored=false; PulseStatus result=PULSE_OK;
    static PulseStatus PULSE_CALL Reveal(void* ctx,PulseStringView path,uint32_t mode,const PulseScreenRect* rect) {
        auto& b=*static_cast<Backend*>(ctx);++b.calls;b.mode=mode;b.path.assign(path.data,path.length);b.anchored=rect!=nullptr;if(rect)b.rect=*rect;return b.result;
    }
};
int main() {
    int failed=0; auto check=[&](bool ok,const char* name){std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);failed+=!ok;};
    Backend backend;PulseWorkbenchV2 api{};api.context=&backend;api.reveal_path_from_rect=Backend::Reveal;
    PreviewDispatch dispatch;PulseStatus status=0;
    for(const auto& mode:{"preview","hover_preview","dismiss_hover_preview"}) {
        const auto json=std::string("{\"mode\":\"")+mode+"\",\"path\":\"C:\\\\fixture\\\\file.pdf\",\"rect_x\":-200,\"rect_y\":20,\"rect_w\":80,\"rect_h\":90}";
        const uint32_t expected=std::string(mode)=="preview"?3:std::string(mode)=="hover_preview"?4:5;
        check(dispatch.Dispatch(json,&api,status)&&status==PULSE_OK&&backend.mode==expected&&backend.path=="C:\\fixture\\file.pdf"&&backend.anchored&&backend.rect.x==-200&&backend.rect.height==90,"three modes forward absolute path and screen anchor unchanged");
    }
    const std::string hover=R"({"mode":"hover_preview","path":"C:\\fixture\\a.pdf"})";
    int calls=backend.calls;
    check(dispatch.Dispatch(hover,nullptr,status)&&status==PULSE_ERROR_UNAVAILABLE&&backend.calls==calls,"V1 or missing V2 does not navigate or launch");
    check(!dispatch.Dispatch(R"({"mode":"unknown","path":"C:\\a.pdf"})",&api,status)&&backend.calls==calls,"unknown mode is not claimed");
    for(const auto& extra:{"\"rect_w\":-1,\"rect_h\":2,\"rect_x\":0,\"rect_y\":0","\"rect_x\":2147483648,\"rect_y\":0,\"rect_w\":1,\"rect_h\":1","\"rect_x\":0.5,\"rect_y\":0,\"rect_w\":1,\"rect_h\":1","\"rect_x\":0"}) {
        auto json=std::string(R"({"mode":"preview","path":"C:\\a.pdf",)")+extra+"}";
        check(dispatch.Dispatch(json,&api,status)&&status==PULSE_ERROR_ARGUMENT&&backend.calls==calls,"invalid or partial anchor rejected before backend");
    }
    dispatch.Dispatch(hover,&api,status);
    backend.result=PULSE_ERROR_STATE;
    dispatch.Dispatch(R"({"mode":"hover_preview","path":"C:\\fixture\\b.pdf"})",&api,status);
    check(dispatch.HoverPath()=="C:\\fixture\\a.pdf","failed replacement retains last accepted hover");
    backend.result=PULSE_OK;dispatch.Deactivate(&api);
    check(backend.mode==5&&backend.path=="C:\\fixture\\a.pdf"&&dispatch.HoverPath().empty(),"deactivate cancels last accepted path");
    calls=backend.calls;dispatch.Deactivate(&api);
    check(backend.calls==calls,"repeated deactivate does not emit duplicate cancel");
    dispatch.Dispatch(hover,&api,status);
    dispatch.Dispatch(R"({"mode":"dismiss_hover_preview","path":"C:\\fixture\\old.pdf"})",&api,status);
    check(!dispatch.HoverPath().empty(),"stale dismiss does not clear current lifecycle");
    dispatch.Dispatch(R"({"mode":"preview","path":"C:\\fixture\\b.pdf"})",&api,status);
    calls=backend.calls;dispatch.Deactivate(&api);
    check(dispatch.HoverPath().empty()&&backend.calls==calls,"accepted explicit preview clears pending hover lifecycle");
    check(dispatch.Dispatch(R"({"mode":"preview","path":"relative.pdf"})",&api,status)&&status==PULSE_ERROR_ARGUMENT,"relative path rejected");
    std::printf("PREVIEW_DISPATCH failed=%d\n",failed);return failed?1:0;
}
