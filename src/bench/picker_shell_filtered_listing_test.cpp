#include "../ui/picker_shell_filtered_listing.h"
#include "../ui/ui_view_model.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace pulse::ui;
namespace {
unsigned checks=0,failures=0;
void Check(bool ok,const char* label){++checks;failures+=!ok;std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);}
PickerListing Fixture(){
 PickerListing listing;listing.generation=19;listing.path=L"C:\\owned-fixture";
 auto rows=std::make_shared<std::vector<pulse::fs::DirEntry>>();
 const wchar_t* names[]{L"blocked.txt",L"visible.txt",L"allowedDir",L"hr2.txt",L"denied.txt"};
 const size_t source[]{80,5,117,3,54};
 for(size_t i=0;i<5;++i){
  PickerEntry entry;entry.name=names[i];entry.path=listing.path+L"\\"+entry.name;entry.kind=i==2?PickerEntryKind::Folder:PickerEntryKind::File;entry.source=source[i];entry.size=100+i;entry.modified.dwLowDateTime=static_cast<DWORD>(700+i);
  pulse::fs::DirEntry row;row.name=entry.name;row.full_path=entry.path;row.shell_id=L"owned-shell-identity-"+std::to_wstring(i);row.attrs=i==2?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_ARCHIVE;row.is_dir=i==2;row.size=entry.size;row.mtime=entry.modified;row.reparse_tag=static_cast<DWORD>(1000+i);
  rows->push_back(std::move(row));listing.entries.push_back(std::move(entry));
 }
 listing.snapshot=std::move(rows);return listing;
}
std::vector<std::wstring> Paths(const PickerListing& listing){std::vector<std::wstring> paths;for(const auto& entry:listing.entries)paths.push_back(entry.path);return paths;}
HRESULT Verdict(const std::wstring& path){return path.ends_with(L"blocked.txt")?S_FALSE:path.ends_with(L"hr2.txt")?HRESULT(2):path.ends_with(L"denied.txt")?E_ACCESSDENIED:S_OK;}
}
int main(){
 auto listing=Fixture();const auto original=listing.snapshot;const auto before=Paths(listing);std::vector<std::wstring> calls;
 const auto filtered=FilterPickerShellListing(listing,[&](const std::wstring& path){calls.push_back(path);return Verdict(path);},[]{return true;});
 Check(filtered==S_OK&&listing.entries.size()==4,"selection and typeahead entries remove exact-S_FALSE item");
 Check(calls==before,"caller filter runs once per original item in existing sorted listing order");
 PaneViewModel pane;pane.path=listing.path;pane.snapshot=listing.snapshot;pane.entries.clear();
 // SyncVm publishes this snapshot. Shared MakeVisibleEntry reads (*snapshot)[index]
 // and EntryCount chooses snapshot size, not the separate PickerEntry vector.
 std::printf("RENDER selection_rows=%zu visible_rows=%zu first=%ls\n",listing.entries.size(),pane.EntryCount(),pane.snapshot&&!pane.snapshot->empty()?pane.snapshot->front().name.c_str():L"<empty>");
 Check(pane.EntryCount()==4,"real pane EntryCount hides the denied row rather than drawing old snapshot size");
 bool names=listing.snapshot&&listing.snapshot->size()==listing.entries.size();
 if(names)for(size_t i=0;i<listing.entries.size();++i)names&=(*listing.snapshot)[i].name==listing.entries[i].name;
 Check(names,"published draw snapshot and selection entries have identical row order and identity");
 Check(listing.snapshot&&listing.snapshot->front().name==L"visible.txt","renderer snapshot first row is visible.txt not the denied blocked.txt");
 Check(pane.SourceIndex(0)==0&&listing.snapshot&&listing.snapshot->front().full_path==listing.entries.front().path,"actual pane hit-source index resolves the same selection and draw object");
 bool metadata=listing.snapshot&&listing.snapshot->size()==4;
 if(metadata)for(size_t i=0;i<4;++i){const auto& row=(*listing.snapshot)[i];const auto& old=(*original)[i+1];metadata&=row.name==old.name&&row.attrs==old.attrs&&row.size==old.size&&row.mtime.dwLowDateTime==old.mtime.dwLowDateTime&&row.reparse_tag==old.reparse_tag&&row.full_path==old.full_path&&row.shell_id==old.shell_id&&&row.extra_.Get()!=&old.extra_.Get();}
 Check(metadata,"snapshot projection preserves logical identity and metadata with independent deep-copied extras");
 Check(listing.entries[0].source==5&&listing.entries[1].source==117,"projection uses current row positions, never raw enumeration source indices");
 Check(listing.snapshot!=original&&original->size()==5&&original->front().name==L"blocked.txt","new immutable renderer snapshot invalidates row cache without mutating shared source snapshot");
 Check(listing.generation==19&&listing.path==L"C:\\owned-fixture","listing folder and generation are unchanged");
 {
  auto all=Fixture();const auto snapshot=all.snapshot;unsigned count=0;const auto result=FilterPickerShellListing(all,[&](const auto&)->HRESULT{++count;return E_ACCESSDENIED;},[]{return true;});
  Check(result==S_OK&&count==5&&all.entries.size()==5&&all.snapshot==snapshot,"native failed IncludeItem verdict includes all and preserves unchanged snapshot identity");
 }
 {
  auto empty=Fixture();const auto snapshot=empty.snapshot;const auto result=FilterPickerShellListing(empty,[](const auto&)->HRESULT{return S_FALSE;},[]{return true;});PaneViewModel view;view.snapshot=empty.snapshot;
  Check(result==S_OK&&empty.entries.empty()&&view.EntryCount()==0,"all-excluded listing publishes zero renderer rows as well as zero selection rows");Check(snapshot->size()==5,"all-excluded projection does not edit another reader's source snapshot");
 }
 {
  auto stale=Fixture();const auto snapshot=stale.snapshot;const auto paths=Paths(stale);bool current=true;unsigned count=0;const auto result=FilterPickerShellListing(stale,[&](const auto&)->HRESULT{if(++count==2)current=false;return S_FALSE;},[&]{return current;});
  Check(result==HRESULT_FROM_WIN32(ERROR_RETRY)&&stale.snapshot==snapshot&&Paths(stale)==paths,"stale or cancellation leaves both original publication channels intact");
 }
 {
  auto malformed=Fixture();auto short_rows=std::make_shared<std::vector<pulse::fs::DirEntry>>(*malformed.snapshot);short_rows->pop_back();malformed.snapshot=short_rows;unsigned count=0;
  const auto result=FilterPickerShellListing(malformed,[&](const auto&)->HRESULT{++count;return S_OK;},[]{return true;});
  Check(result==E_UNEXPECTED&&count==0&&malformed.entries.size()==5&&malformed.snapshot==short_rows,"mismatched row channels fail before callbacks or publication");
 }
 std::printf("SHELL_FILTER_LISTING checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
