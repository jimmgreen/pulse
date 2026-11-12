#include "../shell_bridge/collected_defaults_resolver.h"
#include <propsys.h>
#include <propkey.h>
#include <cstdio>
#include <string>
using namespace pulse::shell_bridge;
int failures=0,checks=0;
void Check(bool yes,const char* text){++checks;printf("[%s] %s\n",yes?"PASS":"FAIL",text);if(!yes)++failures;}
int wmain(int argc,wchar_t**){
 CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
 std::vector<PROPERTYKEY> keys;
 if(argc>1){SetCollectedDefaultsTestDelay(1000);ResolveCollectedDefaults(L".jpg",keys,0);return 0;}
 Check(ResolveCollectedDefaults(L"jpg",keys)==E_INVALIDARG,"reject missing dot");
 Check(ResolveCollectedDefaults(L".x/y",keys)==E_INVALIDARG,"reject path input");
 Check(ResolveCollectedDefaults(L".a.b",keys)==E_INVALIDARG,"reject compound input");
 Check(ResolveCollectedDefaults(L".jpg",keys,5001)==E_INVALIDARG,"bounded timeout input");
 for(auto ext:{L".txt",L".jpg",L".docx"}){
  HRESULT hr=ResolveCollectedDefaults(ext,keys,5000);Check(hr==S_OK,"real public schema resolution");
  printf("EXT %ls count=%zu\n",ext,keys.size());
  for(const auto& key:keys){PWSTR name=nullptr;PSGetNameFromPropertyKey(key,&name);printf("KEY %ls\n",name?name:L"?");CoTaskMemFree(name);}
  const size_t expected=ext==std::wstring(L".txt")?0:ext==std::wstring(L".jpg")?13:8;
  Check(keys.size()==expected,"native oracle editable default count");
 }
 keys={PKEY_Title};SetCollectedDefaultsTestDelay(400);const auto start=GetTickCount64();
 HRESULT hr=ResolveCollectedDefaults(L".jpg",keys,10);
 Check(FAILED(hr)&&GetTickCount64()-start<300,"deadline bounds caller wait");
 Check(keys.size()==1&&IsEqualPropertyKey(keys[0],PKEY_Title),"timeout leaves output unchanged");
 Check(ResolveCollectedDefaults(L".docx",keys,10)==HRESULT_FROM_WIN32(ERROR_BUSY),"no second in-flight worker");
 Sleep(700);SetCollectedDefaultsTestDelay(0);
 Check(ResolveCollectedDefaults(L".txt",keys,5000)==S_OK&&keys.empty(),"worker slot recovers after timed-out job exits");
 CoUninitialize();printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
