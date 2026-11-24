#include "../ops/operation_presentation.h"
#include "../common/path_utils.h"
#include <cstdio>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    using pulse::path::FriendlyPathText;
    check(FriendlyPathText(L"删除 \\\\?\\C:\\文件 完成") == L"删除 C:\\文件 完成", "embedded extended drive path");
    check(FriendlyPathText(L"\\??\\C:\\文件") == L"C:\\文件", "NT drive path");
    check(FriendlyPathText(L"\\\\?\\UNC\\server\\share\\文件") == L"\\\\server\\share\\文件", "extended UNC path");
    check(FriendlyPathText(L"失败：\\??\\unc\\server\\share") == L"失败：\\\\server\\share", "embedded NT UNC path");
    check(FriendlyPathText(L"C:\\文件 → \\\\server\\share") == L"C:\\文件 → \\\\server\\share", "ordinary paths unchanged");
    check(FriendlyPathText(L"\\\\?\\Volume{abc}\\") == L"\\\\?\\Volume{abc}\\", "unknown device path preserved");
    check(FriendlyPathText(L"无法从“\\\\?\\D:\\带空格 源.zip”写入“\\\\?\\unc\\server\\share\\目录”") ==
          L"无法从“D:\\带空格 源.zip”写入“\\\\server\\share\\目录”", "multiple paths in the same prompt are presented normally");
    using pulse::path::StripExtendedPathPrefix;
    check(StripExtendedPathPrefix(L"\\\\?\\c:\\目录") == L"c:\\目录" &&
          StripExtendedPathPrefix(L"\\\\?\\unc\\server\\share\\目录") == L"\\\\server\\share\\目录" &&
          StripExtendedPathPrefix(L"\\\\?\\Volume{abc}\\") == L"\\\\?\\Volume{abc}\\",
          "Shell boundary shares drive/UNC conversion and preserves real device namespaces");
    pulse::ops::OpStatus status;
    status.type = pulse::ops::OpType::RecycleDelete;
    status.phase = pulse::ops::OpPhase::Completed;
    status.percent = -1.0f;
    status.summary = L"删除 \\\\?\\C:\\文件 完成";
    status.last_error = L"无法访问 \\??\\C:\\文件";
    status.source_label = status.destination_label = status.current_item = L"\\\\?\\C:\\文件";
    const auto shown = pulse::ops::PresentOperationStatus(status);
    check(shown.percent == 100.0f, "completed shell deletion has full progress");
    check(shown.summary == L"删除 C:\\文件 完成" && shown.last_error == L"无法访问 C:\\文件" &&
          shown.source_label == L"C:\\文件" && shown.destination_label == L"C:\\文件" &&
          shown.current_item == L"C:\\文件", "all operation display fields sanitized");
    check(status.percent == -1.0f && status.current_item == L"\\\\?\\C:\\文件", "original status unchanged");
    for (auto phase : {pulse::ops::OpPhase::Running, pulse::ops::OpPhase::Paused,
                       pulse::ops::OpPhase::Failed, pulse::ops::OpPhase::Cancelling}) {
        status.phase = phase;
        status.percent = 37.0f;
        check(pulse::ops::PresentOperationStatus(status).percent == 37.0f, "unfinished progress preserved");
    }
    return failures ? 1 : 0;
}
