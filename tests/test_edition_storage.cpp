#include "draft_storage.h"
#include "backup_format.h"
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void write(const fs::path& path,const std::string& bytes){fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);out<<bytes;require(bool(out),"fixture write");}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};}
int main(){
    const auto root=fs::temp_directory_path()/("envhosts-editions-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    struct Cleanup{fs::path path;~Cleanup(){std::error_code ec;fs::remove_all(path,ec);}} cleanup{root};
    try{
        const auto old=root/"drafts/env.json";
        const std::string gdi=R"({"rows":[{"name":"PENDING","value":"gdi"}]})";
        const std::string qml=R"({"schemaVersion":2,"kind":"env","state":{"env":[]}})";
        const auto gp=envh::drafts::path(root.u8string(),"gdi","env"),qp=envh::drafts::path(root.u8string(),"qml","env");
        std::string error;
        write(old,gdi);require(envh::drafts::migrate(root.u8string(),"env",error),"migrate GDI");
        require(read(gp)==gdi&&!fs::exists(qp)&&!fs::exists(old),"GDI routed without QML overwrite");
        write(old,qml);require(envh::drafts::migrate(root.u8string(),"env",error),"migrate QML");
        require(read(qp)==qml&&read(gp)==gdi,"both drafts survive switching");
        fs::remove(qp);require(envh::drafts::migrate(root.u8string(),"env",error)&&!fs::exists(qp),"cleared draft not resurrected");
        write(old,gdi+" ");require(envh::drafts::migrate(root.u8string(),"env",error),"archive duplicate legacy");
        require(read(gp)==gdi,"never overwrite newer per-edition draft");
        int archives=0;for(const auto& entry:fs::directory_iterator(root/"drafts/legacy")){require(entry.file_size()>0,"archive bytes retained");++archives;}
        require(archives==3,"all originals archived");
        write(old,"bad json");require(!envh::drafts::migrate(root.u8string(),"env",error)&&read(old)=="bad json","unknown format preserved");
        envh::EnvSnapshot snap;std::set<std::string> scopes;
        const auto decode=[&](const std::string& value){return envh::decodeGdiBackup(*winshun::Json::parse(value),snap,scopes,error);};
        require(decode(R"({"schemaVersion":2,"scopes":["system"],"vars":[{"name":"A","value":"ok","type":"REG_SZ","scope":"system"}]})"),"QML string backup readable");
        require(scopes==std::set<std::string>{"system"}&&snap.vars.size()==1,"scope retained");
        require(decode(R"({"schemaVersion":2,"scopes":["system"],"vars":[]})")&&scopes.count("system"),"empty system snapshot not interpreted as user");
        require(!decode(R"({"schemaVersion":2,"vars":[{"name":"M","value":"A;B\nC","type":"REG_MULTI_SZ","scope":"user"}]})")&&error=="E_BACKUP_USE_QML","lossless multi-string refused by GDI");
        require(decode(R"({"vars":[{"name":"M","value":"A;B","type":"REG_MULTI_SZ","scope":"user"}]})"),"legacy native backup retained");
        require(!decode(R"({"schemaVersion":2,"vars":[{"name":"X","value":"raw","type":"REG_BINARY","scope":"user"}]})"),"unknown type refused");
        require(!decode(R"({"schemaVersion":77,"vars":[]})"),"unknown backup schema refused");
        require(!decode(R"({"vars":[{"name":"X","value":"a","type":"REG_SZ"},{"name":"x","value":"b","type":"REG_SZ"}]})"),"duplicate keys refused");
        require(!decode(R"({"schemaVersion":2,"scopes":["system"],"vars":[{"name":"A","value":"x","scope":"user","type":"REG_SZ"}]})"),"scope mismatch refused");
        std::cout<<"edition storage: migration, isolation, archive and backup compatibility passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
