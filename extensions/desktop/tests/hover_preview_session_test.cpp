#include "host/hover_preview_session.h"
#include <cstdio>
int main() {
    pulse::desktop::HoverPreviewSession session;
    int failed = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failed += !ok; };
    auto first = session.Update(L"C:\\fixture\\报告.pdf", -120, 40, 64, 90);
    check(first.find("hover_preview") != std::string::npos && first.find("\"rect_x\":-120") != std::string::npos,
          "absolute file candidate preserves negative-monitor screen anchor");
    check(session.Update(L"C:\\fixture\\报告.pdf", -119, 41, 64, 90).empty(), "same candidate does not restart backend dwell");
    auto second = session.Update(L"\\\\server\\share\\file.docx", 30, 40, 60, 80);
    check(second.find("hover_preview") != std::string::npos, "rapid candidate replacement emits new request");
    auto cancel = session.Cancel();
    std::string path;
    pulse::desktop::ipc::JsonFindString(cancel, "path", path);
    check(cancel.find("dismiss_hover_preview") != std::string::npos &&
          pulse::desktop::ipc::Utf8ToWide(path) == L"\\\\server\\share\\file.docx", "cancel identifies latest candidate only");
    check(session.Cancel().empty(), "repeated cancel does not flood pipe");
    check(session.Update(L"relative.pdf", 0, 0, 1, 1).empty(), "relative candidate ignored");
    session.Update(L"C:\\fixture\\a.pdf", 0, 0, 10, 10);
    check(session.Update(L"", 0, 0, 0, 0).find("dismiss_hover_preview") != std::string::npos, "empty hit cancels pending preview");
    check(!session.Update(L"C:\\fixture\\a.pdf", 0, 0, 10, 10).empty(), "reentry publishes candidate again");
    std::printf("HOVER_SESSION failed=%d\n", failed);
    return failed ? 1 : 0;
}
