#include "../common/utf8_file.h"
namespace {
int MenuTreePreferencesTests() {
    using namespace pulse;
    app::ContextMenuPrefs prefs; prefs.persist = false;
    app::ContextMenuController controller;
    controller.StartQuery(prefs, nullptr, {L"C:\\fixture\\sample.tree"}, false, L".tree", false, {}, {});
    auto verb = [](const wchar_t* name, const wchar_t* label, const wchar_t* path = L"") {
        app::StaticVerb row; row.verb = name; row.display = label; row.reg_key = path; row.command = L"fixture.exe %1"; return row;
    };
    auto a = verb(L"vendor-a", L"Vendor A", L"Tree.A\\shell\\vendor");
    auto b = verb(L"vendor-b", L"Vendor B", L"Tree.B\\shell\\vendor");
    auto same = verb(L"run", L"同名");
    auto second = verb(L"second", L"第二项");
    auto pixels = std::make_shared<common::MenuIconPixels>(); pixels->width = pixels->height = 1; pixels->bgra = {1,2,3,255};
    same.icon_pixels = pixels;
    a.children = {same, second}; b.children = {same};
    controller.CompleteStaticVerbs(L".tree", {a,b}, controller.cache_generation());
    bool changed = false;
    auto display = controller.BuildDisplay(prefs, {}, changed);
    const auto roots = prefs.NodeChildren(L"");
    Check(roots.size() == 2 && changed, "tree catalog discovers actual roots through controller Compose");
    if (roots.size() != 2) return 1;
    const auto root_a = roots[0].text == L"Vendor A" ? roots[0] : roots[1];
    const auto root_b = roots[0].text == L"Vendor B" ? roots[0] : roots[1];
    const auto ca = prefs.NodeChildren(root_a.key), cb = prefs.NodeChildren(root_b.key);
    Check(ca.size() == 2 && cb.size() == 1 && ca[0].key != cb[0].key && ca[0].source != cb[0].source,
          "same labels and verbs under different parents and registry sources have separate keys");
    if (ca.size()!=2 || cb.size()!=1) return 1;
    const auto subtree = [](const auto& rows, const wchar_t* name) -> const ui::FluentMenuItem* {
        for (const auto& row:rows) if(row.text==name)return &row; return nullptr;
    };
    Check(prefs.SetNodeEnabled(ca[0].key,false), "child visibility accepts stable catalog identity");
    display=controller.BuildDisplay(prefs,{},changed);
    const auto* da=subtree(display,L"Vendor A"); const auto* db=subtree(display,L"Vendor B");
    Check(da && da->children.size()==1 && db && db->children.size()==1 && db->children[0].icon==pixels,
          "recursive Apply hides only selected parent child and preserves other icon");
    Check(prefs.NodeChildren(root_a.key).size()==2, "hidden child remains discoverable for restoration");
    prefs.SetNodeEnabled(ca[0].key,true);
    Check(prefs.MoveNode(ca[1].key,-1), "sibling move persists selected child order");
    display=controller.BuildDisplay(prefs,{},changed); da=subtree(display,L"Vendor A");
    Check(da && da->children.size()==2 && da->children[0].text==L"第二项", "actual controller output applies sibling order");
    prefs.SetNodeEnabled(ca[0].key,false);
    CreateDirectoryW(L"bench_data",nullptr); CreateDirectoryW(L"bench_data\\context-menu-tree-20261126",nullptr);
    CreateDirectoryW(L"bench_data\\context-menu-tree-20261126\\core",nullptr);
    const std::wstring path=L"bench_data\\context-menu-tree-20261126\\core\\prefs-fixture.json";
    std::wstring saved; app::ContextMenuPrefs loaded; loaded.persist=false;
    Check(WriteUtf8FileAtomic(path,prefs.ToJson()) && ReadUtf8File(path,saved) && loaded.FromJson(saved) &&
          !loaded.NodeEnabled(ca[0].key) && loaded.NodeOrder(root_a.key)==prefs.NodeOrder(root_a.key) &&
          loaded.NodeChildren(root_a.key).size()==2, "UTF8 file save reload preserves tree ancestry hidden and order");
    loaded.SetNodeEnabled(cb[0].key,false);
    Check(loaded.ResetNodeGroup(root_a.key) && loaded.NodeEnabled(ca[0].key) &&
          loaded.NodeOrder(root_a.key).empty() && !loaded.NodeEnabled(cb[0].key), "restore group does not clear another parent choices");
    prefs.SetNodeEnabled(ca[1].key,false);
    display=controller.BuildDisplay(prefs,{},changed);
    Check(!subtree(display,L"Vendor A") && subtree(display,L"Vendor B"), "all hidden descendants remove empty parent");
    prefs.ResetNodeGroup(root_a.key);
    display=controller.BuildDisplay(prefs,{},changed);
    Check(subtree(display,L"Vendor A")!=nullptr, "restoring hidden group recreates parent on next composition");
    // Updated static input changes command slots, not the identity or preference.
    prefs.SetNodeEnabled(ca[0].key,false); std::swap(a.children[0],a.children[1]);
    controller.CompleteStaticVerbs(L".tree",{b,a},controller.cache_generation());
    display=controller.BuildDisplay(prefs,{},changed); da=subtree(display,L"Vendor A");
    Check(da && da->children.size()==1 && da->children[0].command==app::CmdShellStaticBase+ipc::kStaticVerbStride+1,
          "dynamic static command slots refresh while stable child preference remains");
    prefs.ResetNodeGroup(root_a.key);
    const auto current_default=prefs.NodeChildren(root_a.key);
    display=controller.BuildDisplay(prefs,{},changed); da=subtree(display,L"Vendor A");
    Check(current_default.size()==2 && current_default[0].text==L"第二项" && da && da->children[0].text==current_default[0].text,
          "restore uses latest provider sibling order in settings and actual output");
    // COM identity is handler + ancestry + canonical verb, never transient host ID.
    app::ContextMenuController com; uint32_t session=0;
    app::ContextMenuController::ShellOperations operations;
    operations.query=[&](auto,HWND,bool,bool,auto){return ++session;}; com.SetShellOperations(std::move(operations));
    const auto start=[&]{com.ExecuteShellCommand(0,{},{});com.StartQuery(prefs,nullptr,{L"C:\\fixture\\sample.comtree"},false,L".comtree",false,{},{});};
    start(); auto parent=Row(0,L"{TREE-PREFS}",L"vendor",L"COM Vendor",true);
    auto leaf=Row(17,L"{TREE-PREFS}",L"execute",L"Execute",false,true);leaf.depth=1;
    com.CompleteComQuery(session,{parent,leaf},1,false);com.BuildDisplay(prefs,{},changed);
    const auto com_roots=prefs.NodeChildren(L"",ipc::HandlerCatalogKey(L"{TREE-PREFS}"));
    Check(com_roots.size()==1,"handler root navigation locates actual COM tree");
    if(com_roots.empty())return 1;
    const auto com_children=prefs.NodeChildren(com_roots[0].key);
    if(com_children.empty())return 1;
    prefs.SetNodeEnabled(com_children[0].key,false);start();leaf.id=99;leaf.enabled=false;leaf.icon=pixels;
    com.CompleteComQuery(session,{parent,leaf},2,false);display=com.BuildDisplay(prefs,{},changed);
    Check(!subtree(display,L"COM Vendor"),"new COM session IDs do not invalidate hidden child choice");
    prefs.ResetNodeGroup(com_roots[0].key);display=com.BuildDisplay(prefs,{},changed);
    const auto* dc=subtree(display,L"COM Vendor");
    Check(dc && dc->children.size()==1 && dc->children[0].command==app::CmdShellComBase+99 && !dc->children[0].enabled && dc->children[0].icon==pixels,
          "restore uses current COM command not cached identity payload");
    // A provider returns two indistinguishable siblings. Existing hide cannot pick one safely.
    auto twin=leaf;twin.id=100;com.CompleteComQuery(session,{parent,leaf,twin},3,false);
    com.BuildDisplay(prefs,{},changed);
    const auto ambiguous=prefs.NodeChildren(com_roots[0].key);
    Check(ambiguous.size()==1 && !ambiguous[0].configurable && !prefs.SetNodeEnabled(ambiguous[0].key,false),
          "indistinguishable siblings are reported read-only instead of command-keyed");
    prefs.item_enabled[com_children[0].key]=false;
    display=com.BuildDisplay(prefs,{},changed);
    Check(subtree(display,L"COM Vendor")!=nullptr,"ambiguous identity does not apply stale individual hide");
    com.CompleteComQuery(session,{parent,leaf},4,true);display=com.BuildDisplay(prefs,{},changed);
    Check(subtree(display,L"COM Vendor")!=nullptr,"partial reply cannot reactivate previously ambiguous hide");
    app::ContextMenuPrefs compress_prefs; compress_prefs.persist=false;
    app::ContextMenuController compress_controller;
    app::ContextMenuController::ShellOperations compress_operations;
    compress_operations.query=[](auto,HWND,bool,bool,auto){return 71u;};
    compress_controller.SetShellOperations(std::move(compress_operations));
    compress_controller.StartQuery(compress_prefs,nullptr,{L"C:\\fixture\\archive.zip"},false,L".zip",false,{},{});
    auto zip_a=Row(21,L"{ZIP-A}",L"zip-a",L"压缩为 A.zip");
    auto zip_b=Row(22,L"{ZIP-B}",L"zip-b",L"压缩为 B.zip");
    compress_controller.CompleteComQuery(71,{zip_a,zip_b},1,false);
    compress_controller.BuildDisplay(compress_prefs,{},changed);
    const auto compression_roots=compress_prefs.NodeChildren(L"");
    const auto compression_children=compression_roots.empty()?std::vector<app::SeenMenuItem>{}:compress_prefs.NodeChildren(compression_roots[0].key);
    compress_prefs.SetItemEnabled(ipc::HandlerCatalogKey(L"{ZIP-A}"),false);
    display=compress_controller.BuildDisplay(compress_prefs,{},changed);
    bool contains_a=false,contains_b=false;
    for(const auto& root:display)for(const auto& child:root.children){contains_a|=child.command==app::CmdShellComBase+21;contains_b|=child.command==app::CmdShellComBase+22;}
    Check(!contains_a && contains_b,"discovery regrouping never resurrects a disabled compression provider");
    const auto still_children=compression_roots.empty()?std::vector<app::SeenMenuItem>{}:compress_prefs.NodeChildren(compression_roots[0].key);
    Check(compression_children.size()==2 && still_children.size()==2 && compression_children[1].key==still_children[1].key,
          "compression child identity and hidden discovery survive provider filtering");
    app::ShellMenuEntry vendor_a; vendor_a.text=L"7-Zip";vendor_a.clsid=L"{ZIP-A}";vendor_a.from_com=true;
    app::ShellMenuEntry vendor_b; vendor_b.text=L"WinRAR";vendor_b.clsid=L"{ZIP-B}";vendor_b.from_com=true;
    app::ShellMenuEntry action;action.text=L"Action";action.verb=L"action";action.command=6001;action.clsid=L"{ZIP-A}";action.from_com=true;
    vendor_a.children={action};vendor_b.children={action};
    app::ContextMenuPrefs plain;
    auto vendors=app::ApplyExplorerPrefs(plain,{vendor_a,vendor_b});
    Check(vendors.size()==2 && vendors[0].node_key!=vendors[1].node_key && vendors[0].node_configurable && vendors[1].node_configurable,
          "different compression provider trees retain independently configurable parent keys");
    app::ShellMenuEntry loose=action;loose.text=L"压缩为 Loose.zip";loose.verb=L"loose";loose.clsid=L"{ZIP-C}";
    auto before_reparent=app::ApplyExplorerPrefs(plain,{vendor_a,loose});
    std::wstring old_loose_key;
    if(!before_reparent.empty())for(const auto& child:before_reparent[0].children)if(child.verb==L"loose")old_loose_key=child.node_key;
    plain.item_enabled[old_loose_key]=false;
    auto after_reparent=app::ApplyExplorerPrefs(plain,{vendor_b,loose});
    bool loose_visible=false;
    for(const auto& root:after_reparent)for(const auto& child:root.children)loose_visible|=child.verb==L"loose" && child.node_key!=old_loose_key;
    Check(!old_loose_key.empty() && loose_visible,"compression reparent never misapplies hide from a different vendor parent");
    auto deep_leaf=verb(L"deep-run",L"Deep action");
    auto branch=verb(L"branch",L"Branch");branch.children={deep_leaf};
    auto deep_root=verb(L"deep",L"Deep vendor",L"Tree.Deep\\shell\\vendor");deep_root.children={branch};
    controller.CompleteStaticVerbs(L".tree",{deep_root},controller.cache_generation());controller.BuildDisplay(prefs,{},changed);
    const auto all_roots=prefs.NodeChildren(L"");std::wstring deep_key;
    for(const auto& row:all_roots)if(row.text==L"Deep vendor")deep_key=row.key;
    auto branches=prefs.NodeChildren(deep_key);auto leaves=branches.empty()?std::vector<app::SeenMenuItem>{}:prefs.NodeChildren(branches[0].key);
    Check(leaves.size()==1 && leaves[0].depth==2,"recursive discovery records grandchildren with correct parent and depth");
    if(!leaves.empty())prefs.SetNodeEnabled(leaves[0].key,false);
    display=controller.BuildDisplay(prefs,{},changed);
    Check(!subtree(display,L"Deep vendor"),"recursive preference removes newly empty ancestors at every depth");
    // Bound metadata eviction to leaves and preserve configured ancestry.
    app::ContextMenuPrefs bounded;
    app::SeenMenuItem br; br.key=L"n:parent";br.text=L"Parent";br.flyout=true;bounded.RecordSeenNode(br);
    app::SeenMenuItem bc;bc.key=L"n:child";bc.text=L"Child";bc.parent_key=br.key;bc.depth=1;bounded.RecordSeenNode(bc);
    bounded.SetNodeEnabled(bc.key,false);bounded.SetNodeOrder(br.key,{bc.key});
    for(int i=0;i<4100;++i){app::SeenMenuItem row;row.key=L"n:other"+std::to_wstring(i);row.text=L"Other";bounded.RecordSeenNode(std::move(row));}
    Check(bounded.seen.size()<=4096 && bounded.NodeChildren(br.key).size()==1 && !bounded.NodeEnabled(bc.key) &&
          std::any_of(bounded.seen.begin(),bounded.seen.end(),[&](const auto& row){return row.key==br.key;}),
          "catalog retirement preserves configured child parent chain and sibling order");
    app::ShellMenuEntry unknown;unknown.text=L"Unknown provider";unknown.verb=L"unique-now";unknown.from_com=true;
    auto unknown_tree=app::ApplyExplorerPrefs(plain,{unknown});
    Check(unknown_tree.size()==1 && !unknown_tree[0].node_configurable,
          "unknown provider origin is read-only even when its label is currently unique");
    app::ContextMenuPrefs index_prefs;
    app::SeenMenuItem ix;ix.key=L"n:index-a";ix.text=L"A";ix.source=L"registry:0:index-a";
    app::SeenMenuItem iy=ix;iy.key=L"n:index-b";iy.text=L"B";
    index_prefs.RecordSeenNodes({ix,iy});
    std::swap(index_prefs.seen[0],index_prefs.seen[1]);ix.text=L"A updated";
    index_prefs.RecordSeenNodes({ix,iy});
    Check(index_prefs.seen.size()==2 && index_prefs.seen[1].key==ix.key && index_prefs.seen[1].text==ix.text,
          "catalog index validates indices after external reorder");
    auto iz=ix;iz.key=L"n:index-new";iz.text=L"New";index_prefs.seen[0]=iz;
    index_prefs.RecordSeenNodes({iz,ix});
    Check(index_prefs.seen.size()==2 && index_prefs.NodeChildren(L"").size()==2,
          "catalog index detects same-capacity replacement without duplicate admission");
    const auto index_json=index_prefs.ToJson();index_prefs.FromJson(index_json);
    iz.text=L"Reloaded";index_prefs.RecordSeenNodes({iz});
    Check(std::any_of(index_prefs.seen.begin(),index_prefs.seen.end(),[&](const auto& row){return row.key==iz.key && row.text==iz.text;}),
          "catalog index revalidates JSON reload and records fresh metadata");
    index_prefs.seen.clear();index_prefs.RecordSeenNodes({ix});
    app::ContextMenuPrefs another;another.RecordSeenNodes({iy});
    Check(index_prefs.seen.size()==1 && index_prefs.seen[0].key==ix.key && another.seen.size()==1 && another.seen[0].key==iy.key,
          "catalog index never reuses cleared or another prefs catalog state");
    iy.root_key=L"h:{CHANGED}";iy.from_com=true;iy.category=ipc::CtxMenuCategory::Share;
    Check(another.RecordSeenNodes({iy}) && !another.RecordSeenNodes({iy}),
          "catalog metadata-only changes persist once without steady-state dirty churn");
    app::ContextMenuPrefs migrated;
    Check(migrated.FromJson(L"{\"version\":4,\"items\":{\"v:old\":{\"enabled\":false}}}") &&
          migrated.migrated && !migrated.CatalogItemVisible(L"v:old"), "version4 migration retains legacy top-level hide");
    return failures?1:0;
}
}

