#include "editor_session.h"
#include <QHostAddress>
#include <QRegularExpression>

namespace editorwin {
namespace {
QString qs(const std::string& s) { return QString::fromUtf8(s.data(),qsizetype(s.size())); }
QStringList words(const QString& text) { return text.split(QRegularExpression("\\s+"),Qt::SkipEmptyParts); }
bool supported(const EnvValue& v) { return v.formatError.isEmpty() && (v.type=="REG_SZ" || v.type=="REG_EXPAND_SZ" || v.type=="REG_MULTI_SZ"); }
QJsonArray envJson(const QVector<EnvValue>& values) { QJsonArray a; for(const auto& v:values)a.append(v.json());return a; }
QString partsValue(const PartsState& p) { QStringList values;for(const auto& x:p.items)if(!x.deleted)values.append(x.value);return values.join(p.delimiter); }
}

void EditorSession::load(const TargetSnapshot& snapshot,bool preserveIds) {
    QHash<QString,EnvValue> old;
    if(preserveIds)for(const auto& v:work.env)
        if(!old.contains(v.key())||!v.deleted)old.insert(v.key(),v);
    const auto oldParts=preserveIds&&kind=="env"?work.parts:QHash<QString,PartsState>{};
    const auto oldSelections=partSelections;
    base=snapshot;work={};buffer={};baselineUnknown=false;undoStates.clear();redoStates.clear();partSelections.clear();error.clear();
    work.env=snapshot.env;
    for(auto& v:work.env){
        const auto previous=old.constFind(v.key());
        v.id=previous==old.cend()?freshId():previous->id;v.deleted=false;v.isNew=false;
        const auto parts=oldParts.constFind(v.id);
        if(parts==oldParts.cend()||!supported(v)||v.type=="REG_MULTI_SZ")continue;
        PartsState clean;clean.delimiter=parts->delimiter;QStringList before;
        if(previous->type==v.type&&partsValue(*parts)==v.value){
            for(auto p:parts->items){before.append(p.id);if(!p.deleted){p.isNew=false;clean.items.append(p);}}
            work.parts.insert(v.id,clean);
            auto selection=oldSelections.value(v.id);selection.restore(before,partIds(v.id));partSelections.insert(v.id,selection);
        }else{
            // A reload may contain external edits: retain the delimiter, but never
            // restore stale values or attach old selections to different parts.
            for(const auto& value:envh::edit::splitDelimited(v.value.toStdString(),clean.delimiter.toStdString(),true))
                clean.items.append({freshId(),qs(value),false,false});
            work.parts.insert(v.id,clean);
        }
    }
    std::stable_sort(work.env.begin(),work.env.end(),[](const auto& a,const auto& b){return a.scope==b.scope?a.key()<b.key():a.scope=="user";});
    buildHosts(snapshot.hosts);rows={};const auto view=visible();if(!view.isEmpty())rows.select(view[0],view);
    ++revision;
}
void EditorSession::buildHosts(const QByteArray& bytes,bool keepIds) {
    const auto ids=work.hostIds;
    const auto parsed=envh::parseHosts(std::string(bytes.constData(),size_t(bytes.size())));
    int crlf=0;for(const auto& l:parsed.lines)if(l.crlf)++crlf;
    work.hosts=envh::edit::HostsModel::fromParsed(parsed,crlf*2>=int(parsed.lines.size()));
    // The permissive parser retains malformed lines as entries. They must stay opaque
    // in the structured editor, otherwise a batch toggle could silently rewrite them.
    for(auto& h:work.hosts.rows)if(!h.isSection){
        bool valid=!QHostAddress(qs(h.ip)).isNull()&&!h.hostnames.empty();
        for(const auto& name:h.hostnames)valid=valid&&envh::hostValid(name);
        if(!valid&&h.origin>=0){h.isSection=true;h.sectionText=parsed.lines[size_t(h.origin)].text;}
    }
    work.hosts.disk=envh::parseHosts(base.hosts.toStdString());
    work.hostIds.clear();
    for(size_t i=0;i<work.hosts.rows.size();++i)work.hostIds.append(keepIds&&int(i)<ids.size()?ids[int(i)]:freshId());
}
TargetSnapshot EditorSession::effective() const {
    TargetSnapshot out=base;out.env.clear();
    for(const auto& v:work.env)if(!v.deleted)out.env.append(v);
    out.hosts=QByteArray::fromStdString(work.hosts.serializeWork());return out;
}
bool EditorSession::dirty() const {
    if(buffer.pending())return true;
    if(kind=="hosts")return QByteArray::fromStdString(work.hosts.serializeWork())!=base.hosts;
    return !changes().isEmpty();
}
QStringList EditorSession::visible(const QString& filter,const QString& scope,bool showSystem) const {
    QStringList out;
    if(kind=="env")for(const auto& v:work.env){
        if((!showSystem&&v.scope=="system")||(scope!="all"&&v.scope!=scope))continue;
        if(filter.isEmpty()||v.name.contains(filter,Qt::CaseInsensitive)||v.value.contains(filter,Qt::CaseInsensitive))out.append(v.id);
    }else for(size_t i=0;i<work.hosts.rows.size();++i){
        const auto& h=work.hosts.rows[i];if(h.isSection)continue;
        QString text=qs(h.ip)+" "+qs(h.comment);
        for(const auto& n:h.hostnames)text+=" "+qs(n);
        if(filter.isEmpty()||text.contains(filter,Qt::CaseInsensitive))out.append(work.hostIds[int(i)]);
    }
    return out;
}
EnvValue* EditorSession::env(const QString& id) { for(auto& v:work.env)if(v.id==id)return &v;return nullptr; }
const EnvValue* EditorSession::env(const QString& id) const { for(const auto& v:work.env)if(v.id==id)return &v;return nullptr; }
int EditorSession::hostIndex(const QString& id) const { return int(work.hostIds.indexOf(id)); }
bool EditorSession::writable(const QString& id) const {
    if(writing)return false;
    if(kind=="env"){const auto* v=env(id);return v&&writable(*v);}
    const int i=hostIndex(id);return base.hostsWritable&&i>=0&&!work.hosts.rows[size_t(i)].isSection;
}
bool EditorSession::writable(const EnvValue& v) const {return !writing&&supported(v)&&(v.scope=="user"?base.userWritable:base.systemWritable);}
PartsState* EditorSession::ensureParts(const QString& row) {
    const auto* v=env(row);if(!v||v->type=="REG_MULTI_SZ"||!supported(*v))return nullptr;
    if(!work.parts.contains(row)){
        PartsState p;
        for(const auto& value:envh::edit::splitDelimited(v->value.toStdString(),";",true))p.items.append({freshId(),qs(value),false,false});
        work.parts.insert(row,p);
    }
    return &work.parts[row];
}
QStringList EditorSession::partIds(const QString& row) const {
    QStringList ids;for(const auto& p:work.parts.value(row).items)ids.append(p.id);return ids;
}
QString EditorSession::rawText() const {
    QByteArray bytes=QByteArray::fromStdString(work.hosts.serializeWork());if(bytes.startsWith("\xef\xbb\xbf"))bytes.remove(0,3);
    QString text=QString::fromUtf8(bytes);text.replace("\r\n","\n");return text;
}
QString EditorSession::field(const QString& row,const QString& name,const QString& part) const {
    if(name=="raw")return rawText();
    if(!part.isEmpty()){for(const auto& p:work.parts.value(row).items)if(p.id==part)return p.value;return {};}
    if(kind=="env"){
        const auto* v=env(row);if(!v)return {};
        return name=="name"?v->name:name=="type"?v->type:v->value;
    }
    const int i=hostIndex(row);if(i<0)return {};const auto& h=work.hosts.rows[size_t(i)];
    if(name=="ip")return qs(h.ip);if(name=="comment")return qs(h.comment);
    QStringList names;for(const auto& n:h.hostnames)names.append(qs(n));return names.join(" ");
}
QString EditorSession::beginEdit(const QString& row,const QString& name,const QString& part) {
    if(buffer.row==row&&buffer.field==name&&buffer.part==part&&!buffer.token.isEmpty())return buffer.token;
    if(!finishEdit())return {};
    if(writing||(name=="raw"?!base.hostsWritable:!writable(row)))return {};
    if(const auto* v=env(row);v&&v->deleted)return {};
    buffer={freshId(),row,part,name,field(row,name,part),field(row,name,part),false};return buffer.token;
}
void EditorSession::updateEdit(const QString& token,const QString& text,bool composing) {
    if(token==buffer.token&&!token.isEmpty()&&!writing){buffer.text=text;buffer.composing=composing;}
}
bool EditorSession::finishEdit() {
    if(buffer.composing)return false;
    if(!buffer.pending()){buffer={};return true;}
    const auto b=buffer;
    if(!editField(b.row,b.field,b.text,b.part))return false;
    buffer={};return true;
}
void EditorSession::cancelEdit() {buffer={};error.clear();}
QByteArray EditorSession::stateBytes() const {
    QJsonObject p;
    for(auto it=work.parts.cbegin();it!=work.parts.cend();++it){
        QJsonArray a;for(const auto& x:it->items)a.append(QJsonObject{{"id",x.id},{"value",x.value},{"new",x.isNew},{"deleted",x.deleted}});
        p.insert(it.key(),QJsonObject{{"delimiter",it->delimiter},{"items",a}});
    }
    const auto h=QJsonDocument::fromJson(QByteArray::fromStdString(work.hosts.toDraft().dump())).object();
    return QJsonDocument(QJsonObject{{"env",envJson(work.env)},{"parts",p},{"hosts",h},
        {"hostIds",QJsonArray::fromStringList(work.hostIds)}}).toJson(QJsonDocument::Compact);
}
bool EditorSession::mutate(const std::function<void()>& fn) {
    if(writing)return false;
    auto before=work;const auto bytes=stateBytes();fn();
    if(bytes==stateBytes())return true;
    undoStates.append(std::move(before));if(undoStates.size()>200)undoStates.removeFirst();redoStates.clear();++revision;error.clear();return true;
}
void EditorSession::syncParts(const QString& row) {if(auto* v=env(row)){v->value=partsValue(work.parts[row]);v->encode();}}
bool EditorSession::applyRaw(const QString& text) {
    if(text.contains(QChar(0))){error="E_NUL";return false;}
    const auto old=work.hosts.toParsed();auto next=envh::parseHosts(text.toUtf8().toStdString());next.bom=old.bom;
    QHash<QString,QStringList> oldIds;
    for(int i=0;i<work.hostIds.size();++i){const auto& row=work.hosts.rows[size_t(i)];
        const auto key=qs(row.isSection?row.sectionText:row.ip)+"|"+field(work.hostIds[i],"names")+"|"+qs(row.comment)+(row.commented?"#":"");
        oldIds[key].append(work.hostIds[i]);}
    QSet<int> used;
    for(size_t i=0;i<next.lines.size();++i){
        int match=-1;
        if(i<old.lines.size()&&next.lines[i].text==old.lines[i].text&&!used.contains(int(i)))match=int(i);
        else for(size_t j=0;j<old.lines.size();++j)if(!used.contains(int(j))&&old.lines[j].text==next.lines[i].text){match=int(j);break;}
        if(match>=0){used.insert(match);next.lines[i].crlf=old.lines[size_t(match)].crlf;}
        else next.lines[i].crlf=i<old.lines.size()?old.lines[i].crlf:work.hosts.dominantCrlf();
    }
    buildHosts(QByteArray::fromStdString(envh::serializeHosts(next)));
    for(int i=0;i<work.hostIds.size();++i){const auto& row=work.hosts.rows[size_t(i)];
        const auto key=qs(row.isSection?row.sectionText:row.ip)+"|"+field(work.hostIds[i],"names")+"|"+qs(row.comment)+(row.commented?"#":"");
        if(!oldIds[key].isEmpty())work.hostIds[i]=oldIds[key].takeFirst();}
    return true;
}
bool EditorSession::editField(const QString& row,const QString& name,const QString& value,const QString& part) {
    error.clear();
    if(writing||(name=="raw"?!base.hostsWritable:!writable(row))){error="E_DENIED";return false;}
    if(value.contains(QChar(0))){error="E_NUL";return false;}
    if(name=="raw")return mutate([&]{applyRaw(value);});
    if(kind=="env"){
        auto* v=env(row);if(!v||v->deleted)return false;
        if(!part.isEmpty()){
            auto* p=ensureParts(row);if(!p)return false;
            const auto it=std::find_if(p->items.begin(),p->items.end(),[&](const Part& x){return x.id==part&&!x.deleted;});
            if(it==p->items.end())return false;
            return mutate([&]{for(auto& x:work.parts[row].items)if(x.id==part)x.value=value;syncParts(row);});
        }
        if(name=="name"){
            if(!envh::isValidEnvName(value.toStdString())){error="E_ENV_NAME";return false;}
            for(const auto& x:work.env)if(x.id!=row&&!x.deleted&&x.scope==v->scope&&x.name.compare(value,Qt::CaseInsensitive)==0){error="E_ENV_DUP";return false;}
        }
        if(name=="type"&&value!="REG_SZ"&&value!="REG_EXPAND_SZ"&&value!="REG_MULTI_SZ"){error="E_ENV_TYPE";return false;}
        if((name=="value"&&v->type=="REG_MULTI_SZ")||(name=="type"&&value=="REG_MULTI_SZ")){
            const auto text=name=="value"?value:v->value;
            if(!text.isEmpty()&&(text.startsWith('\n')||text.endsWith('\n')||text.contains("\n\n")||text.contains('\r'))){error="E_MULTI_EMPTY";return false;}
        }
        if(field(row,name)==value)return true;
        return mutate([&]{auto* e=env(row);if(name=="name")e->name=value;else {if(name=="type")e->type=value;else e->value=value;e->encode();}
            if(name!="name"&&work.parts.contains(row)){const auto d=work.parts[row].delimiter;work.parts.remove(row);if(e->type!="REG_MULTI_SZ"){
                PartsState p;p.delimiter=d;for(const auto& s:envh::edit::splitDelimited(e->value.toStdString(),d.toStdString(),true))p.items.append({freshId(),qs(s),false,false});work.parts[row]=p;}}
        });
    }
    const int i=hostIndex(row);if(i<0||work.hosts.rows[size_t(i)].deleted)return false;
    if(name=="ip"&&QHostAddress(value).isNull()){error="E_HOSTS_IP";return false;}
    if(name=="names"){
        const auto names=words(value);if(names.isEmpty()){error="E_HOSTS_HOST";return false;}
        for(const auto& n:names)if(!envh::hostValid(n.toStdString())){error="E_HOSTS_HOST";return false;}
    }
    if(value.contains('\n')||value.contains('\r')){error="E_LINE_BREAK";return false;}
    return mutate([&]{auto& h=work.hosts.rows[size_t(i)];if(name=="ip")h.ip=value.toStdString();else if(name=="comment")h.comment=value.toStdString();else{
        h.hostnames.clear();for(const auto& n:words(value))h.hostnames.push_back(n.toStdString());}});
}
QString EditorSession::addRow(const QString& scope,const QStringList& visibleOrder) {
    if(!finishEdit()||writing)return {};
    const auto selected=rows.ordered(visibleOrder.isEmpty()?visible({},scope):visibleOrder);
    const auto after=selected.isEmpty()?QString{}:selected.last();
    if(kind=="env"){
        if(scope!="user"&&scope!="system")return {};
        if(!(scope=="user"?base.userWritable:base.systemWritable))return {};
        EnvValue v;v.id=freshId();v.scope=scope;v.isNew=true;v.name="NEW_VARIABLE";
        int suffix=1;const auto used=[&]{for(const auto& e:work.env)if(!e.deleted&&e.key()==v.key())return true;return false;};
        while(used())v.name="NEW_VARIABLE_"+QString::number(suffix++);v.encode();
        int position=int(work.env.size());
        for(int i=0;i<work.env.size();++i)if(work.env[i].scope==scope){position=i+1;if(work.env[i].id==after)break;}
        mutate([&]{work.env.insert(position,v);});rows.select(v.id,visible());return v.id;
    }
    if(!base.hostsWritable)return {};
    const auto id=freshId();const int position=after.isEmpty()?int(work.hostIds.size()):hostIndex(after)+1;
    mutate([&]{envh::edit::HostsRow h;h.isNew=true;h.ip="127.0.0.1";h.hostnames={"new.test"};work.hosts.rows.insert(work.hosts.rows.begin()+position,h);work.hostIds.insert(position,id);});
    rows.select(id,visible());return id;
}
int EditorSession::deleteRows(const QStringList& ids,bool deleted) {
    if(!finishEdit()||writing)return 0;int changed=0;
    mutate([&]{for(const auto& id:ids){if(!writable(id))continue;
        if(kind=="env"){auto* v=env(id);if(v->isNew&&deleted){const int index=int(v-work.env.data());work.env.removeAt(index);work.parts.remove(id);++changed;}
            else if(v->deleted!=deleted){v->deleted=deleted;++changed;}}
        else{const int i=hostIndex(id);auto& h=work.hosts.rows[size_t(i)];if(h.isNew&&deleted){work.hosts.rows.erase(work.hosts.rows.begin()+i);work.hostIds.removeAt(i);++changed;}
            else if(h.deleted!=deleted){h.deleted=deleted;++changed;}}
    }});rows.prune(visible());return changed;
}
int EditorSession::enableRows(const QStringList& ids,bool enabled) {
    if(kind!="hosts"||!finishEdit()||writing)return 0;int changed=0;
    mutate([&]{for(const auto& id:ids)if(writable(id)){auto& h=work.hosts.rows[size_t(hostIndex(id))];if(!h.deleted&&h.commented==enabled){h.commented=!enabled;++changed;}}});return changed;
}
QString EditorSession::addPart(const QString& row) {
    if(!finishEdit()||!writable(row)||!ensureParts(row)||env(row)->deleted)return {};
    const auto order=partIds(row);const auto selected=partSelections.value(row).ordered(order);
    const int position=selected.isEmpty()?int(order.size()):int(order.indexOf(selected.last()))+1;
    const auto id=freshId();mutate([&]{work.parts[row].items.insert(position,{id,{},true,false});syncParts(row);});
    partSelections[row].select(id,partIds(row));return id;
}
QString EditorSession::duplicateParts(const QString& row) {
    if(!finishEdit()||!writable(row)||!ensureParts(row)||env(row)->deleted)return {};
    const auto order=partIds(row);const auto selection=partSelections.value(row);
    const int current=int(order.indexOf(selection.current));if(current<0)return {};
    auto chosen=selection.ids;chosen.insert(selection.current);
    // Snapshot all sources before inserting so a batch never copies its own new items.
    QVector<Part> copies;QStringList ids;
    for(const auto& part:work.parts.value(row).items)if(chosen.contains(part.id)){
        const auto id=freshId();copies.append({id,part.value,true,false});ids.append(id);
    }
    if(!mutate([&]{auto& items=work.parts[row].items;
        for(int i=0;i<copies.size();++i)items.insert(current+1+i,copies[i]);
        syncParts(row);
    }))return {};
    auto& next=partSelections[row];next.ids=QSet<QString>(ids.begin(),ids.end());
    next.current=ids.first();next.anchor=next.current;return next.current;
}
bool EditorSession::deleteParts(const QString& row,const QStringList& ids,bool deleted) {
    if(!finishEdit()||!writable(row)||!ensureParts(row)||env(row)->deleted)return false;
    const QSet<QString> chosen(ids.begin(),ids.end());
    const bool ok=mutate([&]{auto& items=work.parts[row].items;
        for(int i=int(items.size())-1;i>=0;--i)if(chosen.contains(items[i].id)){
            if(items[i].isNew&&deleted)items.removeAt(i);else items[i].deleted=deleted;}
        syncParts(row);});partSelections[row].prune(partIds(row));return ok;
}
bool EditorSession::moveParts(const QString& row,const QStringList& ids,int direction) {
    if(!finishEdit()||!writable(row)||!ensureParts(row)||env(row)->deleted)return false;
    const QSet<QString> chosen(ids.begin(),ids.end());
    if(direction==0)return true;
    return mutate([&]{auto& p=work.parts[row].items;
        if(direction<0){for(int i=1;i<p.size();++i)if(chosen.contains(p[i].id)&&!chosen.contains(p[i-1].id))std::swap(p[i],p[i-1]);}
        else {for(int i=int(p.size())-2;i>=0;--i)if(chosen.contains(p[i].id)&&!chosen.contains(p[i+1].id))std::swap(p[i],p[i+1]);}
        syncParts(row);
    });
}
bool EditorSession::delimiter(const QString& row,const QString& value,bool confirmed) {
    if(!finishEdit()||!writable(row)||!ensureParts(row))return false;
    if(value.isEmpty()){error="E_DELIMITER_EMPTY";return false;}
    if(value==" "&&!confirmed){error="E_NORMALIZE_CONFIRM";return false;}
    if(value==work.parts[row].delimiter)return true;
    return mutate([&]{PartsState p;p.delimiter=value;for(const auto& v:envh::edit::splitDelimited(env(row)->value.toStdString(),value.toStdString(),true))
        p.items.append({freshId(),qs(v),false,false});work.parts[row]=p;syncParts(row);});
}
bool EditorSession::history(bool redo) {
    if(writing||buffer.pending()||buffer.composing)return false;
    auto& from=redo?redoStates:undoStates;auto& to=redo?undoStates:redoStates;if(from.isEmpty())return false;
    to.append(work);work=from.takeLast();++revision;buffer={};error.clear();rows.prune(visible());
    for(auto it=partSelections.begin();it!=partSelections.end();++it)it->prune(partIds(it.key()));return true;
}
QVector<Change> EditorSession::changes() const {
    QVector<Change> out;if(kind!="env")return out;
    QHash<QString,EnvValue> before,after;
    for(const auto& v:base.env)before.insert(v.key(),v);
    for(const auto& v:work.env)if(!v.deleted)after.insert(v.key(),v);
    QStringList keys=before.keys();for(const auto& key:after.keys())if(!before.contains(key))keys.append(key);keys.sort();
    for(const auto& k:keys){const bool a=before.contains(k),b=after.contains(k);if(a&&b&&sameValue(before[k],after[k]))continue;
        Change c;c.hasBefore=a;c.hasAfter=b;c.before=before.value(k);c.after=after.value(k);c.scope=(b?c.after:c.before).scope;c.name=(b?c.after:c.before).name;
        c.operation=!a?"add":!b?"delete":"modify";out.append(c);}
    return out;
}
QStringList EditorSession::validation() const {
    QStringList errors;
    if(kind=="env"){
        QSet<QString> keys;
        for(const auto& v:work.env)if(!v.deleted){
            if(!envh::isValidEnvName(v.name.toStdString()))errors.append("E_ENV_NAME: "+v.name);
            if(keys.contains(v.key()))errors.append("E_ENV_DUP: "+v.name);keys.insert(v.key());
            if(v.formatError.isEmpty()&&v.value.contains(QChar(0)))errors.append("E_NUL: "+v.name);
        }
    }else for(size_t i=0;i<work.hosts.rows.size();++i){const auto& h=work.hosts.rows[i];if(!h.deleted&&!h.isSection){
        // Existing opaque raw lines are preserved. Newly edited entries use strict validation.
        if(!h.isNew&&work.hosts.rowState(i)==envh::edit::RowState::Unchanged)continue;
        if(QHostAddress(qs(h.ip)).isNull())errors.append("E_HOSTS_IP");
        if(h.hostnames.empty())errors.append("E_HOSTS_HOST");
        for(const auto& n:h.hostnames)if(!envh::hostValid(n))errors.append("E_HOSTS_HOST");
    }}
    return errors;
}
QStringList EditorSession::warnings() const {
    QStringList out;if(kind=="env")for(const auto& v:work.env)if(!v.deleted&&v.value.size()>=29491)
        out.append(v.name+": "+QString::number(v.value.size())+" / 32767");return out;
}
bool EditorSession::restore(const TargetSnapshot& snapshot,const QString& scope) {
    if(!finishEdit()||writing)return false;
    if(kind=="hosts"){if(!base.hostsWritable)return false;return mutate([&]{buildHosts(snapshot.hosts);});}
    if(scope!="user"&&scope!="system"&&scope!="all")return false;
    if((scope!="system"&&!base.userWritable)||(scope!="user"&&!base.systemWritable)){error="E_DENIED";return false;}
    return mutate([&]{QHash<QString,EnvValue> desired;for(const auto& v:snapshot.env)if(scope=="all"||v.scope==scope)desired.insert(v.key(),v);
        for(int i=int(work.env.size())-1;i>=0;--i){auto& v=work.env[i];if(scope!="all"&&v.scope!=scope)continue;
            if(desired.contains(v.key())){auto next=desired.take(v.key());next.id=v.id;next.isNew=v.isNew;v=next;}
            else if(v.isNew)work.env.removeAt(i);else v.deleted=true;
        }
        for(auto v:desired){v.id=freshId();v.isNew=true;work.env.append(v);}work.parts.clear();
    });
}
QJsonObject EditorSession::draft(const QString& identity) const {
    auto state=QJsonDocument::fromJson(stateBytes()).object();
    return {{"schemaVersion",2},{"kind",kind},{"targetIdentity",identity},{"base",base.json()},{"baselineUnknown",baselineUnknown},{"state",state},
        {"buffer",QJsonObject{{"row",buffer.row},{"part",buffer.part},{"field",buffer.field},{"text",buffer.text},{"before",buffer.before}}}};
}
bool EditorSession::restoreDraft(const QJsonObject& object,const QString& identity) {
    if(!object.contains("schemaVersion")&&object["rows"].isArray()){
        // Legacy drafts carry no baseline or target identity. Import into a copy,
        // preserve unrelated current values and force an explicit conflict review.
        EditorSession migrated=*this;migrated.cancelEdit();migrated.work.parts.clear();
        if(kind=="env"){
            QSet<QString> keys;
            for(const auto& j:object["rows"].toArray()){
                if(!j.isObject()){error="E_DRAFT_FORMAT";return false;}const auto o=j.toObject();auto v=EnvValue::fromJson(o);
                if(v.scope.isEmpty())v.scope="user";
                if((v.scope!="user"&&v.scope!="system")||!envh::isValidEnvName(v.name.toStdString())||!o["value"].isString()||
                    (v.type!="REG_SZ"&&v.type!="REG_EXPAND_SZ"&&v.type!="REG_MULTI_SZ")||keys.contains(v.key())||v.value.contains(QChar(0))){error="E_DRAFT_FORMAT";return false;}
                keys.insert(v.key());auto found=std::find_if(migrated.work.env.begin(),migrated.work.env.end(),[&](const auto& e){return e.key()==v.key();});
                if(v.type=="REG_MULTI_SZ"){
                    if(found==migrated.work.env.end()||found->type!="REG_MULTI_SZ"||QString(found->value).replace('\n',';')!=v.value){error="E_DRAFT_MULTI_LEGACY";return false;}
                    v.value=found->value;v.bytes=found->bytes;v.nativeType=found->nativeType;
                }else v.encode();
                v.id=found==migrated.work.env.end()?freshId():found->id;v.isNew=found==migrated.work.env.end();
                if(found==migrated.work.env.end())migrated.work.env.append(v);else *found=v;
            }
        }else{
            const auto parsed=winshun::Json::parse(QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString());
            if(!parsed){error="E_DRAFT_FORMAT";return false;}
            const auto baseBytes=object.contains("baseText")?object["baseText"].toString().toUtf8():base.hosts;
            const auto lineCount=envh::parseHosts(baseBytes.toStdString()).lines.size();
            for(const auto& j:object["rows"].toArray()){const int origin=j.toObject()["origin"].toInt(-1);if(origin< -1||(origin>=0&&size_t(origin)>=lineCount)){error="E_DRAFT_FORMAT";return false;}}
            migrated.work.hosts.fromDraft(*parsed);migrated.work.hostIds.clear();for(const auto& h:migrated.work.hosts.rows){(void)h;migrated.work.hostIds.append(freshId());}
        }
        auto envelope=migrated.draft(identity);envelope["baselineUnknown"]=true;return restoreDraft(envelope,identity);
    }
    if(object["schemaVersion"].toInt()!=2||object["kind"].toString()!=kind||object["targetIdentity"].toString()!=identity){error="E_DRAFT_FORMAT";return false;}
    const auto state=object["state"].toObject();if(!state["env"].isArray()||!state["hosts"].isObject()||!object["base"].isObject()){error="E_DRAFT_FORMAT";return false;}
    WorkingState next;QSet<QString> seen;
    for(const auto& j:state["env"].toArray()){auto v=EnvValue::fromJson(j.toObject());if(v.id.isEmpty()||seen.contains(v.id)||(v.scope!="user"&&v.scope!="system")||!envh::isValidEnvName(v.name.toStdString())){error="E_DRAFT_ID";return false;}
        auto encoded=v;encoded.encode();if(v.formatError.isEmpty()&&(encoded.bytes!=v.bytes||encoded.nativeType!=v.nativeType)){error="E_DRAFT_ENCODING";return false;}seen.insert(v.id);next.env.append(v);}
    const auto b=object["base"].toObject();TargetSnapshot original=base;original.env.clear();
    for(const auto& j:b["vars"].toArray())original.env.append(EnvValue::fromJson(j.toObject()));
    original.hosts=QByteArray::fromBase64(b["hostsBase64"].toString().toLatin1());
    next.hosts=envh::edit::HostsModel::fromParsed(envh::parseHosts(original.hosts.toStdString()),true);
    const auto parsed=winshun::Json::parse(QJsonDocument(state["hosts"].toObject()).toJson(QJsonDocument::Compact).toStdString());
    if(!parsed){error="E_DRAFT_FORMAT";return false;}
    const auto hostState=state["hosts"].toObject();const auto hostBytes=hostState.contains("baseText")?hostState["baseText"].toString().toUtf8():original.hosts;
    const auto lineCount=envh::parseHosts(hostBytes.toStdString()).lines.size();QSet<int> origins;
    for(const auto& j:hostState["rows"].toArray()){const int origin=j.toObject()["origin"].toInt(-1);if(origin< -1||(origin>=0&&(size_t(origin)>=lineCount||origins.contains(origin)))){error="E_DRAFT_FORMAT";return false;}if(origin>=0)origins.insert(origin);}
    next.hosts.fromDraft(*parsed);
    QSet<QString> hostIds;for(const auto& j:state["hostIds"].toArray()){const auto id=j.toString();if(id.isEmpty()||hostIds.contains(id)){error="E_DRAFT_ID";return false;}hostIds.insert(id);next.hostIds.append(id);}
    if(next.hostIds.size()!=qsizetype(next.hosts.rows.size())){error="E_DRAFT_FORMAT";return false;}
    const auto all=state["parts"].toObject();
    for(auto it=all.begin();it!=all.end();++it){PartsState p;const auto po=it.value().toObject();p.delimiter=po["delimiter"].toString();
        if(p.delimiter.isEmpty()||!seen.contains(it.key())){error="E_DRAFT_FORMAT";return false;}QSet<QString> partIds;
        for(const auto& j:po["items"].toArray()){const auto o=j.toObject();const auto id=o["id"].toString();if(id.isEmpty()||partIds.contains(id)){error="E_DRAFT_ID";return false;}
            partIds.insert(id);p.items.append({id,o["value"].toString(),o["new"].toBool(),o["deleted"].toBool()});}
        for(const auto& v:next.env)if(v.id==it.key()&&(v.type=="REG_MULTI_SZ"||partsValue(p)!=v.value)){error="E_DRAFT_PARTS";return false;}
        next.parts.insert(it.key(),p);
    }
    base=original;work=next;baselineUnknown=object["baselineUnknown"].toBool();undoStates.clear();redoStates.clear();buffer={};++revision;
    const auto bf=object["buffer"].toObject();if(!bf["field"].toString().isEmpty()){
        buffer={freshId(),bf["row"].toString(),bf["part"].toString(),bf["field"].toString(),bf["before"].toString(),bf["text"].toString(),false};}
    rows={};const auto view=visible();if(!view.isEmpty())rows.select(view.contains(buffer.row)?buffer.row:view[0],view);return true;
}

} // namespace editorwin
