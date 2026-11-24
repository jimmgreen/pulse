// Test actual host load/save logic without reading or writing user preferences.
#define PULSE_DESKTOP_DRAG_TEST 1
#define PULSE_DESKTOP_PERSISTENCE_TEST 1
#define main DesktopPluginEntryForPersistenceTest
#include "host/host_main.cpp"
#undef main

namespace {
struct DesktopPersistenceHostTestPeer {
    static int Run() {
        int cases=0,failed=0;
        const auto check=[&](bool ok,const char* label){++cases;failed+=!ok;std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);};
        DesktopHostSession host({},0);
        auto load=[&](std::string text) {
            host.state_={}; host.test_saved_model_="not-written";
            host.test_model_input_=std::move(text);host.LoadOrInitModel();
        };
        model::DesktopModelState cleared;
        cleared.tiles=model::MakeDefaultTiles();cleared.widget_defaults_applied=true;
        cleared.tiles.clear();
        load(model::SerializeModelStateJson(cleared));
        check(host.state_.tiles.empty(),"deleting all tiles survives actual host load");
        check(host.test_saved_model_=="not-written","loading does not rewrite persisted model");
        host.SaveModel();const auto once=host.test_saved_model_;
        load(once);
        check(host.state_.tiles.empty(),"saved empty board remains empty across second restart");
        load(R"({"version":1,"tiles":[]})");
        check(host.state_.tiles.empty(),"explicit empty array without migration marker stays empty");
        load(" \n {\"version\":1, \"t\\u0069les\" : [ ] } \t");
        check(host.state_.tiles.empty(),"escaped top-level tiles key and whitespace preserve empty intent");
        load(R"({"version":1,"settings":{"tiles_enabled":false},"tiles":[]})");
        check(host.state_.tiles.empty()&&!host.state_.settings.tiles_enabled,"disabled empty board stays disabled and empty");
        load(R"({"version":1})");
        check(!host.state_.tiles.empty(),"legacy model missing tiles field receives initial defaults");
        load(R"({"version":1,"metadata":{"tiles":[]}})");
        check(!host.state_.tiles.empty(),"nested tiles key does not suppress legacy initialization");
        load(R"({"version":1,"note":"\"tiles\":[]"})");
        check(!host.state_.tiles.empty(),"text mentioning tiles is not a root field");
        model::DesktopModelState custom;custom.widget_defaults_applied=true;
        model::DesktopTile tile;tile.id="retained-note";tile.kind=model::TileKind::StickyNote;tile.note_body=L"保留内容";
        custom.tiles={tile};load(model::SerializeModelStateJson(custom));
        check(host.state_.tiles.size()==1&&host.state_.tiles[0].id==tile.id&&host.state_.tiles[0].note_body==tile.note_body,
              "nonempty customized board is not replaced or padded");
        for(const auto* broken:{R"({"version":1,"tiles":null})",R"({"version":1,"tiles":[1]})",R"({"version":1,"tiles":[)",R"({"version":999,"tiles":[]})"}) {
            load(broken);check(!host.state_.tiles.empty(),"invalid model follows existing default initialization instead of empty intent");
            check(host.test_saved_model_=="not-written","invalid load never overwrites source automatically");
        }
        load({});check(!host.state_.tiles.empty(),"missing model still creates initial tile board");
        std::printf("Tile persistence host: %d cases, %d failures\n",cases,failed);
        return failed?1:0;
    }
};
}
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const int result=DesktopPersistenceHostTestPeer::Run();
    CoUninitialize();return result;
}

