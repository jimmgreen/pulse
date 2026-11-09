#include "../app/extensions/resource_contract.h"
#include <cstdio>
namespace {int failed=0;void Check(bool ok,const char* text){std::printf("[%s] %s\n",ok?"PASS":"FAIL",text);if(!ok)++failed;}}
int main() {
    using namespace pulse;using namespace pulse::extensions;
    const auto parse=[](const char* bytes){json::Value value;return json::Parse(bytes,value)?ParseResourceRequest(value):std::nullopt;};
    for(const auto* request:{R"({"op":"context.get"})",R"({"op":"files.list","limit":256})",R"({"op":"files.stat","entry":"opaque"})",
        R"({"op":"operations.get","taskId":"18446744073709551615"})",R"({"op":"operations.cancel","taskId":"1"})",
        R"({"op":"settings.get"})",R"({"op":"settings.set","key":"display.name","value":"name","expectedVersion":0})",
        R"({"op":"settings.set","key":"display.name","value":null,"expectedVersion":9007199254740991})",R"({"op":"audit.list","limit":128})"})
        Check(parse(request).has_value(),"strict supported resource request accepted");
    for(const auto* request:{R"({"op":"context.get","caller":"other"})",R"({"op":"files.list","path":"C:\\"})",
        R"({"op":"files.list","limit":257})",R"({"op":"files.list","limit":1.5})",R"({"op":"files.list","cursor":42})",
        R"({"op":"files.stat","entry":""})",R"({"op":"operations.get","taskId":1})",R"({"op":"operations.get","taskId":"01"})",
        R"({"op":"operations.get","taskId":"0"})",R"({"op":"operations.get","taskId":"18446744073709551616"})",
        R"({"op":"settings.set","key":"../outside","value":"x","expectedVersion":0})",
        R"({"op":"settings.set","key":"x","value":{},"expectedVersion":0})",R"({"op":"settings.set","key":"x","value":"v"})",
        R"({"op":"settings.set","key":"x","value":"v","expectedVersion":9007199254740992})",R"({"op":"audit.list","limit":129})",
        R"({"op":"unknown"})",R"([])"})Check(!parse(request),"unknown fields identity paths invalid sizes and versions rejected");
    auto data=json::Value::Object();data.set("count",1);
    Check(ResourceSuccess(data).get_bool("ok")&&ResourceSuccess(data).find("data"),"success envelope contains typed data");
    Check(!ResourceFailure("permission_denied").get_bool("ok")&&ResourceFailure("permission_denied").find("error")->get_string("code")=="permission_denied","failure envelope retains stable code");
    return failed?1:0;
}
