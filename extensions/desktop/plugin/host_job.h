#pragma once
#include <windows.h>

namespace pulse::desktop::plugin {
// The host belongs to Pulse; applications opened by the user do not.
inline bool ConfigureHostJob(HANDLE job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    return SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) != FALSE;
}
}
