#include "../app/tree_transfer_destination.h"
#include "../common/localization.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <objbase.h>
using namespace pulse;
namespace {
int passes=0,failures=0;
void Check(bool ok,const char* text){std::printf("[%s] %s\n",ok?"PASS":"FAIL",text);ok?++passes:++failures;}
std::wstring dialog_snapshot;
bool dialog_dark=false;
void CALLBACK CaptureConfirm(HWND window,UINT,UINT_PTR timer,DWORD) {
    KillTimer(window,timer);
    Check(ui::SaveConfirmDialogFrame(window,dialog_snapshot),"existing live confirmation renders long-path snapshot");
    if(dialog_dark) PostMessageW(window,WM_KEYDOWN,VK_RETURN,0);
    else {PostMessageW(window,WM_CLOSE,0,0);PostMessageW(window,WM_CLOSE,0,0);}
}
bool Wait(ops::OpsManager& manager,uint64_t task){
    for(int i=0;i<1000;++i){if(manager.FinalStatus(task))return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));}return false;
}
void Write(const std::filesystem::path& p){std::ofstream out(p,std::ios::binary);out<<"tree destination fixture";}
}
int main(int argc, char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr),L"en-US");
    const auto root=std::filesystem::absolute(std::filesystem::path("bench_data/tree_transfer_feedback/destination")/("fixture-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())));
    std::filesystem::create_directories(root/"tree/nested/A");std::filesystem::create_directories(root/"tree/nested/B");std::filesystem::create_directories(root/"source");
    const auto a=(root/"tree/nested/A").wstring(),b=(root/"tree/nested/B").wstring(),current=(root/"tree").wstring();
    if (argc == 2 && std::string(argv[1]) == "--same-directory-only") {
        const auto folder = root / "tree/node_modules";
        std::filesystem::create_directories(folder / "nested");
        Write(folder / "nested/item.txt");
        const std::vector<std::wstring> sources{folder.wstring()};
        const auto selection = app::ResolveTreeTransferDestinations(true, current, {{folder.wstring(), true, false}});
        const auto paste = app::ResolveClipboardTransferDestinations(current, sources, selection);
        auto plan = app::PlanTreeTransfer(sources, paste, false, true, 0);
        Check(plan.requests.size() == 1 && plan.requests[0].dest_dir == current,
              "Ctrl+C/V selected source folder resolves current directory");
        ops::OpsManager manager; manager.Start([]{}, false);
        bool copied = false;
        if (!plan.requests.empty()) {
            const auto task = manager.Submit(std::move(plan.requests[0]));
            if (Wait(manager, task)) {
                for (const auto& entry : std::filesystem::directory_iterator(root / "tree")) {
                    if (entry.path() == folder || !entry.is_directory()) continue;
                    std::ifstream file(entry.path() / "nested/item.txt", std::ios::binary);
                    std::string data((std::istreambuf_iterator<char>(file)), {});
                    if (data == "tree destination fixture") copied = true;
                }
            }
        }
        Check(copied && std::filesystem::exists(folder / "nested/item.txt"),
              "real OpsManager creates sibling duplicate and preserves original");
        manager.Stop();
        auto resolve = [&](std::wstring here, std::vector<std::wstring> src, app::TreeTransferDestinations dest) {
            return app::ResolveClipboardTransferDestinations(here, src, dest);
        };
        Check(resolve(current, sources, {{a}, false}).folders == std::vector<std::wstring>{a}, "different selected target preserved");
        Check(resolve(current, sources, {{a,b}, false}).folders.size() == 2, "explicit fanout preserved");
        Check(!app::PlanTreeTransfer(sources, resolve(folder.wstring(), sources, selection), false, true, 0).invalid_target.empty(), "paste inside source still rejected");
        Check(!app::PlanTreeTransfer(sources, resolve(current, sources, {{(folder / "nested").wstring()}, false}), false, true, 0).invalid_target.empty(), "descendant target still rejected");
        Check(!app::PlanTreeTransfer(sources, resolve(current, sources, {{folder.wstring(),a}, false}), false, true, 0).invalid_target.empty(), "mixed self and other destination still rejected atomically");
        Check(resolve(current, sources, {{folder.wstring()}, true}).invalid, "invalid selection preserved");
        Check(resolve(current, {a,b}, {{a,b}, false}).folders == std::vector<std::wstring>{current}, "multiple copied folders use current directory once");
        Check(resolve(current, sources, {{L"\\\\?\\" + folder.wstring() + L"\\"}, false}).folders == std::vector<std::wstring>{current}, "extended path and trailing separator normalize");
        auto cut = app::PlanTreeTransfer(sources, paste, true, true, 0);
        Check(cut.requests.size() == 1 && cut.requests[0].type == ops::OpType::Move, "same-directory cut retains move no-op semantics");
        CoUninitialize();
        std::printf("RESULT passed=%d failed=%d\n", passes, failures);
        return failures ? 1 : 0;
    }
    auto single=app::ResolveTreeTransferDestinations(true,current,{{a,true,false}});
    Check(single.folders==std::vector<std::wstring>{a},"single nested tree folder uses full child path");
    auto mixed=app::ResolveTreeTransferDestinations(true,current,{{(root/"note.txt").wstring(),false,false},{a,true,false}});
    Check(mixed.folders==single.folders,"mixed file and one folder resolves only folder");
    auto targets=app::ResolveTreeTransferDestinations(true,current,{{a,true,false},{b,true,false},{L"file.txt",false,false}});
    Check(targets.folders==std::vector<std::wstring>{a,b},"multiple folders remain distinct fanout targets");
    auto normal=app::ResolveTreeTransferDestinations(false,current,{{a,true,false},{b,true,false}});
    Check(normal.folders==std::vector<std::wstring>{current},"non-tree selection preserves current-directory destination");
    auto nofolder=app::ResolveTreeTransferDestinations(true,current,{{L"file.txt",false,false},{L"loading",true,true}});
    Check(nofolder.folders==normal.folders,"file-only and placeholder selection preserve current directory");
    Check(app::ResolveTreeTransferDestinations(true,current,{{a,true,false},{a+L"\\",true,false}}).folders.size()==1,"duplicate destination spelling does not duplicate submission");
    const auto source=root/"source/item.txt";Write(source);
    auto move=app::PlanTreeTransfer({source.wstring()},single,true,true,123);
    Check(move.requests.size()==1&&move.requests[0].type==ops::OpType::Move&&move.requests[0].undo_group==0&&move.requests[0].undo_label.empty(),"single-folder move retains type and default undo metadata");
    auto plain=app::PlanTreeTransfer({source.wstring()},normal,false,true,123);
    Check(plain.requests.size()==1&&plain.requests[0].type==ops::OpType::Copy&&plain.requests[0].dest_dir==current&&plain.requests[0].undo_label.empty(),"non-tree paste keeps existing copy destination and undo label");
    auto fanout=app::PlanTreeTransfer({source.wstring()},targets,true,true,123);
    Check(fanout.requests.size()==2&&fanout.requests[0].type==ops::OpType::Copy&&fanout.requests[1].type==ops::OpType::Copy&&fanout.requests[0].undo_group==123&&fanout.requests[1].undo_group==123,"cut/move fanout becomes copy with one undo group");
    const auto spec=BuildTreeTransferConfirmation(targets);
    Check(spec.items==targets.folders&&spec.default_choice==ui::ConfirmChoice::Cancel&&spec.item_checks==nullptr,"existing confirmation lists every target and defaults to cancel");
    Check(spec.message.find(L"{n}")==std::wstring::npos&&!spec.note.empty()&&!spec.title.empty()&&!spec.confirm_text.empty(),"localized confirmation resolves count and explains source preservation");
    int confirmations=0,submissions=0;
    auto count=app::SubmitTreeTransferPlan(fanout,[&]{++confirmations;return false;},[&](ops::OpRequest){++submissions;return true;});
    Check(!count&&confirmations==1&&submissions==0&&std::filesystem::exists(source)&&!std::filesystem::exists(root/"tree/nested/A/item.txt"),"cancel or close invokes no submissions and changes no files");
    confirmations=0;submissions=0;
    count=app::SubmitTreeTransferPlan(fanout,[&]{++confirmations;return true;},[&](ops::OpRequest){++submissions;return true;});
    Check(count==2&&confirmations==1&&submissions==2,"one confirmation submits each target exactly once");
    Check(app::PlanTreeTransfer({a},targets,false,true,123).requests.empty(),"self target rejects entire fanout before any submit");
    Check(app::PlanTreeTransfer({(root/"tree").wstring()},targets,false,true,123).requests.empty(),"descendant targets reject entire plan");
    auto invalid=app::ResolveTreeTransferDestinations(true,current,{{a,true,false},{L"",true,false}});
    Check(invalid.invalid&&app::PlanTreeTransfer({source.wstring()},invalid,false,true,123).requests.empty(),"invalid selected folder rejects all instead of silently skipping it");
    for (bool dark : {false,true}) {
        auto long_targets = targets;
        long_targets.folders.push_back((root / std::wstring(120,L'x') / L"long-target-folder").wstring());
        auto live = BuildTreeTransferConfirmation(long_targets);
        dialog_snapshot=(root/(dark?L"confirm-dark.png":L"confirm-light.png")).wstring();
        dialog_dark=dark;
        live.on_open = [](HWND window) {
            if(window) SetTimer(window,0x5152,600,CaptureConfirm);
        };
        submissions=0;
        const auto submitted=app::SubmitTreeTransferPlan(fanout,[&]{
            return ui::ShowConfirmDialogEx(nullptr,live,dark,D2D1::ColorF(0x0078D4))==ui::ConfirmChoice::Confirm;
        },[&](ops::OpRequest){++submissions;return true;});
        Check(!submitted&&!submissions,dark?"actual default Enter cancels without submits":"actual close/double-close cancels without submits");
    }
    ops::OpsManager manager;manager.Start([]{},false);
    std::vector<uint64_t> tasks;
    app::SubmitTreeTransferPlan(fanout,[]{return true;},[&](ops::OpRequest req){const auto id=manager.Submit(std::move(req));tasks.push_back(id);return id!=0;});
    bool finished=true;for(auto id:tasks)finished=Wait(manager,id)&&finished;
    Check(finished&&std::filesystem::exists(root/"tree/nested/A/item.txt")&&std::filesystem::exists(root/"tree/nested/B/item.txt")&&std::filesystem::exists(source),"real OpsManager copies to both targets and preserves cut source");
    const auto source2=root/"source/partial.txt";Write(source2);const auto bad=root/"not-a-folder";Write(bad);
    auto partial=app::PlanTreeTransfer({source2.wstring()},{{a,bad.wstring()},false},true,true,124);tasks.clear();
    app::SubmitTreeTransferPlan(std::move(partial),[]{return true;},[&](ops::OpRequest req){const auto id=manager.Submit(std::move(req));tasks.push_back(id);return id!=0;});
    finished=true;for(auto id:tasks)finished=Wait(manager,id)&&finished;
    Check(finished&&std::filesystem::exists(root/"tree/nested/A/partial.txt")&&std::filesystem::exists(source2)&&!std::filesystem::exists(bad/"partial.txt"),"partial target failure keeps successful copy and original source");
    const auto later=root/"later";std::filesystem::create_directory(later);
    auto later_move=app::PlanTreeTransfer({source.wstring()},{{later.wstring()},false},true,true,125);
    const auto id=manager.Submit(std::move(later_move.requests.front()));
    Check(Wait(manager,id)&&!std::filesystem::exists(source)&&std::filesystem::exists(later/"item.txt"),"later single-target cut paste still moves original after fanout");
    manager.Stop();CoUninitialize();
    std::printf("RESULT passed=%d failed=%d\n",passes,failures);return failures?1:0;
}
