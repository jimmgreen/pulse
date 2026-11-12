#include "setup_dialog_admin_task_transaction.h"
#include "setup_common.h"
#include "setup_log.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
namespace pulse::setup {
std::wstring Win32Error(DWORD code){return std::to_wstring(code);}
void Log(const std::wstring&){}
bool ReadWholeFile(const std::wstring& path,std::string& bytes){std::ifstream in(std::filesystem::path(path),std::ios::binary);if(!in)return false;bytes.assign(std::istreambuf_iterator<char>(in),{});return true;}
}
using namespace pulse::setup;
int checks=0,failures=0;
void Check(bool good,const char* label){++checks;printf("[%s] %s\n",good?"PASS":"FAIL",label);if(!good)++failures;}
int wmain(){
 const auto root=std::filesystem::absolute(L"bench_data/admin_auto_task/journal-"+std::to_wstring(GetCurrentProcessId()));
 std::filesystem::create_directory(root);std::filesystem::create_directory(root/L".pulse-setup");const auto journal=root/L".pulse-setup/admin-auto-task";
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
 std::filesystem::remove(root/L".pulse-setup");std::filesystem::remove(root);
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
