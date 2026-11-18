#include "resource_contract.h"
#include <algorithm>
#include <charconv>
#include <initializer_list>

namespace pulse::extensions {
namespace {
bool Only(const json::Value& input, std::initializer_list<std::string_view> fields) {
    return input.is_object() && std::all_of(input.keys().begin(), input.keys().end(), [&](const auto& key) {
        return std::find(fields.begin(), fields.end(), key) != fields.end();
    });
}
bool Text(const json::Value& input, const char* key, std::string& value, size_t limit, bool required) {
    const auto* field = input.find(key);
    if (!field) return !required;
    if (!field->is_string() || field->as_string().size() > limit || (required && field->as_string().empty())) return false;
    value = field->as_string();
    return true;
}
}
std::optional<ResourceRequest> ParseResourceRequest(const json::Value& input) {
    ResourceRequest request;
    if (!Text(input,"op",request.name,64,true)) return std::nullopt;
    if (request.name == "context.get") {
        if (!Only(input,{"op"})) return std::nullopt;
    } else if (request.name == "files.list") {
        request.operation=ResourceOperation::List;
        if (!Only(input,{"op","cursor","limit"}) || !Text(input,"cursor",request.cursor,64,false)) return std::nullopt;
    } else if (request.name == "files.stat") {
        request.operation=ResourceOperation::Stat;
        if (!Only(input,{"op","entry"}) || !Text(input,"entry",request.entry,64,true)) return std::nullopt;
    } else if (request.name == "operations.get" || request.name == "operations.cancel") {
        request.operation=request.name=="operations.get" ? ResourceOperation::TaskGet : ResourceOperation::TaskCancel;
        std::string id;
        if (!Only(input,{"op","taskId"}) || !Text(input,"taskId",id,20,true)) return std::nullopt;
        auto parsed=std::from_chars(id.data(),id.data()+id.size(),request.task);
        if (parsed.ec!=std::errc{} || parsed.ptr!=id.data()+id.size() || !request.task || std::to_string(request.task)!=id) return std::nullopt;
    } else if (request.name == "settings.get") {
        request.operation=ResourceOperation::SettingsGet;
        if (!Only(input,{"op"})) return std::nullopt;
    } else if (request.name == "settings.set") {
        request.operation=ResourceOperation::SettingsSet;
        if (!Only(input,{"op","key","value","expectedVersion"}) || !Text(input,"key",request.key,64,true)) return std::nullopt;
        if (!std::all_of(request.key.begin(),request.key.end(),[](unsigned char c) {
            return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';
        })) return std::nullopt;
        const auto* value=input.find("value"); const auto* version=input.find("expectedVersion");
        int64_t number=0;
        if (!value || (!value->is_null()&&!value->is_string()) || value->as_string().size()>1024 ||
            !version || !version->get_int(number,0,(int64_t{1}<<53)-1)) return std::nullopt;
        if (value->is_string()) request.value=value->as_string();
        request.version=static_cast<uint64_t>(number);
    } else if (request.name == "audit.list") {
        request.operation=ResourceOperation::Audit;
        if (!Only(input,{"op","limit"})) return std::nullopt;
    } else if (request.name == "files.prepareRename") {
        request.operation = ResourceOperation::RenamePrepare;
        const auto* items = input.find("items");
        if (!Only(input, {"op", "items"}) || !items || !items->is_array() ||
            items->size() == 0 || items->size() > 64) return std::nullopt;
        for (const auto& item : items->items()) {
            std::string entry, name;
            if (!Only(item, {"entry", "newLeafName"}) || !Text(item, "entry", entry, 64, true) ||
                !Text(item, "newLeafName", name, 1024, true) ||
                entry.find('\0') != std::string::npos || name.find('\0') != std::string::npos)
                return std::nullopt;
        }
        request.rename_items = *items;
    } else if (request.name == "files.commitRename") {
        request.operation = ResourceOperation::RenameCommit;
        if (!Only(input, {"op", "planId"}) || !Text(input, "planId", request.plan_id, 64, true) ||
            request.plan_id.find('\0') != std::string::npos) return std::nullopt;
    } else return std::nullopt;
    if (const auto* value=input.find("limit")) {
        int64_t number=0;
        if (!value->get_int(number,1,request.operation==ResourceOperation::Audit?128:256)) return std::nullopt;
        request.limit=static_cast<size_t>(number);
    }
    return request;
}
json::Value ResourceSuccess(json::Value data) {
    auto result=json::Value::Object(); result.set("ok",true); result.set("data",std::move(data)); return result;
}
json::Value ResourceFailure(std::string_view code) {
    auto result=json::Value::Object(); auto error=json::Value::Object(); error.set("code",code);
    result.set("ok",false); result.set("error",std::move(error)); return result;
}
}
