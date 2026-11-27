#include "setup_dialog_admin_task_transaction.h"
#include "setup_common.h"
#include "setup_log.h"
#include "../common/dialog_admin_trust.h"
#include <cstring>
namespace pulse::setup {
namespace {
bool Failure(std::wstring& error,HRESULT hr,const wchar_t* stage=L"recover"){error=Win32Error(HRESULT_FACILITY(hr)==FACILITY_WIN32?HRESULT_CODE(hr):static_cast<DWORD>(hr));Log(L"Admin auto task transaction failed: "+std::to_wstring(static_cast<unsigned long>(hr))+L" stage="+stage);return false;}
struct Header { DWORD magic=0x31544150,exists=0,running=0,units=0; };
}
bool DialogAdminTaskTransaction::Prepare(std::wstring& error,bool updates_task){
 if(!real_)return true;
 if(!updates_task){
  Log(L"Admin auto task snapshot skipped: package does not update the legacy task");
  return true;
 }
 dialog_admin_auto::Snapshot before;
 HRESULT hr=S_OK;
 if(api_.capture){
  hr=api_.capture(before);
 }else if(SUCCEEDED(dialog_admin_auto::ValidateInstalledRoot(root_,false))){
  hr=dialog_admin_auto::Inspect(root_,before);
  if(hr==HRESULT_FROM_WIN32(ERROR_LOGON_TYPE_NOT_GRANTED)||hr==HRESULT_FROM_WIN32(ERROR_NO_SUCH_LOGON_SESSION)){
   Log(L"Admin auto task snapshot skipped for session: "+std::to_wstring(static_cast<unsigned long>(hr)));
   before={};
   hr=S_OK;
  }
 }
 if(FAILED(hr))return Failure(error,hr,L"capture");
 if(before.root.size()>32768)return Failure(error,E_INVALIDARG,L"snapshot-validation");
 Header header;header.exists=before.exists?1:0;header.running=before.running?1:0;header.units=static_cast<DWORD>(before.root.size());
 const auto path=root_+L"\\.pulse-setup\\admin-auto-task";
 HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return Failure(error,HRESULT_FROM_WIN32(GetLastError()),L"journal-open");
 DWORD written=0;bool ok=WriteFile(file,&header,sizeof(header),&written,nullptr)&&written==sizeof(header);
 if(ok&&header.units)ok=WriteFile(file,before.root.data(),header.units*sizeof(wchar_t),&written,nullptr)&&written==header.units*sizeof(wchar_t);
 if(ok)ok=FlushFileBuffers(file)!=FALSE;const DWORD code=GetLastError();CloseHandle(file);return ok||Failure(error,HRESULT_FROM_WIN32(code?code:ERROR_WRITE_FAULT),L"journal-write-flush");
}
bool DialogAdminTaskTransaction::Recover(std::wstring& error){
 if(!real_)return true;const auto path=root_+L"\\.pulse-setup\\admin-auto-task";
 const DWORD attributes=GetFileAttributesW(path.c_str());
 if(attributes==INVALID_FILE_ATTRIBUTES){const auto code=GetLastError();return code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND||Failure(error,HRESULT_FROM_WIN32(code));}
 if(attributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))return Failure(error,HRESULT_FROM_WIN32(ERROR_REPARSE_TAG_INVALID));
 std::string bytes;if(!ReadWholeFile(path,bytes)||bytes.size()<sizeof(Header)||bytes.size()>sizeof(Header)+65536)return Failure(error,E_INVALIDARG);
 Header header{};std::memcpy(&header,bytes.data(),sizeof(header));if(header.magic!=0x31544150||header.exists>1||header.running>1||header.units>32768||bytes.size()!=sizeof(header)+header.units*sizeof(wchar_t)||(!header.exists&&(header.running||header.units)))return Failure(error,E_INVALIDARG);
 dialog_admin_auto::Snapshot before;before.exists=header.exists!=0;before.running=header.running!=0;before.root.resize(header.units);if(header.units)std::memcpy(before.root.data(),bytes.data()+sizeof(header),header.units*sizeof(wchar_t));
 if(before.root.find(L'\0')!=before.root.npos||(before.exists&&before.root.empty()))return Failure(error,E_INVALIDARG);
 if(api_.restore){const HRESULT hr=api_.restore(before);return SUCCEEDED(hr)||Failure(error,hr);}
 if(!before.exists&&FAILED(dialog_admin_auto::ValidateInstalledRoot(root_,false))){
  DeleteFileW(path.c_str());
  return true;
 }
 const DWORD code=dialog_admin::VerifyProtectedPath(path,false);if(code)return Failure(error,HRESULT_FROM_WIN32(code));
 const HRESULT hr=dialog_admin_auto::Restore(root_,before);
 if(!before.exists&&(hr==HRESULT_FROM_WIN32(ERROR_LOGON_TYPE_NOT_GRANTED)||hr==HRESULT_FROM_WIN32(ERROR_NO_SUCH_LOGON_SESSION)))return true;
 return SUCCEEDED(hr)||Failure(error,hr);
}
}
