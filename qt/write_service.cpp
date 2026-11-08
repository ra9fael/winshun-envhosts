#include "write_service.h"
#include "draft_storage.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringDecoder>
#include <cstring>

namespace editorwin {
namespace {
bool readFile(const QString& path,QByteArray& bytes,QString& error) {
    QFile f(path);if(!f.open(QIODevice::ReadOnly)){error="E_READ";return false;}
    bytes=f.readAll();if(f.error()!=QFileDevice::NoError){error="E_READ";return false;}return true;
}
bool durableFile(const QString& path,const QByteArray& bytes,QString& error) {
    if(!QDir().mkpath(QFileInfo(path).absolutePath())){error="E_BACKUP_DIR";return false;}
    QSaveFile f(path);f.setDirectWriteFallback(false);
    if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size()||!f.commit()){error="E_PERSIST";return false;}
    QByteArray actual;if(!readFile(path,actual,error)||actual!=bytes){error="E_PERSIST_READBACK";return false;}return true;
}
bool hostsEncoding(const QByteArray& bytes,QString& error) {
    if(bytes.startsWith("\xff\xfe")||bytes.startsWith("\xfe\xff")){error="E_HOSTS_UTF16";return false;}
    QStringDecoder d(QStringDecoder::Utf8);const QString text=d(bytes);
    if(d.hasError()||text.contains(QChar(0))){error="E_HOSTS_ENCODING";return false;}return true;
}
bool readKey(HKEY root,const std::wstring& sub,const QString& scope,TargetSnapshot& out,bool& writable) {
    HKEY key=nullptr;const LONG opened=RegOpenKeyExW(root,sub.c_str(),0,KEY_QUERY_VALUE,&key);
    if(opened!=ERROR_SUCCESS){out.error="E_ENV_READ";return false;}
    HKEY probe=nullptr;writable=RegOpenKeyExW(root,sub.c_str(),0,KEY_SET_VALUE,&probe)==ERROR_SUCCESS;if(probe)RegCloseKey(probe);
    DWORD count=0,maxName=0,maxData=0;
    if(RegQueryInfoKeyW(key,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&count,&maxName,&maxData,nullptr,nullptr)!=ERROR_SUCCESS){RegCloseKey(key);out.error="E_ENV_READ";return false;}
    for(DWORD i=0;i<count;++i){
        std::vector<wchar_t> name(size_t(maxName)+2);DWORD n=maxName+1,type=0,size=maxData;
        QByteArray bytes(int(maxData),0);
        LONG rc=RegEnumValueW(key,i,name.data(),&n,nullptr,&type,reinterpret_cast<BYTE*>(bytes.data()),&size);
        if(rc==ERROR_MORE_DATA){bytes.resize(qsizetype(size));n=maxName+1;rc=RegEnumValueW(key,i,name.data(),&n,nullptr,&type,reinterpret_cast<BYTE*>(bytes.data()),&size);}
        if(rc!=ERROR_SUCCESS){RegCloseKey(key);out.error="E_ENV_READ";return false;}
        bytes.resize(qsizetype(size));EnvValue v;v.scope=scope;v.name=QString::fromWCharArray(name.data(),int(n));v.nativeType=type;v.bytes=bytes;
        v.type=type==REG_SZ?"REG_SZ":type==REG_EXPAND_SZ?"REG_EXPAND_SZ":type==REG_MULTI_SZ?"REG_MULTI_SZ":"REG_"+QString::number(type);
        if(type==REG_SZ||type==REG_EXPAND_SZ||type==REG_MULTI_SZ){
            QString raw=fromUtf16Bytes(bytes);
            if(bytes.size()%2||raw.isEmpty()||!raw.endsWith(QChar(0))){v.formatError="E_ENV_ENCODING";v.value=raw;}
            else if(type==REG_MULTI_SZ){
                if(raw.size()<2||raw[raw.size()-2]!=QChar(0)){v.formatError="E_MULTI_FORMAT";v.value=raw;}
                else {raw.chop(2);const auto values=raw.isEmpty()?QStringList{}:raw.split(QChar(0));
                    for(const auto& s:values)if(s.isEmpty()||s.contains('\n')||s.contains('\r'))v.formatError="E_MULTI_FORMAT";
                    v.value=values.join('\n');}
            }else{raw.chop(1);v.value=raw;if(raw.contains(QChar(0)))v.formatError="E_ENV_ENCODING";}
        }else{v.value=QString::fromLatin1(bytes.toHex());v.formatError="E_ENV_TYPE";}
        out.env.append(v);
    }
    RegCloseKey(key);return true;
}
}

