#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace archive_writer {
enum class Format { SevenZip, Zip };
enum class Status { Ok, Cancelled, InvalidInput, IoError, LimitReached, Unsupported };
struct Progress { std::string path; uint64_t completed_bytes=0,total_bytes=0,completed_items=0,total_items=0; };
struct Result { Status status=Status::IoError; std::string error; std::wstring path; uint64_t bytes=0,items=0; };
// Worker only. Local absolute sources, empty private staging, single basename.
Result Create(const std::vector<std::wstring>& sources,const std::wstring& staging,const std::wstring& basename,Format format,unsigned level,const std::function<bool()>& cancelled={},const std::function<void(const Progress&)>& progress={});
}
