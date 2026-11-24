#include "../shell_bridge/file_dialog_contract.h"
#include <cstdio>

using namespace pulse::shell_bridge;
namespace wire = pulse::ipc::picker;

int main() {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks;
        failures += !ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    };
    wire::RequestId id{};
    id[0] = 1;
    for (auto factory : {DialogFactory::Open, DialogFactory::Save}) {
        const auto policy = factory == DialogFactory::Open ? AccessPolicy::LocalOpenV1 : AccessPolicy::LocalSaveV1;
        PreparationContext context;
        context.resolved_initial_directory = u"C:\\fixture";
        context.native_save_defaults_supported = true;
        SetterCall call;
        call.setting = Setting::FileName;
        call.native_result = S_OK;
        for (auto name : {u"C:\\other\\file.txt", u"sub\\file.txt", u"\\\\server\\share\\file.txt", u"\\\\?\\C:\\other\\file.txt"}) {
            call.text = name;
            context.resolved_filename = name;
            const auto decision = PrepareFileDialogContract(factory, {&call, 1}, id, policy, context);
            check(decision.request && std::get<wire::Request>(decision.request->body).filename == name &&
                std::get<wire::Request>(decision.request->body).initial_directory == u"C:\\fixture",
                "snapshot preserves filename and folder independently");
        }
        for (auto name : {u"*.txt", u"\"file.txt\"", u"C:relative.txt", u"\\rooted.txt", u"sub\\..\\file.txt", u"C:\\file.txt:stream", u"C:\\folder\\"}) {
            call.text = name;
            context.resolved_filename = name;
            check(!PrepareFileDialogContract(factory, {&call, 1}, id, policy, context).request,
                "unsupported syntax retains native route");
        }
        call.text = u"C:\\other\\file.txt";
        context.resolved_filename.reset();
        check(!PrepareFileDialogContract(factory, {&call, 1}, id, policy, context).request,
            "missing filename getter does not authorize paths");
        context.resolved_filename = u"later.txt";
        const auto decision = PrepareFileDialogContract(factory, {&call, 1}, id, policy, context);
        check(decision.request && std::get<wire::Request>(decision.request->body).filename == u"later.txt",
            "final native getter wins over earlier filename setter");
    }
    PreparationContext context;
    context.resolved_initial_directory = u"C:\\fixture";
    context.native_save_defaults_supported = true;
    SetterCall call;
    call.setting = Setting::DefaultExtension;
    call.native_result = S_OK;
    for (auto extension : {u"tar.gz", u".yjk"}) {
        call.text = extension;
        const auto decision = PrepareFileDialogContract(DialogFactory::Save, {&call, 1}, id, AccessPolicy::LocalSaveV1, context);
        check(decision.request && std::get<wire::Request>(decision.request->body).default_extension == extension,
            "compound Save extension preserved verbatim");
    }
    for (auto extension : {u"*.tar.gz", u"..txt", u"tar.", u"*.tar.", u"*.*", u"txt;dat", u"a/b", u"a\tb"}) {
        call.text = extension;
        check(!PrepareFileDialogContract(DialogFactory::Save, {&call, 1}, id, AccessPolicy::LocalSaveV1, context).request,
            "unsupported Save extension retains native route");
    }
    std::printf("Checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
