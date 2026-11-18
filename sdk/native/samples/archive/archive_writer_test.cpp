#include "archive_writer.h"
#include "archive_reader.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>

int wmain() {
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH,temp)) return 2;
    const auto root=std::filesystem::path(temp)/(L"PulseWriterTest-"+std::to_wstring(GetCurrentProcessId()));
    const auto source=root/L"source"; const auto stage=root/L"stage";
    std::filesystem::create_directories(source/L"empty"); std::filesystem::create_directory(stage);
    { std::ofstream f(source/L"notes beta.txt",std::ios::binary); f<<"writer round-trip"; }
    int failures=0;
    auto check=[&](bool ok,const char* label){std::printf("[%s] %s\n",ok?"PASS":"FAIL",label); std::fflush(stdout); if(!ok)++failures;};
    uint64_t progress=0;
    auto seven=archive_writer::Create({source.wstring()},stage.wstring(),L"source.7z",archive_writer::Format::SevenZip,5,{},[&](const auto& p){progress=p.completed_bytes;});
    check(seven.status==archive_writer::Status::Ok && std::filesystem::exists(seven.path),"7z writer creates archive");
    auto listing=archive_reader::Read(seven.path);
    check(listing.status==archive_reader::Status::Ok && listing.entries.size()==3,"7z writer output reopens with bundled handler");
    const auto extract=root/L"extract"; std::filesystem::create_directory(extract);
    auto extracted=archive_reader::Extract(seven.path,extract.wstring());
    std::string bytes;
    {std::ifstream f(extract/L"source"/L"notes beta.txt",std::ios::binary); bytes.assign(std::istreambuf_iterator<char>(f),{});}
    check(extracted.status==archive_reader::Status::Ok && bytes=="writer round-trip","7z payload and member round-trip");
    check(progress>0,"writer reports progress");
    const auto zipStage=root/L"zip-stage";
    std::filesystem::create_directory(zipStage);
    auto zip=archive_writer::Create({source.wstring()},zipStage.wstring(),L"source.zip",archive_writer::Format::Zip,3);
    std::ifstream zipFile(zip.path,std::ios::binary); unsigned char signature[4]{}; zipFile.read(reinterpret_cast<char*>(signature),4);
    check(zip.status==archive_writer::Status::Ok && signature[0]=='P' && signature[1]=='K' && signature[2]==3 && signature[3]==4,
        "ZIP writer emits a standard local-file header");
    zipFile.close();
    std::filesystem::create_directory(root/L"cancel");
    auto cancelled=archive_writer::Create({source.wstring()},(root/L"cancel").wstring(),L"cancel.7z",archive_writer::Format::SevenZip,1,[]{return true;});
    check(cancelled.status==archive_writer::Status::Cancelled && std::filesystem::is_empty(root/L"cancel"),"cancelled writer leaves no output");
    bool stop = false;
    auto interrupted=archive_writer::Create({source.wstring()},(root/L"cancel").wstring(),L"cancel.7z",
        archive_writer::Format::SevenZip,1,[&]{return stop;},[&](const auto&){stop=true;});
    check(stop && interrupted.status==archive_writer::Status::Cancelled && std::filesystem::is_empty(root/L"cancel"),
        "cancellation during engine work removes partial archive");
    auto duplicate=archive_writer::Create({source.wstring(),source.wstring()},(root/L"cancel").wstring(),L"duplicate.7z",archive_writer::Format::SevenZip,1);
    check(duplicate.status!=archive_writer::Status::Ok && std::filesystem::is_empty(root/L"cancel"),
        "duplicate members fail without partial output");
    auto occupied=archive_writer::Create({source.wstring()},stage.wstring(),L"source.7z",archive_writer::Format::SevenZip,1);
    check(occupied.status==archive_writer::Status::InvalidInput && archive_reader::Read(seven.path).status==archive_reader::Status::Ok,
        "nonempty stage is rejected without replacing existing archive");
    auto extended=archive_writer::Create({L"\\\\?\\"+source.wstring()},(root/L"cancel").wstring(),L"extended.7z",archive_writer::Format::SevenZip,1);
    check(extended.status==archive_writer::Status::Ok && archive_reader::Read(extended.path).status==archive_reader::Status::Ok,
        "host extended local paths create readable archives");
    std::filesystem::remove_all(root);
    std::printf("Result: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