WriteResult WriteService::execute(StoreBackend& store,const WriteRequest& request) {
    // Shared with the launcher's protocol thread. No GUI work occurs under this lock.
    std::lock_guard<std::mutex> lock(envh::native::g_writeLock);
    WriteResult result;result.actual=store.read();
    if(!result.actual.error.isEmpty()){result.error=result.actual.error;return result;}
    if(request.fingerprints.isEmpty()||request.token.isEmpty()||request.revision.isEmpty()||(request.kind!="env"&&request.kind!="hosts")){result.error="E_REVIEW";return result;}
    for(const auto& scope:request.fingerprints.keys())if(request.kind=="env"?(scope!="user"&&scope!="system"):scope!="hosts"){result.error="E_REVIEW_SCOPE";return result;}
    QSet<QString> touched;
    for(const auto& c:request.changes){
        const auto key=envKey(c.scope,c.name);
        if(request.kind!="env"||!request.fingerprints.contains(c.scope)||touched.contains(key)||(!c.hasBefore&&!c.hasAfter)||
            !envh::isValidEnvName(c.name.toStdString())||(c.hasAfter&&(c.after.scope!=c.scope||c.after.name!=c.name||!c.after.formatError.isEmpty()))){result.error="E_REVIEW_CHANGE";return result;}
        if(c.hasAfter){auto encoded=c.after;encoded.encode();if(encoded.nativeType!=c.after.nativeType||encoded.bytes!=c.after.bytes){result.error="E_ENV_ENCODING";return result;}}
        touched.insert(key);
    }
    if(request.kind=="hosts"&&!hostsEncoding(request.hosts,result.error))return result;
    for(auto it=request.fingerprints.cbegin();it!=request.fingerprints.cend();++it){
        if(result.actual.fingerprint(it.key())!=it.value()){result.state="Conflict";result.error="E_CONFLICT";return result;}
        const bool writable=it.key()=="user"?result.actual.userWritable:it.key()=="system"?result.actual.systemWritable:result.actual.hostsWritable;
        if(!writable){result.error="E_DENIED";return result;}
    }
    if(request.kind=="env"&&request.changes.isEmpty()){result.state="Succeeded";return result;}
    if(request.kind=="hosts"&&request.hosts==result.actual.hosts){result.state="Succeeded";return result;}
    if(!store.backup(result.actual,request.fingerprints.keys(),result.backupId,result.error)){result.error="E_BACKUP: "+result.error;return result;}
    TargetSnapshot expected=result.actual;
    if(request.kind=="env"){
        for(int i=0;i<request.changes.size();++i){const auto& c=request.changes[i];
            if(!store.writeEnv(c,result.error)){result.failed.append(c.name);for(int j=i+1;j<request.changes.size();++j)result.notExecuted.append(request.changes[j].name);
                result.state="PartialFailure";result.actual=store.read();return result;}
            result.completed.append(c.name);
            const auto key=envKey(c.scope,c.name);
            expected.env.erase(std::remove_if(expected.env.begin(),expected.env.end(),[&](const EnvValue& v){return v.key()==key;}),expected.env.end());
            if(c.hasAfter)expected.env.append(c.after);
        }
    }else{
        if(!store.writeHosts(request.hosts,result.error)){result.state="PartialFailure";result.actual=store.read();return result;}expected.hosts=request.hosts;
    }
    result.actual=store.read();
    if(!result.actual.error.isEmpty()){result.state="ReadbackFailure";result.error=result.actual.error;return result;}
    for(const auto& scope:request.fingerprints.keys())if(result.actual.fingerprint(scope)!=expected.fingerprint(scope)){
        result.state="ReadbackFailure";result.error="E_READBACK";return result;}
    result.state=request.kind=="env"&&!store.broadcast()?"StoredButBroadcastFailed":"Succeeded";return result;
}

