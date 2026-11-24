#define PULSE_DESKTOP_DRAG_TEST 1
#define PULSE_DESKTOP_PREVIEW_TEST 1
#define main DesktopPluginEntryForPreviewTest
#include "host/host_main.cpp"
#undef main
namespace {
struct DesktopPreviewHostTestPeer {
    static int Run() {
        int count=0,failed=0;
        auto check=[&](bool ok,const char* name){++count;failed+=!ok;std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);};
        DesktopHostSession host({},0);
        const std::wstring paths[]={L"C:\\fixture\\报告.pdf",L"\\\\server\\share\\a b.docx",L"\\\\?\\C:\\fixture\\image.png"};
        for(const auto& path:paths) {
            host.test_preview_request_.clear();
            host.OpenQuickLookForPath(path);
            std::string routed;
            ipc::JsonFindString(host.test_preview_request_,"path",routed);
            std::string mode;
            ipc::JsonFindString(host.test_preview_request_,"mode",mode);
            check(ipc::Utf8ToWide(routed)==path && mode=="preview",
                  "file outside desktop snapshot reaches direct Pulse full-preview mode");
            check(!host.overlays_.IsOpen(),"file preview does not open the desktop thumbnail/excerpt overlay");
        }
        host.test_preview_request_.clear();
        host.interaction_.focused_path=paths[0];
        bool handled=false;
        host.OnSurfaceMessage(nullptr,WM_KEYDOWN,VK_SPACE,0,handled);
        check(handled&&!host.test_preview_request_.empty(),"Space dispatch requests Pulse preview for focused file");
        model::DesktopItem grouped;
        grouped.path=paths[0];grouped.name=L"报告.pdf";grouped.pinned_group_id="rule";
        host.state_.items.push_back(grouped);
        host.test_preview_request_.clear();host.OpenQuickLookForPath(grouped.path);
        check(!host.test_preview_request_.empty()&&!host.overlays_.IsOpen(),
              "file inside rule group uses the same full-preview route");
        for(const auto& path:{L"",L"relative.pdf",L"C:\\bad\nname.pdf"}) {
            host.test_preview_request_.clear();host.OpenQuickLookForPath(path);
            check(host.test_preview_request_.empty(),"invalid or empty preview path is ignored");
        }
        auto hover_mode=[&] { std::string value;ipc::JsonFindString(host.test_hover_request_,"mode",value);return value; };
        auto hovered_path=[&] { std::string value;ipc::JsonFindString(host.test_hover_request_,"path",value);return ipc::Utf8ToWide(value); };
        host.state_={};host.state_.settings.organize_enabled=true;host.state_.settings.tiles_enabled=true;
        grouped.pinned_group_id.clear();grouped.force_loose=true;grouped.loose_pos={24,44};
        host.state_.items={grouped};host.group_views_.clear();
        host.OnSurfaceMessage(nullptr,WM_MOUSEMOVE,0,MAKELPARAM(32,52),handled);
        check(hover_mode()=="hover_preview"&&hovered_path()==grouped.path,"loose file mouse-move sends hover candidate");
        host.test_hover_request_.clear();host.UpdateHoverPreviewAt(33,53);
        check(host.test_hover_request_.empty(),"same file hover does not flood requests or reset host dwell");
        host.OnSurfaceMessage(nullptr,WM_MOUSELEAVE,0,0,handled);
        check(hover_mode()=="dismiss_hover_preview"&&hovered_path()==grouped.path,"mouse leave cancels matching hover path");
        host.UpdateHoverPreviewAt(32,52);host.test_hover_request_.clear();
        host.interaction_.dragging_items=true;host.UpdateHoverPreviewAt(32,52);
        check(hover_mode()=="dismiss_hover_preview","drag start suppresses hover preview");
        host.interaction_.dragging_items=false;
        host.UpdateHoverPreviewAt(32,52);host.test_hover_request_.clear();host.state_.settings.icons_hidden=true;
        host.UpdateHoverPreviewAt(32,52);
        check(hover_mode()=="dismiss_hover_preview","hidden desktop cancels preview");
        host.state_.settings.icons_hidden=false;
        host.state_.items[0].force_loose=false;host.state_.items[0].pinned_group_id="rule";
        model::DesktopGroup rule;rule.id="rule";rule.x=240;rule.y=44;rule.cols=2;
        host.state_.groups={rule};host.group_views_=model::ComputeGroupViews(host.state_,0);
        const auto cell=model::GroupItemRect(host.group_views_[0],0);
        host.UpdateHoverPreviewAt(cell.x+8,cell.y+8);
        check(!cell.empty()&&hover_mode()=="hover_preview"&&hovered_path()==grouped.path,
              "rule-group file uses shared item hit geometry for hover");
        host.CancelHoverPreview();host.state_.groups.clear();host.group_views_.clear();host.state_.items.clear();
        model::DesktopTile tile;tile.id="downloads";tile.kind=model::TileKind::Downloads;tile.size={2,2};
        tile.free_placed=true;tile.x=24;tile.y=44;tile.w=tile.h=208;tile.live_content=true;tile.look.show_title=true;
        host.state_.tiles={tile};mount::MiniFolderItem row;row.path=paths[1];row.name=L"a b.docx";
        host.live_data_.download_items={row};
        const auto rr=render::DesktopRenderer::ListRowRect(tile.kind,D2D1::RectF(24,44,232,252),0,true);
        host.UpdateHoverPreviewAt(static_cast<int>(rr.left+10),static_cast<int>((rr.top+rr.bottom)/2));
        check(hover_mode()=="hover_preview"&&hovered_path()==row.path,"Downloads row sends its real file path");
        host.test_hover_request_.clear();host.OpenQuickLookForPath(row.path);
        check(hover_mode()=="dismiss_hover_preview"&&!host.test_preview_request_.empty(),
              "explicit preview cancels pending hover before opening full preview");
        std::printf("Desktop Pulse preview: %d cases, %d failures\n",count,failed);return failed?1:0;
    }
};
}
int main(){CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);const int r=DesktopPreviewHostTestPeer::Run();CoUninitialize();return r;}