namespace {
int MenuTreeCpuBench() {
    using namespace pulse;
    app::ContextMenuPrefs prefs; prefs.persist=false;
    app::ContextMenuController controller;
    controller.StartQuery(prefs,nullptr,{L"C:\\fixture\\cpu.tree"},false,L".tree",false,{},{});
    std::vector<app::StaticVerb> roots;
    for(int p=0;p<4;++p) {
        app::StaticVerb root;root.verb=L"provider-"+std::to_wstring(p);root.display=L"Provider "+std::to_wstring(p);
        root.reg_key=L"Cpu.Tree\\shell\\"+root.verb;
        for(int c=0;c<16;++c){app::StaticVerb child;child.verb=L"action-"+std::to_wstring(c);
            child.display=L"Action "+std::to_wstring(c);child.command=L"fixture.exe %1";root.children.push_back(std::move(child));}
        roots.push_back(std::move(root));
    }
    controller.CompleteStaticVerbs(L".tree",roots,controller.cache_generation());
    bool changed=false;controller.BuildDisplay(prefs,{},changed);
    for(size_t i=prefs.seen.size();i<4096;++i){app::SeenMenuItem row;row.key=L"n:benchmark:"+std::to_wstring(i);
        row.text=L"Historical node";row.tree_node=true;row.source=L"registry:0:Benchmark\\shell\\action";
        row.default_order=static_cast<uint32_t>(i);prefs.seen.push_back(std::move(row));}
    for(int i=0;i<5;++i)controller.BuildDisplay(prefs,{},changed);
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    std::vector<double> timings;int changes=0;size_t leaf_count=0;
    for(int i=0;i<200;++i){LARGE_INTEGER a{},b{};QueryPerformanceCounter(&a);
        const auto display=controller.BuildDisplay(prefs,{},changed);QueryPerformanceCounter(&b);
        timings.push_back(1000.0*static_cast<double>(b.QuadPart-a.QuadPart)/static_cast<double>(frequency.QuadPart));
        changes+=changed?1:0;leaf_count=0;for(const auto& row:display)leaf_count+=row.children.size();}
    double total=0;for(double t:timings)total+=t;std::sort(timings.begin(),timings.end());
    std::printf("TREE_CPU iterations=200 parents=4 children_each=16 catalog=%zu leaves=%zu total_ms=%.6f median_ms=%.6f p95_ms=%.6f prefs_changed=%d\n",
        prefs.seen.size(),leaf_count,total,(timings[99]+timings[100])*0.5,timings[189],changes);
    return leaf_count==64 && changes==0?0:1;
}
}
