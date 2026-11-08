#pragma once
#include "logic.h"
#include "winshun.h"
#include <set>

namespace envh {
// The native editor stores REG_MULTI_SZ as a semicolon string. It must not
// reinterpret QML's lossless multi-string snapshots or unsupported raw types.
inline bool decodeGdiBackup(const winshun::Json& root,EnvSnapshot& snapshot,
                            std::set<std::string>& scopes,std::string& error) {
    snapshot={};scopes.clear();
    const auto fail=[&](const char* code){error=code;snapshot={};scopes.clear();return false;};
    const auto schema=root["schemaVersion"].toInt(1);
    if(!root.isObject()||!root["vars"].isArray()||(schema!=1&&schema!=2))return fail("E_BACKUP_FORMAT");
    std::set<std::string> keys;
    for(const auto& item:root["vars"].toArray()) {
        EnvVar v;v.name=item["name"].toString();v.type=item["type"].toString();v.value=item["value"].toString();
        v.scope=item.has("scope")?item["scope"].toString():"user";
        if(!item.isObject()||!item["name"].isString()||!item["value"].isString()||!isValidEnvName(v.name)
           ||v.value.find('\0')!=std::string::npos||(v.scope!="user"&&v.scope!="system")
           ||!keys.insert(v.scope+"/"+toLower(v.name)).second)return fail("E_BACKUP_FORMAT");
        if(v.type!="REG_SZ"&&v.type!="REG_EXPAND_SZ"&&v.type!="REG_MULTI_SZ")return fail("E_BACKUP_USE_QML");
        if(schema==2&&(v.type=="REG_MULTI_SZ"||!item["formatError"].toString().empty()))return fail("E_BACKUP_USE_QML");
        scopes.insert(v.scope);snapshot.vars.push_back(v);
    }
    if(root.has("scopes")) {
        if(!root["scopes"].isArray())return fail("E_BACKUP_SCOPE");
        std::set<std::string> declared;
        for(const auto& item:root["scopes"].toArray()) {
            const auto scope=item.toString();if(scope!="user"&&scope!="system")return fail("E_BACKUP_SCOPE");
            declared.insert(scope);
        }
        if(declared.empty())return fail("E_BACKUP_SCOPE");
        for(const auto& scope:scopes)if(!declared.count(scope))return fail("E_BACKUP_SCOPE");
        scopes=declared;
    }else if(scopes.empty())scopes.insert("user"); // legacy empty snapshots
    error.clear();return true;
}
} // namespace envh
