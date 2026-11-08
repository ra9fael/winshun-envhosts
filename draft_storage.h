#pragma once
#include "winshun.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace envh::drafts {
inline std::mutex migrationMutex;

inline std::filesystem::path path(const std::string& root,const std::string& backend,const std::string& kind) {
    return std::filesystem::u8path(root)/"drafts"/backend/(kind+".json");
}

// Old releases shared drafts/<kind>.json. Route by format, regardless of the
// backend opening it first. Archive the original bytes before removing that
// ambiguous path; never overwrite a newer backend-specific draft.
inline bool migrate(const std::string& root,const std::string& kind,std::string& error) {
    std::lock_guard<std::mutex> guard(migrationMutex);
    const auto legacy=std::filesystem::u8path(root)/"drafts"/(kind+".json");
    std::error_code ec;
    if(!std::filesystem::exists(legacy,ec)) {
        if(ec)error="E_DRAFT_READ: "+ec.message();
        return !ec;
    }
    std::ifstream input(legacy,std::ios::binary);
    if(!input){error="E_DRAFT_READ";return false;}
    const std::string bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    if(input.bad()){error="E_DRAFT_READ";return false;}
    input.close();
    const auto object=winshun::Json::parse(bytes);
    std::string backend;
    if(object&&object->isObject()) {
        if((*object)["schemaVersion"].toInt()==2&&(*object)["kind"].toString()==kind&&(*object)["state"].isObject())backend="qml";
        else if(!object->has("schemaVersion")&&(*object)["rows"].isArray())backend="gdi";
    }
    if(backend.empty()){error="E_DRAFT_LEGACY_FORMAT";return false;}
    const auto target=path(root,backend,kind);
    const auto archiveDir=legacy.parent_path()/"legacy";
    std::filesystem::create_directories(target.parent_path(),ec);
    if(!ec)std::filesystem::create_directories(archiveDir,ec);
    if(ec){error="E_DRAFT_MIGRATE: "+ec.message();return false;}
    const auto stamp=std::chrono::system_clock::now().time_since_epoch().count();
    auto archive=archiveDir/(kind+"-"+backend+"-"+std::to_string(stamp)+".json");
    for(int i=0;std::filesystem::exists(archive)&&i<100;++i)
        archive=archiveDir/(kind+"-"+backend+"-"+std::to_string(stamp)+"-"+std::to_string(i)+".json");
    std::filesystem::copy_file(legacy,archive,std::filesystem::copy_options::none,ec);
    if(ec){error="E_DRAFT_ARCHIVE: "+ec.message();return false;}
    const bool exists=std::filesystem::exists(target,ec);
    if(!ec&&!exists)std::filesystem::copy_file(legacy,target,std::filesystem::copy_options::none,ec);
    if(ec){error="E_DRAFT_MIGRATE: "+ec.message();return false;}
    std::filesystem::remove(legacy,ec);
    if(ec){error="E_DRAFT_MIGRATE: "+ec.message();return false;}
    return true;
}
} // namespace envh::drafts
