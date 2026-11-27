#include "setup_dialog_admin_task_transaction.h"
#include "setup_common.h"
#include "setup_log.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <vector>
namespace { std::vector<std::wstring> messages; }
namespace pulse::setup {
std::wstring Win32Error(DWORD code){return std::to_wstring(code);}
void Log(const std::wstring& message){messages.push_back(message);}
bool ReadWholeFile(const std::wstring& path,std::string& bytes){std::ifstream in(std::filesystem::path(path),std::ios::binary);if(!in)return false;bytes.assign(std::istreambuf_iterator<char>(in),{});return true;}
}
using namespace pulse::setup;
int checks=0,failures=0;
void Check(bool good,const char* label){++checks;printf("[%s] %s\n",good?"PASS":"FAIL",label);if(!good)++failures;}
// Compile the same regression against the pre-fix API for the baseline binary.
template<class Transaction>
bool PrepareForPackage(Transaction& transaction,bool updates_task,std::wstring& error){
 if constexpr(requires { transaction.Prepare(error,updates_task); }) return transaction.Prepare(error,updates_task);
 else return transaction.Prepare(error);
}
int wmain(){
 const auto root=std::filesystem::absolute(L"bench_data/admin_auto_task/journal-"+std::to_wstring(GetCurrentProcessId()));
 std::filesystem::create_directories(root);std::filesystem::create_directory(root/L".pulse-setup");const auto journal=root/L".pulse-setup/admin-auto-task";
 for(int mode=0;mode<4;++mode){
  pulse::dialog_admin_auto::Snapshot before;before.exists=mode!=0;before.running=mode==2;before.root=before.exists?L"C:\\Program Files\\OldPulse":L"";int captures=0,restores=0;
  DialogAdminTaskApi api{[&](auto& value){++captures;value=before;return S_OK;},[&](const auto& value){++restores;Check(value.exists==before.exists&&value.running==before.running&&value.root==before.root,"journal restores prior task identity/state only");return mode==3?E_ACCESSDENIED:S_OK;}};
  DialogAdminTaskTransaction transaction(root.wstring(),true,api);std::wstring error;
  Check(transaction.Prepare(error)&&captures==1,"snapshot journal is durable before mutation");
  DialogAdminTaskTransaction restarted(root.wstring(),true,api);
  Check(restarted.Recover(error)==(mode!=3)&&restores==1,"restart recovery reports backend failure accurately");
  std::filesystem::remove(journal);
 }
 {
  int called=0;DialogAdminTaskApi api{[&](auto&){++called;return S_OK;},[&](const auto&){++called;return S_OK;}};
  DialogAdminTaskTransaction isolated(root.wstring(),false,api);std::wstring error;
  Check(isolated.Prepare(error)&&isolated.Recover(error)&&called==0&&!std::filesystem::exists(journal),"PULSETEST never calls persistent task backend");
  std::ofstream out(journal,std::ios::binary);out<<"arbitrary task XML must not execute";out.close();
  DialogAdminTaskTransaction corrupted(root.wstring(),true,api);
  Check(!corrupted.Recover(error)&&called==0,"corrupt journal rejected before task operation");std::filesystem::remove(journal);
 }
 {
  std::wstring error;
  DialogAdminTaskTransaction prod(root.wstring(),true);
  Check(prod.Prepare(error)&&std::filesystem::exists(journal),"custom install root prepares admin-auto snapshot without Program Files gate");
  DialogAdminTaskTransaction prod_recover(root.wstring(),true);
  Check(prod_recover.Recover(error)&&!std::filesystem::exists(journal),"custom install root recovers interrupted admin-auto journal without VerifyProtectedPath access denied");
  std::ofstream out(journal,std::ios::binary);out<<"corrupt custom journal";out.close();
  DialogAdminTaskTransaction prod_corrupt(root.wstring(),true);
  Check(!prod_corrupt.Recover(error),"corrupt journal rejected on custom install root too");
  std::filesystem::remove(journal);
 }
 {
  int captures=0,restores=0;
  DialogAdminTaskApi denied{[&](auto&){++captures;return E_ACCESSDENIED;},[&](const auto&){++restores;return S_OK;}};
  DialogAdminTaskTransaction compact(root.wstring(),true,denied);std::wstring error;
  Check(PrepareForPackage(compact,false,error)&&captures==0&&error.empty(),
   "compact package never inspects the unrelated inaccessible legacy task");
  Check(!std::filesystem::exists(journal)&&compact.Recover(error)&&restores==0,
   "compact failure recovery does not fabricate an absent-task snapshot or mutate the task");
  messages.clear();
  DialogAdminTaskTransaction legacy(root.wstring(),true,denied);
  Check(!PrepareForPackage(legacy,true,error)&&!error.empty()&&!std::filesystem::exists(journal)&&restores==0,
   "legacy task update still fails closed when its snapshot is denied");
  bool identified=false;for(const auto& message:messages)if(message.find(L"stage=capture")!=message.npos)identified=true;
  Check(identified,"snapshot failure log identifies the capture stage");
 }
 {
  int captures=0,restores=0;
  DialogAdminTaskApi api{[&](auto& before){++captures;before.exists=true;before.root=L"C:\\Program Files\\OldPulse";return S_OK;},
   [&](const auto& before){++restores;return before.exists&&before.root==L"C:\\Program Files\\OldPulse"?S_OK:E_FAIL;}};
  DialogAdminTaskTransaction legacy(root.wstring(),true,api);std::wstring error;
  Check(legacy.Prepare(error),"seed a previous interrupted legacy task journal");
  std::string before,after;ReadWholeFile(journal.wstring(),before);
  DialogAdminTaskTransaction compact(root.wstring(),true,api);
  Check(PrepareForPackage(compact,false,error)&&captures==1,
   "compact selection never overwrites or captures over an existing recovery journal");
  ReadWholeFile(journal.wstring(),after);
  Check(before==after&&compact.Recover(error)&&restores==1,
   "compact install still restores a genuine interrupted legacy task journal");
  std::filesystem::remove(journal);
 }
 std::filesystem::remove(root/L".pulse-setup");std::filesystem::remove(root);
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