QString NativeBackend::identity() const {
    if(args.kind=="env")return "env:HKCU/"+QString::fromStdWString(args.userSubkey)+"|"+QString::fromStdWString(args.sysSubkey);
    return "hosts:"+QDir::cleanPath(QFileInfo(QString::fromStdWString(args.hostsPath)).absoluteFilePath()).toCaseFolded();
}
TargetSnapshot NativeBackend::read() {
    TargetSnapshot s;
    if(args.kind=="env"){
        if(!readKey(HKEY_CURRENT_USER,args.userSubkey,"user",s,s.userWritable))return s;
        if(args.showSystem){size_t skip=0;const HKEY root=envh::native::systemEnvRoot(args.sysSubkey,skip);readKey(root,args.sysSubkey.substr(skip),"system",s,s.systemWritable);}
    }else{
        if(!readFile(QString::fromStdWString(args.hostsPath),s.hosts,s.error)||!hostsEncoding(s.hosts,s.error))return s;
        envh::native::HostsStore store(args.hostsPath);std::string error;s.hostsWritable=store.isWritable(error);
    }
    return s;
}
bool NativeBackend::writeEnv(const Change& c,QString& error) {
    if(c.scope!="user"&&c.scope!="system"){error="E_SCOPE";return false;}
    HKEY key=nullptr;envh::native::EnvStore store(args.userSubkey,args.sysSubkey);
    if(!store.openForWrite(c.scope.toStdString(),key)){error="E_DENIED";return false;}
    LONG rc;
    if(c.hasAfter){const auto& v=c.after;rc=RegSetValueExW(key,reinterpret_cast<LPCWSTR>(v.name.utf16()),0,v.nativeType,
        reinterpret_cast<const BYTE*>(v.bytes.constData()),DWORD(v.bytes.size()));}
    else rc=RegDeleteValueW(key,reinterpret_cast<LPCWSTR>(c.name.utf16()));
    RegCloseKey(key);
    if(rc!=ERROR_SUCCESS&&!(rc==ERROR_FILE_NOT_FOUND&&!c.hasAfter)){error="E_ENV_WRITE:"+QString::number(rc);return false;}return true;
}
bool NativeBackend::writeHosts(const QByteArray& bytes,QString& error) {
    if(!hostsEncoding(bytes,error))return false;
    const HANDLE file=CreateFileW(args.hostsPath.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){error="E_HOSTS_WRITE:"+QString::number(GetLastError());return false;}
    DWORD written=0;
    bool ok=SetEndOfFile(file)&&WriteFile(file,bytes.constData(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);
    if(!CloseHandle(file))ok=false;if(!ok)error="E_HOSTS_WRITE";return ok;
}
bool NativeBackend::broadcast() {
    return SendMessageTimeoutW(HWND_BROADCAST,WM_SETTINGCHANGE,0,reinterpret_cast<LPARAM>(L"Environment"),SMTO_ABORTIFHUNG,3000,nullptr)!=0;
}
QString NativeBackend::backupDir() const {return QString::fromStdString(args.dataDir)+"/backups/"+QString::fromStdString(args.kind);}
QString NativeBackend::backupPath(const QString& id) const {
    if(!envh::native::BackupStore::validId(id.toStdString()))return {};
    return backupDir()+"/"+id+(args.kind=="env"?".json":".hosts");
}
bool NativeBackend::backup(const TargetSnapshot& s,const QStringList& scopes,QString& id,QString& error) {
    id=QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmss.zzz'Z'")+"-"+freshId().left(8);
    QByteArray bytes=s.hosts;
    if(args.kind=="env"){
        QJsonArray vars;for(const auto& v:s.env)if(scopes.contains(v.scope))vars.append(v.json());
        bytes=QJsonDocument(QJsonObject{{"schemaVersion",2},{"scopes",QJsonArray::fromStringList(scopes)},{"vars",vars}}).toJson();
    }
    if(!durableFile(backupPath(id),bytes,error)){id.clear();return false;}
    // Retention is deferred until recovery points can be pinned across sessions.
    // Deleting an older backup here can remove the point needed after a failed write.
    return true;
}
QVector<BackupRecord> NativeBackend::backups(QString& error) const {
    QVector<BackupRecord> out;QDir d(backupDir());if(!d.exists())return out;
    if(!QFileInfo(backupDir()).isReadable()){error="E_BACKUP_READ";return out;}
    for(const auto& f:d.entryInfoList({args.kind=="env"?"*.json":"*.hosts"},QDir::Files,QDir::Name|QDir::Reversed)){
        const QString id=f.fileName().chopped(args.kind=="env"?5:6);QString scope="hosts";TargetSnapshot s;QString ignored;
        if(args.kind=="env"&&!readBackup(id,s,scope,ignored))scope="invalid";
        out.append({id,scope,f.size()});
    }return out;
}
bool NativeBackend::readBackup(const QString& id,TargetSnapshot& s,QString& scope,QString& error) const {
    const auto path=backupPath(id);if(path.isEmpty()){error="E_BACKUP_ID";return false;}
    QByteArray bytes;if(!readFile(path,bytes,error))return false;
    if(args.kind=="hosts"){if(!hostsEncoding(bytes,error))return false;s.hosts=bytes;scope="hosts";return true;}
    QJsonParseError parse;const auto doc=QJsonDocument::fromJson(bytes,&parse);
    if(parse.error!=QJsonParseError::NoError||!doc.isObject()||!doc.object()["vars"].isArray()){error="E_BACKUP_FORMAT";return false;}
    const auto root=doc.object();const int schema=root["schemaVersion"].toInt(1);if(schema!=1&&schema!=2){error="E_BACKUP_SCHEMA";return false;}
    s.env.clear();QSet<QString> keys,scopes;
    for(const auto& j:root["vars"].toArray()){
        if(!j.isObject()){error="E_BACKUP_FORMAT";return false;}const auto o=j.toObject();auto v=EnvValue::fromJson(o);
        if(v.scope.isEmpty())v.scope="user";
        if(!o["name"].isString()||!o["value"].isString()||!o["type"].isString()||(v.scope!="user"&&v.scope!="system")||!envh::isValidEnvName(v.name.toStdString())||keys.contains(v.key())){error="E_BACKUP_FORMAT";return false;}
        if(!o.contains("bytes")){if(v.type=="REG_MULTI_SZ"){error="E_BACKUP_MULTI_LEGACY";return false;}v.encode();}
        else if(QByteArray::fromBase64Encoding(o["bytes"].toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors).decodingStatus!=QByteArray::Base64DecodingStatus::Ok){error="E_BACKUP_ENCODING";return false;}
        if(v.formatError.isEmpty()){
            if(v.type!="REG_SZ"&&v.type!="REG_EXPAND_SZ"&&v.type!="REG_MULTI_SZ"){error="E_BACKUP_TYPE";return false;}
            auto encoded=v;encoded.encode();if(encoded.nativeType!=v.nativeType||encoded.bytes!=v.bytes||v.value.contains(QChar(0))){error="E_BACKUP_ENCODING";return false;}
        }
        keys.insert(v.key());scopes.insert(v.scope);s.env.append(v);
    }
    for(const auto& value:root["scopes"].toArray()){const auto sc=value.toString();if(sc!="user"&&sc!="system"){error="E_BACKUP_SCOPE";return false;}scopes.insert(sc);}
    scope=scopes.size()>1?"all":scopes.isEmpty()?"user":*scopes.begin();return true;
}
bool NativeBackend::removeBackup(const QString& id,QString& error) const {
    const auto path=backupPath(id);if(path.isEmpty()||!QFile::remove(path)){error="E_BACKUP_DELETE";return false;}return true;
}
QString NativeBackend::exportBackup(const QString& id,QString& error) const {
    const auto path=backupPath(id);if(path.isEmpty()){error="E_BACKUP_ID";return {};}
    QByteArray bytes;if(!readFile(path,bytes,error))return {};
    const auto dest=QString::fromStdString(args.dataDir)+"/exports/"+QFileInfo(path).fileName();return durableFile(dest,bytes,error)?dest:QString{};
}
QString NativeBackend::draftPath() const {return QString::fromStdWString(envh::drafts::path(args.dataDir,"qml",args.kind).wstring());}
bool NativeBackend::saveRecoveryDraft(const QJsonObject& object,QString& error) const {
    return durableFile(QString::fromStdString(args.dataDir)+"/drafts/qml/recovery/"+QString::fromStdString(args.kind)+"-"+freshId()+".json",QJsonDocument(object).toJson(),error);
}
bool NativeBackend::saveDraft(const QJsonObject& object,QString& error) const {
    if(QFile::exists(draftPath())){
        QByteArray bytes;if(!readFile(draftPath(),bytes,error))return false;
        const auto old=QJsonDocument::fromJson(bytes).object();
        if(old["schemaVersion"].toInt()!=2||old["targetIdentity"]!=object["targetIdentity"]){
            const auto recovery=QString::fromStdString(args.dataDir)+"/drafts/qml/recovery/"+QString::fromStdString(args.kind)+"-"+freshId()+".json";
            if(!durableFile(recovery,bytes,error))return false;
        }
    }
    return durableFile(draftPath(),QJsonDocument(object).toJson(),error);
}
QJsonObject NativeBackend::readDraft(QString& error) const {
    std::string migrationError;
    if(!envh::drafts::migrate(args.dataDir,args.kind,migrationError))error=QString::fromStdString(migrationError);
    if(!QFile::exists(draftPath()))return {};QByteArray bytes;if(!readFile(draftPath(),bytes,error))return {};
    QJsonParseError p;const auto doc=QJsonDocument::fromJson(bytes,&p);if(p.error!=QJsonParseError::NoError||!doc.isObject()){error="E_DRAFT_FORMAT";return {};}return doc.object();
}
bool NativeBackend::removeDraft(QString& error) const {if(!QFile::exists(draftPath())||QFile::remove(draftPath()))return true;error="E_DRAFT_DELETE";return false;}

} // namespace editorwin
