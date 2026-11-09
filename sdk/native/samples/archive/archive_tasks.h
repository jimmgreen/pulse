#pragma once
#include <pulse/client.hpp>
#include <pulse/staging.h>
#include <memory>
#include <string>
#include <vector>
#include "archive_writer.h"

class ArchiveTasks {
public:
    ArchiveTasks(PulseTasksV1 api, PulseStagingV1 staging);
    ~ArchiveTasks();
    PulseStatus Verify(const std::wstring& path);
    PulseStatus Extract(const std::wstring& path, const std::string& destination);
    PulseStatus Compress(const std::vector<std::wstring>& paths, const std::string& destination,
        const std::wstring& name, archive_writer::Format format, unsigned level);
private:
    struct Job;
    PulseStatus Start(const std::wstring& path, const std::string& destination);
    pulse::sdk::TaskGroup tasks_;
    PulseStagingV1 staging_;
};
