#include "../ui/file_save_access_policy.h"
#include <filesystem>
#include <cstdio>
#include <thread>
#include <atomic>
using namespace pulse::ui;
int main() {
    const auto root=std::filesystem::absolute(L"bench_data")/(L"picker-cleanup-delay-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    if(!CreateDirectoryW(root.c_str(),nullptr))return 2;
    int failures=0;
    for(const unsigned delay:{40u,400u}) {
        const auto path=root/(std::to_wstring(delay)+L".txt");
        SaveTargetRequest request;request.directory=root.wstring();request.filename=path.filename().wstring();
        auto snapshot=CaptureSaveConfirmation(request);if(!snapshot.snapshot)return 2;
        HANDLE held=INVALID_HANDLE_VALUE;std::thread release;
        const auto result=ValidateLocalSaveAccessV1(*snapshot.snapshot,{},[&](SaveAccessStage stage){
            if(stage==SaveAccessStage::Created)held=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
            if(stage==SaveAccessStage::AfterClose&&held!=INVALID_HANDLE_VALUE)release=std::thread([held,delay]{Sleep(delay);CloseHandle(held);});
        });
        if(release.joinable())release.join();
        const bool absent=GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND;
        const bool okay=held!=INVALID_HANDLE_VALUE&&absent&&(delay==40
            ?result.state==SaveAccessState::Ready&&result.cleanup==SaveAccessCleanup::Confirmed
            :result.state==SaveAccessState::Failed&&result.cleanup==SaveAccessCleanup::Unconfirmed&&result.cleanup_error!=0);
        std::printf("[%s] metadata holder delay=%u state=%d cleanup=%d error=%lu; absent=%d\n",okay?"PASS":"FAIL",delay,static_cast<int>(result.state),static_cast<int>(result.cleanup),result.error,absent);
        failures+=!okay;
    }
    const bool cleaned=RemoveDirectoryW(root.c_str())!=FALSE;failures+=!cleaned;
    std::printf("FAILURES=%d cleaned=%d\n",failures,cleaned);return failures?1:0;
}
