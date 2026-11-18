#include "ui_renderer_internal.h"
#include "pane_layout_strip.h"

namespace pulse::ui {
namespace {
float ZoomEase(float t, float x1=.2f, float y1=.8f, float x2=.2f, float y2=1) {
    const auto cubic=[](float a,float b,float x) {const float q=1-x;return 3*q*q*x*a+3*q*x*x*b+x*x*x;};
    float lo=0,hi=1;
    for(int i=0;i<20;++i){const float mid=(lo+hi)*.5f;if(cubic(x1,x2,mid)<t)lo=mid;else hi=mid;}
    return cubic(y1,y2,(lo+hi)*.5f);
}
}
void MainRenderer::BeginPaneZoomMotion(const WindowViewModel& vm) {
    auto& m=pane_zoom_motion_; const auto& c=vm.layout_controls;
    const auto size=D2D1::SizeU(compositor_->Width(),compositor_->Height());
    auto identity=c.pane_paths;
    uintptr_t tabId=0;for(const auto& tab:vm.tabs)if(tab.active){tabId=tab.id;break;}
    identity.push_back(std::to_wstring(tabId));identity.push_back(std::to_wstring(scale_));
    if(m.paths!=identity||(m.snapshot.get()&&m.preset!=c.preset)||size.width!=m.size.width||size.height!=m.size.height||!motion::SystemAnimationsEnabled()) {
        m.snapshot.reset();m.start=0;m.before.clear();m.previous=c.zoom_index;
    }
    if(c.zoom_index!=m.previous) {
        if(c.zoom_index>=0&&!m.last.empty()) {
            auto* dc=compositor_->Dc();ComPtr<ID2D1Image> image;dc->GetTarget(&image);
            ComPtr<ID2D1Bitmap1> target;
            if(image.get()&&SUCCEEDED(image->QueryInterface(IID_PPV_ARGS(&target)))) {
                D2D1_BITMAP_PROPERTIES1 props{};props.pixelFormat=target->GetPixelFormat();props.dpiX=props.dpiY=96;
                ComPtr<ID2D1Bitmap1> captured;
                dc->Flush();
                const bool switching=m.previous>=0&&m.snapshot.get()&&m.before.size()>=c.pane_names.size();
                auto captureSize=target->GetPixelSize();if(switching)captureSize.height*=2;
                if(SUCCEEDED(dc->CreateBitmap(captureSize,nullptr,0,&props,&captured))) {
                    HRESULT copied=E_FAIL;
                    if(switching) {
                        const auto original=D2D1::RectU(0,0,size.width,size.height);
                        const auto offset=D2D1::Point2U(0,size.height);
                        copied=captured->CopyFromBitmap(nullptr,m.snapshot.get(),&original);
                        if(SUCCEEDED(copied))copied=captured->CopyFromBitmap(&offset,target.get(),nullptr);
                    } else copied=captured->CopyFromBitmap(nullptr,target.get(),nullptr);
                    if(SUCCEEDED(copied)) {
                        m.snapshot=std::move(captured);
                        if(switching) {
                            m.before.resize(c.pane_names.size());m.indices.resize(c.pane_names.size());
                            auto source=m.last.front();source.top+=size.height;source.bottom+=size.height;
                            m.before.push_back(source);m.indices.push_back(m.previous);
                        } else {
                            m.before=m.last;m.indices.clear();
                            for(size_t i=0;i<m.before.size();++i)m.indices.push_back(static_cast<int>(i));
                        }
                        m.start=motion_now_;m.restoring=false;m.excluded=c.zoom_index;m.preset=c.preset;
                    }
                }
            }
        } else if(c.zoom_index<0&&m.snapshot.get()&&m.before.size()>1) {
            m.start=motion_now_;m.restoring=true;m.excluded=m.previous;
        } else m.start=0;
        m.previous=c.zoom_index;
    }
    if(m.start&&motion_now_-m.start>=280)m.start=0;
    m.last.clear();for(const auto& slot:vm.pane_slots)m.last.push_back(slot.rect);
    m.paths=std::move(identity);m.size=size;
}
void MainRenderer::DrawPaneZoomMotion(const WindowViewModel& vm,const D2D1_RECT_F& full) {
    auto& m=pane_zoom_motion_;if(!m.start||!m.snapshot.get())return;
    const float t=std::clamp(float(motion_now_-m.start)/280.0f,0.0f,1.0f),p=ZoomEase(t);
    auto map=PaneLayoutStripBounds(vm,full,scale_,compositor_->DwriteFactory(),compositor_->SmallFormat());
    map=PaneLayoutMiniBounds(map,scale_);
    const auto cells=PaneLayoutCells(m.preset,map,2*scale_);
    auto* dc=compositor_->Dc();
    for(size_t i=0;i<m.before.size();++i) {
        const int index=m.indices[i];if(index==m.excluded||index<0||static_cast<size_t>(index)>=cells.size())continue;
        const bool switched=m.before.size()>cells.size();
        if(switched&&((m.restoring&&i>=cells.size())||(!m.restoring&&i<cells.size())))continue;
        const auto source=m.before[i]; const auto cell=cells[static_cast<size_t>(index)];
        auto screenSource=source;
        if(switched&&i>=cells.size()){screenSource.top-=m.size.height;screenSource.bottom-=m.size.height;}
        const auto from=m.restoring?cell:screenSource,to=m.restoring?screenSource:cell;
        auto dest=D2D1::RectF(from.left+(to.left-from.left)*p,from.top+(to.top-from.top)*p,
            from.right+(to.right-from.right)*p,from.bottom+(to.bottom-from.bottom)*p);
        const float fade=ZoomEase(std::clamp(t*280/220,0.0f,1.0f),.25f,.1f,.25f,1);
        const float opacity=m.restoring?fade:1-fade;
        dc->DrawBitmap(m.snapshot.get(),dest,opacity,D2D1_INTERPOLATION_MODE_LINEAR,&source);
    }
    RequestMotionFrame();
}
}
