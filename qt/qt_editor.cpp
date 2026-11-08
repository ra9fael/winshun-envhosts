#include "qt_editor.h"
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QQuickStyle>
#include <QPointer>
#include <QThread>
#include <QDebug>
#include <QFontMetricsF>
#include <QSettings>
#include <QDir>
#include <QElapsedTimer>
#include <psapi.h>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <cmath>

namespace editorwin {
namespace {
QString qs(const std::string& s){return QString::fromUtf8(s.data(),qsizetype(s.size()));}
QString hostNames(const envh::edit::HostsRow& h){QStringList names;for(const auto& n:h.hostnames)names.append(qs(n));return names.join(" ");}
QString valueLabel(const EnvValue& v){return v.type+"\n"+v.value;}
void logMemory(const char* phase){
    PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);
    if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters)))
        qInfo().nospace()<<"qml-memory "<<phase<<" workingMiB="<<counters.WorkingSetSize/1048576
            <<" privateMiB="<<counters.PrivateUsage/1048576;
}
}

EditorBridge::EditorBridge(OpenArgs a,QObject* parent,std::shared_ptr<StoreBackend> backend)
    :QObject(parent),args(std::move(a)),edit(qs(args.kind)),native(std::make_shared<NativeBackend>(args)) {
    store=backend?std::move(backend):native;filter=qs(args.filter);page=args.kind=="env"?"env":"hosts";
    if(!args.dataDir.empty()){
        QSettings settings(qs(args.dataDir)+"/qml-view.ini",QSettings::IniFormat);
        for(const auto& entry:columns()){const auto c=entry.toMap();const auto key=c["key"].toString();
            if(settings.contains(edit.kind+"/"+key))columnWidths[key]=std::clamp(settings.value(edit.kind+"/"+key).toDouble(),c["minimum"].toDouble(),c["maximum"].toDouble());}
    }
    watcher.setInterval(2000);connect(&watcher,&QTimer::timeout,this,&EditorBridge::poll);watcher.start();
    draftTimer.setInterval(30000);connect(&draftTimer,&QTimer::timeout,this,[this]{if(ready&&edit.dirty()&&!busy&&dialog.isEmpty())persistDraft();});draftTimer.start();
}
QVariantList EditorBridge::columns() const {
    QVariantList out;
    auto add=[&](QString key,QString title,int width,int minimum,int maximum){out.append(QVariantMap{{"key",key},{"title",title},{"width",columnWidths.value(key,width)},{"minimum",minimum},{"maximum",maximum},{"explicit",columnWidths.contains(key)}});};
    if(edit.kind=="env"){
        add("name",trText("变量名","Name"),182,100,480);add("value",trText("值","Value"),320,160,800);add("type",trText("类型","Type"),150,130,300);
    }else{
        add("enabled",trText("启停","Enabled"),90,82,180);add("ip","IP",150,110,380);add("names",trText("主机名","Hostnames"),280,140,700);add("comment",trText("行尾注释","Comment"),180,120,600);
    }
    add("state",trText("修改状态","Changes"),94,88,220);return out;
}
bool EditorBridge::resizeColumn(const QString& key,qreal width){
    if(!dialog.isEmpty()||!finish()||!std::isfinite(width))return false;
    for(const auto& entry:columns()){const auto c=entry.toMap();if(c["key"].toString()!=key)continue;
        const qreal value=std::clamp(width,c["minimum"].toDouble(),c["maximum"].toDouble());
        if(!columnWidths.contains(key)||columnWidths.value(key)!=value){columnWidths[key]=value;emit columnsChanged();}return true;}
    return false;
}
void EditorBridge::saveColumns(){
    if(args.dataDir.empty())return;QDir().mkpath(qs(args.dataDir));QSettings settings(qs(args.dataDir)+"/qml-view.ini",QSettings::IniFormat);
    for(auto it=columnWidths.cbegin();it!=columnWidths.cend();++it)settings.setValue(edit.kind+"/"+it.key(),it.value());
}
void EditorBridge::autoSizeColumn(const QString& key,const QFont& font){
    if(!finish()||!dialog.isEmpty())return;const QFontMetricsF metrics(font);qreal width=0,maximum=800;
    for(const auto& entry:columns()){const auto c=entry.toMap();if(c["key"].toString()==key){width=metrics.horizontalAdvance(c["title"].toString())+30;maximum=c["maximum"].toDouble();}}
    // Scan current records with capped text; no delegate creation or model mutation.
    for(int i=0;i<rowModel.rowCount()&&width<maximum;++i){const auto r=rowModel.get(i);QString text=r[key].toString();
        if(key=="state")text=trText("● 已修改","Modified");if(key=="enabled")text=trText("Ⅱ 停用","Disabled");
        width=std::max(width,metrics.horizontalAdvance(text.left(512))+24);}
    if(resizeColumn(key,width))saveColumns();
}
QStringList EditorBridge::visibleIds()const {
    auto ids=edit.visible(filter,scope,args.showSystem);if(sortKey.isEmpty()){
        if(!manualOrder.isEmpty()){QHash<QString,int> positions;for(int i=0;i<manualOrder.size();++i)positions.insert(manualOrder[i],i);
            std::stable_sort(ids.begin(),ids.end(),[&](const QString& a,const QString& b){return positions.value(a,int(manualOrder.size()))<positions.value(b,int(manualOrder.size()));});}
        return ids;
    }
    QHash<QString,QString> values;
    QSet<QString> dirtyKeys;if(edit.kind=="env"&&sortKey=="state")for(const auto& c:edit.changes())dirtyKeys.insert(envKey(c.scope,c.name));
    if(edit.kind=="env")for(const auto& v:edit.work.env)values.insert(v.id,sortKey=="name"?v.name:sortKey=="type"?v.type:sortKey=="state"?(v.deleted?"3":v.isNew?"2":dirtyKeys.contains(v.key())?"1":"0"):v.value);
    else for(int i=0;i<edit.work.hostIds.size();++i){const auto& h=edit.work.hosts.rows[size_t(i)];const auto st=sortKey=="state"?edit.work.hosts.rowState(size_t(i)):envh::edit::RowState::Unchanged;values.insert(edit.work.hostIds[i],sortKey=="ip"?qs(h.ip):sortKey=="names"?hostNames(h):sortKey=="comment"?qs(h.comment):sortKey=="enabled"?(h.commented?"0":"1"):h.deleted?"3":st==envh::edit::RowState::Added?"2":st==envh::edit::RowState::Modified?"1":"0");}
    std::stable_sort(ids.begin(),ids.end(),[&](const QString& a,const QString& b){const int c=QString::compare(values.value(a),values.value(b),Qt::CaseInsensitive);return sortDescending?c>0:c<0;});return ids;
}
bool EditorBridge::sortColumn(const QString& key){
    if(!dialog.isEmpty()||!finish())return false;bool found=false;for(const auto& c:columns())found|=c.toMap()["key"].toString()==key;if(!found)return false;
    sortDescending=sortKey==key?!sortDescending:false;sortKey=key;manualOrder.clear();rebuild();return true;
}
void EditorBridge::rememberScope(){views[scope]={filter,sortKey,sortDescending,edit.rows,scrollPosition,manualOrder};}
void EditorBridge::rememberScroll(qreal y){scrollPosition=std::max(qreal(0),y);}
bool EditorBridge::selectCell(const QString& id,int modifiers,bool part){
    if(modifiers&(Qt::ControlModifier|Qt::ShiftModifier))return select(id,modifiers,part);
    if(!dialog.isEmpty()||!finish())return false;
    const auto ids=part?edit.partIds(edit.rows.current):visibleIds();if(!ids.contains(id))return false;
    auto& selection=part?edit.partSelections[edit.rows.current]:edit.rows;
    if(selection.ids.contains(id))selection.current=id;else selection.select(id,ids);rebuild();return true;
}
bool EditorBridge::activateCell(const QString& id,const QString& field){
    if(!selectCell(id))return false;
    if(field=="state"||field=="enabled"){
        const auto index=visibleIds().indexOf(id);const auto r=rowModel.get(index);
        showDialog("info",trText("条目状态","Entry status"),r["state"].toString()=="deleted"?trText("待删除；保存前可恢复。","Marked for deletion; restore before saving."):edit.kind=="hosts"?(r["enabled"].toBool()?trText("已启用","Enabled"):trText("已停用","Disabled")):r["state"].toString()=="clean"?trText("与目标一致","In sync with target"):trText("工作副本已修改，尚未保存。","Working copy changed; not saved."));return true;
    }
    if(!edit.writable(id)||state()["detail"].toMap()["deleted"].toBool())return false;
    if(edit.kind=="env"&&field=="value")mode="raw";rebuild();emit focusField(id,field,{});return true;
}
void EditorBridge::toggleSelected(){
    if(!finish()||!dialog.isEmpty()||edit.kind!="hosts")return;bool enable=false;
    for(const auto& id:edit.rows.ordered(visibleIds())){const auto& h=edit.work.hosts.rows[size_t(edit.hostIndex(id))];if(edit.writable(id)&&!h.deleted&&h.commented)enable=true;}
    enableSelected(enable);
}
QVariantMap EditorBridge::state() const {
    const auto id=edit.rows.current;QVariantMap detail;
    if(edit.kind=="env"){
        if(const auto* v=edit.env(id))detail={{"id",id},{"name",v->name},{"value",v->value},{"type",v->type},{"scope",v->scope},
            {"deleted",v->deleted},{"writable",edit.writable(id)&&!v->deleted},{"formatError",v->formatError},{"length",v->value.size()}};
    }else{const int i=edit.hostIndex(id);if(i>=0){const auto& h=edit.work.hosts.rows[size_t(i)];detail={{"id",id},{"ip",qs(h.ip)},
        {"names",hostNames(h)},{"comment",qs(h.comment)},{"deleted",h.deleted},{"section",h.isSection},{"enabled",!h.commented},{"writable",edit.writable(id)&&!h.deleted}};}}
    const auto envChanges=edit.changes();int userChanges=0,systemChanges=0;for(const auto& c:envChanges)(c.scope=="user"?userChanges:systemChanges)++;
    const int changes=edit.kind=="env"?int(envChanges.size()):int(edit.work.hosts.changeList().size());
    const auto p=edit.work.parts.value(id);int active=0,deleted=0;for(const auto& x:p.items)(x.deleted?deleted:active)++;
    return {{"kind",edit.kind},{"zh",args.zh},{"dark",args.dark},{"ready",ready},{"busy",busy},{"writing",edit.writing},
        {"title",trText(args.kind=="env"?"环境变量":"hosts 文件",args.kind=="env"?"Environment variables":"hosts file")},
        {"page",page},{"mode",mode},{"filter",filter},{"scope",scope},{"showSystem",args.showSystem},
        {"userChanges",userChanges},{"systemChanges",systemChanges},{"scroll",scrollPosition},{"sortKey",sortKey},{"sortDescending",sortDescending},
        {"dirty",edit.dirty()},{"pending",edit.buffer.pending()},{"canUndo",edit.canUndo()},{"canRedo",edit.canRedo()},
        {"userWritable",edit.base.userWritable},{"systemWritable",edit.base.systemWritable},{"hostsWritable",edit.base.hostsWritable},
        {"currentId",id},{"selected",edit.rows.ids.size()},{"partSelected",edit.partSelections.value(id).ids.size()},
        {"partCurrent",edit.partSelections.value(id).current},{"partActive",active},{"partDeleted",deleted},{"delimiter",p.delimiter},
        {"count",rowModel.rowCount()},{"changeCount",changes},{"detail",detail},{"raw",edit.rawText()},
        {"error",error},{"notice",notice},{"validation",edit.validation()},{"warnings",edit.warnings()},{"conflict",conflicted},
        {"dialog",dialog},{"dialogTitle",dialogTitle},{"dialogText",dialogText},{"diff",diff},{"backupId",backupId},{"backupScope",backupScope},{"backupAvailableScope",backupAvailableScope}};
}
void EditorBridge::fail(const QString& code){error=code;emit changed();}
bool EditorBridge::finish(){
    if(busy||edit.writing)return false;
    if(!edit.finishEdit()){if(!edit.error.isEmpty())fail(edit.error);emit focusField(edit.buffer.row,edit.buffer.field,edit.buffer.part);return false;}
    error.clear();return true;
}
bool EditorBridge::finishEdit(){const bool ok=finish();if(ok)rebuild();return ok;}
void EditorBridge::rebuild(){
    const auto ids=visibleIds();edit.rows.prune(ids);QVector<QVariantMap> values;
    const auto changes=edit.changes();QSet<QString> dirtyKeys;for(const auto& c:changes)dirtyKeys.insert(envKey(c.scope,c.name));
    QHash<QString,const EnvValue*> envById;for(const auto& v:edit.work.env)envById.insert(v.id,&v);
    const auto duplicates=edit.work.hosts.duplicateFlags();
    for(const auto& id:ids){QVariantMap r{{"id",id},{"selected",edit.rows.ids.contains(id)},{"focused",edit.rows.current==id},{"writable",edit.kind=="env"?edit.writable(*envById[id]):edit.writable(id)}};
        if(edit.kind=="env"){const auto& v=*envById[id];const auto state=v.deleted?"deleted":v.isNew?"added":dirtyKeys.contains(v.key())?"modified":"clean";
            r.insert("c1",v.name);r.insert("c2",v.scope=="user"?trText("用户","User"):trText("系统","System"));r.insert("c3",v.type);r.insert("c4",v.value);r.insert("state",state);r.insert("section",false);r.insert("deleted",v.deleted);
            r.insert("name",v.name);r.insert("value",v.value);r.insert("type",v.type);
        }else{const int i=edit.hostIndex(id);const auto& h=edit.work.hosts.rows[size_t(i)];const auto st=edit.work.hosts.rowState(size_t(i));
            r.insert("c1",h.isSection?qs(h.sectionText):qs(h.ip));r.insert("c2",h.isSection?"":h.commented?trText("停用","Off"):trText("启用","On"));
            r.insert("c3",h.isSection?"":hostNames(h));r.insert("c4",qs(h.comment));r.insert("section",h.isSection);r.insert("deleted",h.deleted);
            r.insert("state",h.deleted?"deleted":st==envh::edit::RowState::Added?"added":st==envh::edit::RowState::Modified?"modified":"clean");
            r.insert("duplicate",i<int(duplicates.size())&&duplicates[size_t(i)]);r.insert("line",h.origin+1);
            r.insert("enabled",!h.commented);r.insert("ip",qs(h.ip));r.insert("names",hostNames(h));r.insert("comment",qs(h.comment));
        }values.append(r);
    }rowModel.sync(values);
    QVector<QVariantMap> ps;const auto id=edit.rows.current;const auto* v=edit.env(id);
    if(v&&!v->deleted){if(auto* p=edit.ensureParts(id)){
        auto& selection=edit.partSelections[id];const auto pids=edit.partIds(id);selection.prune(pids);
        for(const auto& part:p->items)ps.append({{"id",part.id},{"row",id},{"value",part.value},{"deleted",part.deleted},{"isNew",part.isNew},
            {"selected",selection.ids.contains(part.id)},{"focused",selection.current==part.id},{"writable",edit.writable(id)&&!part.deleted}});
    }}partModel.sync(ps);emit changed();
}
void EditorBridge::adoptSnapshot(const TargetSnapshot& snapshot){
    const auto selection=edit.rows;const auto before=visibleIds();
    edit.load(snapshot,true);
    // The bridge owns the active scope, filter and sort, so fallback must use
    // its visible order rather than the complete registry snapshot.
    if(ready&&edit.kind=="env"){edit.rows=selection;edit.rows.restore(before,visibleIds());}
}
void EditorBridge::load(){
    if(busy||edit.writing)return;
    const auto backend=store;const auto n=native;
    job([backend,n]{auto s=backend->read();QString error;auto d=n->readDraft(error);return std::make_tuple(s,d,error);},[this](const auto& result){
        const auto s=std::get<0>(result);if(!s.error.isEmpty()){fail(s.error);return;}
        adoptSnapshot(s);ready=true;conflicted=false;error=std::get<2>(result);draftObject=std::get<1>(result);rebuild();
        if(!args.select.empty())applyFocus(args);
        if(!draftObject.isEmpty())showDialog("draft",trText("发现未保存草稿","Unsaved draft found"),trText("恢复到工作副本后，仍需明确保存到系统。","Restoring a draft does not write the target."));
    });
}
bool EditorBridge::navigate(const QString& next){
    if(!dialog.isEmpty()||!finish())return false;
    if(next!="backup"&&next!=(edit.kind=="env"?"env":"hosts")&&!(edit.kind=="hosts"&&next=="raw"))return false;
    page=next;rebuild();if(page=="backup")refreshBackups();return true;
}
bool EditorBridge::setFilter(const QString& text){if(!finish())return false;filter=text;rebuild();return true;}
bool EditorBridge::setScope(const QString& next){
    if(edit.kind!="env"||!dialog.isEmpty()||(next!="user"&&next!="system")||(next=="system"&&!args.showSystem)||!finish())return false;
    if(next==scope)return true;rememberScope();emit scopeChanging();scope=next;
    const auto v=views.value(scope);filter=v.filter;sortKey=v.sort;sortDescending=v.descending;scrollPosition=v.scroll;edit.rows=v.selection;manualOrder=v.manualOrder;
    const auto ids=visibleIds();if(edit.rows.current.isEmpty()&&!ids.isEmpty())edit.rows.select(ids.first(),ids);rebuild();return true;
}
bool EditorBridge::select(const QString& id,int modifiers,bool part){
    if(!dialog.isEmpty()||!finish())return false;const bool ctrl=modifiers&Qt::ControlModifier,shift=modifiers&Qt::ShiftModifier;
    if(part)edit.partSelections[edit.rows.current].select(id,edit.partIds(edit.rows.current),ctrl,shift);else edit.rows.select(id,visibleIds(),ctrl,shift);rebuild();return true;
}
void EditorBridge::selectAll(bool part){if(!finish()||!dialog.isEmpty())return;if(part)edit.partSelections[edit.rows.current].all(edit.partIds(edit.rows.current));else edit.rows.all(visibleIds());rebuild();}
void EditorBridge::clearSelection(bool part){if(!finish()||!dialog.isEmpty())return;if(part)edit.partSelections[edit.rows.current].ids.clear();else edit.rows.ids.clear();rebuild();}
void EditorBridge::moveFocus(int delta,int modifiers,bool part){
    const auto ids=part?edit.partIds(edit.rows.current):visibleIds();if(ids.isEmpty())return;
    const auto id=part?edit.partSelections.value(edit.rows.current).current:edit.rows.current;
    const auto index=std::clamp(int(ids.indexOf(id))+delta,0,int(ids.size())-1);select(ids[index],modifiers,part);
}
QString EditorBridge::beginEdit(const QString& row,const QString& field,const QString& part){if(busy||!dialog.isEmpty()||(edit.kind=="env"&&!visibleIds().contains(row)))return {};return edit.beginEdit(row,field,part);}
void EditorBridge::updateEdit(const QString& token,const QString& text,bool composing){edit.updateEdit(token,text,composing);reviewToken.clear();emit changed();}
void EditorBridge::cancelEdit(){edit.cancelEdit();error.clear();rebuild();}
void EditorBridge::addRow(const QString& targetScope){if(!finish()||!dialog.isEmpty())return;
    if(edit.kind=="env"&&((targetScope!="user"&&targetScope!="system")||(targetScope=="system"&&!args.showSystem)))return;
    if(edit.kind=="env"&&targetScope!=scope&&!setScope(targetScope))return;
    const auto order=visibleIds();const auto selected=edit.rows.ordered(order);const auto after=selected.isEmpty()?QString{}:selected.last();
    const auto id=edit.addRow(targetScope,order);if(id.isEmpty())return;
    filter.clear();auto insertedOrder=visibleIds();insertedOrder.removeAll(id);
    insertedOrder.insert(after.isEmpty()?insertedOrder.size():insertedOrder.indexOf(after)+1,id);
    manualOrder=insertedOrder;sortKey.clear();sortDescending=false;rebuild();emit focusField(id,edit.kind=="env"?"name":"ip",{});}
void EditorBridge::deleteSelected(bool deleted,bool part){
    if(!finish()||!dialog.isEmpty())return;
    if(part){const auto id=edit.rows.current;edit.deleteParts(id,edit.partSelections.value(id).ordered(edit.partIds(id)),deleted);}
    else{const auto ids=edit.rows.ordered(visibleIds());const int done=edit.deleteRows(ids,deleted);notice=trText("处理 %1 项，跳过 %2 项","Changed %1; skipped %2").arg(done).arg(ids.size()-done);}
    rebuild();
}
void EditorBridge::enableSelected(bool enabled){if(!finish()||!dialog.isEmpty())return;const auto ids=edit.rows.ordered(visibleIds());const int n=edit.enableRows(ids,enabled);notice=trText("处理 %1 项，跳过 %2 项","Changed %1; skipped %2").arg(n).arg(ids.size()-n);rebuild();}
void EditorBridge::addPart(){if(!finish()||!dialog.isEmpty())return;const auto row=edit.rows.current;const auto id=edit.addPart(row);rebuild();if(!id.isEmpty())emit focusField(row,"value",id);}
void EditorBridge::duplicateParts(){if(!finish()||!dialog.isEmpty())return;const auto row=edit.rows.current;const auto id=edit.duplicateParts(row);rebuild();if(!id.isEmpty())emit focusField(row,"value",id);}
void EditorBridge::moveParts(int direction){if(!finish()||!dialog.isEmpty())return;const auto row=edit.rows.current;edit.moveParts(row,edit.partSelections.value(row).ordered(edit.partIds(row)),direction);rebuild();}
void EditorBridge::setDelimiter(const QString& value){if(!finish())return;if(!edit.delimiter(edit.rows.current,value)){
    if(edit.error=="E_NORMALIZE_CONFIRM"){pendingDelimiter=value;showDialog("normalize",trText("确认归一化空格","Normalize whitespace?"),trText("空格会拆开 C:\\Program Files。将合并连续空格，可撤销。","Spaces split C:\\Program Files. Consecutive whitespace will be normalized; this can be undone."));}
    else fail(edit.error);
}rebuild();}
bool EditorBridge::setMode(const QString& next){if(!finish()||(next!="raw"&&next!="parts"))return false;mode=next;rebuild();return true;}
void EditorBridge::history(bool redo){if(busy||!dialog.isEmpty())return;if(edit.buffer.pending())return;edit.cancelEdit();edit.history(redo);rebuild();}
void EditorBridge::copySelected(bool part,bool expanded){
    QStringList out;
    if(part){const auto row=edit.rows.current;const auto selection=edit.partSelections.value(row);for(const auto& p:edit.work.parts.value(row).items)if(selection.ids.contains(p.id))out.append(p.value);}
    else for(const auto& id:edit.rows.ordered(visibleIds())){if(edit.kind=="env"){const auto* v=edit.env(id);QString value=v->value;
        if(expanded){const auto input=value.toStdWString();const DWORD n=ExpandEnvironmentStringsW(input.c_str(),nullptr,0);std::wstring expandedValue(n,L'\0');if(n&&ExpandEnvironmentStringsW(input.c_str(),expandedValue.data(),n))value=QString::fromWCharArray(expandedValue.c_str());}
        out.append(v->name+"="+value);
    }else{const int i=edit.hostIndex(id);const auto& h=edit.work.hosts.rows[size_t(i)];
        if(h.origin>=0&&edit.work.hosts.rowState(size_t(i))==envh::edit::RowState::Unchanged)out.append(qs(edit.work.hosts.base.lines[size_t(h.origin)].text));
        else out.append(h.isSection?qs(h.sectionText):(h.commented?"# ":"")+qs(h.ip)+" "+hostNames(h)+(h.comment.empty()?"":" # "+qs(h.comment)));}}
    if(!out.isEmpty())QGuiApplication::clipboard()->setText(out.join("\r\n"));
}
QVariantList EditorBridge::difference(const TargetSnapshot& from,const TargetSnapshot& to) const {
    QVariantList rows;
    if(edit.kind=="hosts"){if(from.hosts!=to.hosts)rows.append(QVariantMap{{"name","hosts"},{"operation","modify"},{"before",QString::fromUtf8(from.hosts)},{"after",QString::fromUtf8(to.hosts)}});return rows;}
    EditorSession comparison("env");comparison.load(from);comparison.work.env=to.env;
    for(const auto& c:comparison.changes())rows.append(QVariantMap{{"name",c.scope+" / "+c.name},{"operation",c.operation},{"before",c.hasBefore?valueLabel(c.before):trText("（不存在）","(absent)")},{"after",c.hasAfter?valueLabel(c.after):trText("（删除）","(deleted)")}});
    return rows;
}
void EditorBridge::showDialog(const QString& type,const QString& title,const QString& text){dialog=type;dialogTitle=title;dialogText=text;emit changed();}
void EditorBridge::requestSave(bool overwrite){
    if(!finish()||!ready)return;dialog.clear();
    if(!edit.dirty()){notice=trText("没有需要保存的修改","No changes to save");emit changed();return;}
    if(!edit.validation().isEmpty()){fail(edit.validation().join("\n"));return;}
    const auto revision=edit.revision;const auto backend=store;
    job([backend]{return backend->read();},[this,revision,overwrite](const TargetSnapshot& actual){
        if(revision!=edit.revision){fail("E_STALE");return;}if(!actual.error.isEmpty()){fail(actual.error);return;}
        external=actual;QStringList scopes;
        if(edit.kind=="env"){for(const auto& c:edit.changes())if(!scopes.contains(c.scope))scopes.append(c.scope);}else scopes={"hosts"};
        bool mismatch=edit.baselineUnknown;for(const auto& s:scopes)if(actual.fingerprint(s)!=edit.base.fingerprint(s))mismatch=true;
        if(mismatch&&!overwrite){conflicted=true;diff=difference(edit.base,actual);
            const auto mine=edit.effective();for(auto& entry:diff){auto row=entry.toMap();const QString key=row["name"].toString();QString mineText=edit.kind=="hosts"?QString::fromUtf8(mine.hosts):trText("（不存在）","(absent)");
                if(edit.kind=="env")for(const auto& v:mine.env)if(v.scope+" / "+v.name==key)mineText=valueLabel(v);row["mine"]=mineText;entry=row;}
            showDialog("conflict",edit.baselineUnknown?trText("旧草稿基线未知，请核对目标","Legacy draft baseline is unknown; review the target"):trText("目标已被外部修改","The target changed externally"),edit.kind=="env"?trText("覆盖仅应用你的变更集，保留外部无关变量。","Overwrite applies your changes and preserves unrelated variables."):trText("覆盖会替换整个 hosts 文件，丢弃这里显示的外部改动。","Overwrite replaces the entire hosts file and discards the external changes shown here."));return;}
        request={};request.kind=edit.kind;request.revision=QString::number(edit.revision);request.token=freshId();request.changes=edit.changes();request.hosts=edit.effective().hosts;
        for(const auto& s:scopes){if(!(s=="user"?actual.userWritable:s=="system"?actual.systemWritable:actual.hostsWritable)){fail("E_DENIED");return;}request.fingerprints[s]=actual.fingerprint(s);}
        reviewToken=request.token;reviewRevision=request.revision;diff=difference(edit.base,edit.effective());
        showDialog("review",trText("确认保存到系统","Review system changes"),trText("先备份，再写入并读回核对。取消可继续编辑。","A verified backup precedes writing and readback. Cancel to keep editing.")+"\n"+edit.warnings().join("\n"));
    });
}
void EditorBridge::saveConfirmed(){
    if(busy||reviewToken.isEmpty()||reviewToken!=request.token||reviewRevision!=QString::number(edit.revision)||edit.buffer.pending()){fail("E_STALE_REVIEW");dialog.clear();return;}
    reviewToken.clear();dialog.clear();edit.writing=true;const auto plan=request;const auto backend=store;
    job([backend,plan]{return WriteService::execute(*backend,plan);},[this](const WriteResult& result){
        edit.writing=false;
        if(result.state=="Succeeded"||result.state=="StoredButBroadcastFailed"){
            adoptSnapshot(result.actual);conflicted=false;error.clear();QString ignored;native->removeDraft(ignored);
            notice=result.state=="Succeeded"?trText("已保存并读回核对","Saved and verified"):trText("已保存，广播失败","Saved; environment broadcast failed");
            if(!ignored.isEmpty())error=ignored;
        }else{conflicted=result.state=="Conflict";error=result.state+": "+result.error+"\n"+trText("恢复备份：","Recovery backup: ")+result.backupId;
            if(!result.completed.isEmpty())error+="\n"+trText("已完成：","Completed: ")+result.completed.join(", ");
            if(!result.failed.isEmpty())error+="\n"+trText("失败：","Failed: ")+result.failed.join(", ");
            if(!result.notExecuted.isEmpty())error+="\n"+trText("未执行：","Not executed: ")+result.notExecuted.join(", ");
        }
        rebuild();if(edit.dirty())persistDraft();
        if(closeAfterWrite&&!busy){closeAfterWrite=false;requestClose(shutdownRequested);}
    });
}
void EditorBridge::requestReload(){if(!finish())return;if(edit.dirty()){diff.clear();showDialog("reload",trText("放弃未保存修改？","Discard unsaved changes?"),trText("重新载入将丢弃当前工作副本。","Reload discards the current working copy."));}else load();}
void EditorBridge::poll(){
    if(!ready||busy||polling||!dialog.isEmpty()||edit.buffer.composing)return;
    const auto backend=store;const auto revision=edit.revision;
    polling=true;auto* w=new QFutureWatcher<TargetSnapshot>(this);
    connect(w,&QFutureWatcher<TargetSnapshot>::finished,this,[this,w,revision]{
        const auto s=w->result();w->deleteLater();polling=false;
        if(busy||edit.writing||revision!=edit.revision)return;
        if(!s.error.isEmpty()){fail(s.error);return;}
        const bool changed=edit.kind=="hosts"?s.hosts!=edit.base.hosts:s.fingerprint("user")!=edit.base.fingerprint("user")||s.fingerprint("system")!=edit.base.fingerprint("system");
        edit.base.userWritable=s.userWritable;edit.base.systemWritable=s.systemWritable;edit.base.hostsWritable=s.hostsWritable;
        if(changed){if(edit.dirty()||!edit.buffer.token.isEmpty()){conflicted=true;external=s;}else adoptSnapshot(s);}
        rebuild();
    });
    w->setFuture(QtConcurrent::run([backend]{return backend->read();}));
}
void EditorBridge::refreshBackups(){if(busy)return;const auto n=native;job([n]{QString e;return std::make_pair(n->backups(e),e);},[this](const auto& r){
    QVector<QVariantMap> values;for(const auto& b:r.first)values.append({{"id",b.id},{"scope",b.scope},{"bytes",b.bytes}});backupModel.sync(values);if(!r.second.isEmpty())fail(r.second);
});}
void EditorBridge::previewBackup(const QString& id){if(!finish())return;const auto n=native;job([n,id]{TargetSnapshot s;QString scope,error;n->readBackup(id,s,scope,error);return std::make_tuple(s,scope,error);},[this,id](const auto& r){
    if(!std::get<2>(r).isEmpty()){fail(std::get<2>(r));return;}backupId=id;backupScope=std::get<1>(r);backupAvailableScope=backupScope;backupSnapshot=std::get<0>(r);backupRevision=QString::number(edit.revision);
    EditorSession preview=edit;preview.cancelEdit();preview.base.userWritable=true;preview.base.systemWritable=true;preview.base.hostsWritable=true;
    if(!preview.restore(backupSnapshot,backupScope)){fail(preview.error);return;}diff=difference(edit.effective(),preview.effective());
    backupHash=digest(QJsonDocument(backupSnapshot.json()).toJson(QJsonDocument::Compact)+backupAvailableScope.toUtf8());emit changed();
});}
void EditorBridge::setBackupScope(const QString& selectedScope){
    if(!finish()||backupId.isEmpty()||backupAvailableScope!="all"||(selectedScope!="user"&&selectedScope!="system"&&selectedScope!="all"))return;
    backupScope=selectedScope;EditorSession preview=edit;preview.cancelEdit();preview.base.userWritable=true;preview.base.systemWritable=true;
    if(!preview.restore(backupSnapshot,selectedScope)){fail(preview.error);return;}diff=difference(edit.effective(),preview.effective());backupRevision=QString::number(edit.revision);emit changed();
}
void EditorBridge::restoreBackup(){
    if(!finish()||backupId.isEmpty())return;if(backupRevision!=QString::number(edit.revision)){previewBackup(backupId);notice=trText("工作副本已变，请核对新预览后再次恢复","Working copy changed; review the updated preview before restoring");return;}
    showDialog("restore",trText("恢复到工作副本","Restore into working copy"),trText("先保留目标与当前工作副本的恢复点；不会写入系统。","Recovery points will be created for the target and current edits. The target will not be written."));
}
void EditorBridge::exportBackup(){if(busy||backupId.isEmpty())return;const auto n=native;const auto id=backupId;job([n,id]{QString e;auto path=n->exportBackup(id,e);return std::make_pair(path,e);},[this](const auto& r){if(!r.second.isEmpty())fail(r.second);else notice=trText("已导出：","Exported: ")+r.first;});}
void EditorBridge::deleteBackup(){if(busy||backupId.isEmpty())return;showDialog("deleteBackup",trText("删除这个备份？","Delete this backup?"),backupId);}
void EditorBridge::respond(const QString& action){
    if(busy)return;const auto type=dialog;
    if(action=="cancel"){dialog.clear();reviewToken.clear();closeAfterWrite=false;emit changed();return;}
    if(type=="review"&&action=="confirm"){saveConfirmed();return;}
    if(type=="conflict"){dialog.clear();if(action=="overwrite")requestSave(true);else if(action=="reload")requestReload();return;}
    if(type=="reload"&&action=="confirm"){dialog.clear();QString e;native->removeDraft(e);load();return;}
    if(type=="normalize"&&action=="confirm"){dialog.clear();edit.delimiter(edit.rows.current,pendingDelimiter,true);rebuild();return;}
    if(type=="draft"){
        dialog.clear();if(action=="restore"){const auto current=edit.base;if(!edit.restoreDraft(draftObject,native->identity()))fail(edit.error);else{
            conflicted=edit.baselineUnknown||(edit.kind=="hosts"?current.hosts!=edit.base.hosts:current.fingerprint("user")!=edit.base.fingerprint("user")||current.fingerprint("system")!=edit.base.fingerprint("system"));
            if(edit.buffer.field=="raw")page="raw";if(edit.buffer.field=="value"&&edit.buffer.part.isEmpty())mode="raw";
            if(edit.kind=="env")if(const auto* v=edit.env(edit.buffer.row);v&&(v->scope!="system"||args.showSystem)){if(scope!=v->scope)emit scopeChanging();scope=v->scope;filter.clear();scrollPosition=0;}
            rebuild();QTimer::singleShot(0,this,[this]{emit focusField(edit.buffer.row,edit.buffer.field,edit.buffer.part);});}}
        else if(action=="discard"){QString e;if(!native->removeDraft(e))fail(e);}return;
    }
    if(type=="close"){
        dialog.clear();if(action=="save"){closeAfterWrite=true;requestSave();}
        else if(action=="draft")persistDraft(true);
        else if(action=="discard")showDialog("discardClose",trText("确认丢弃所有未保存修改？","Discard all unsaved edits?"),trText("工作副本和对应草稿将被丢弃。","The working copy and its draft will be discarded."));return;
    }
    if(type=="discardClose"&&action=="confirm"){QString e;if(!native->removeDraft(e)){fail(e);return;}closingAllowed=true;dialog.clear();emit closeReady();return;}
    if(type=="deleteBackup"&&action=="confirm"){dialog.clear();const auto n=native;const auto id=backupId;job([n,id]{QString e;n->removeBackup(id,e);return e;},[this](const QString& e){if(!e.isEmpty())fail(e);else{backupId.clear();diff.clear();refreshBackups();}});return;}
    if(type=="restore"&&action=="confirm"){
        dialog.clear();if(backupRevision!=QString::number(edit.revision)){fail("E_STALE_RESTORE");return;}
        const auto n=native;const auto backend=store;const auto id=backupId;const auto hash=backupHash;const auto sc=backupScope;const auto draft=edit.draft(native->identity());
        job([n,backend,id,hash,sc,draft]{QString e,scope,backupId;TargetSnapshot s;
            if(!n->readBackup(id,s,scope,e))return e;
            if(hash!=digest(QJsonDocument(s.json()).toJson(QJsonDocument::Compact)+scope.toUtf8()))return QString("E_STALE_RESTORE");
            const auto actual=backend->read();if(!actual.error.isEmpty())return actual.error;
            const QStringList scopes=sc=="all"?QStringList{"user","system"}:QStringList{sc};
            if(!backend->backup(actual,scopes,backupId,e)||!n->saveRecoveryDraft(draft,e))return e;
            return QString{};
        },[this](const QString& e){if(!e.isEmpty()){fail(e);return;}if(!edit.restore(backupSnapshot,backupScope))fail(edit.error);else{notice=trText("已恢复到工作副本，尚未写入系统","Restored into working copy; target unchanged");page=edit.kind;}rebuild();});
    }
}
void EditorBridge::persistDraft(bool close){
    if(busy)return;const auto n=native;const auto data=edit.draft(n->identity());
    job([n,data]{QString e;n->saveDraft(data,e);return e;},[this,close](const QString& e){
        if(!e.isEmpty()){fail(e);if(shutdownRequested)qWarning()<<"E_DRAFT_SHUTDOWN";return;}
        if(close||shutdownRequested){closingAllowed=true;emit closeReady();}
        else if(closeAfterWrite){closeAfterWrite=false;requestClose(false);}
    });
}
bool EditorBridge::requestClose(bool shutdown){
    shutdownRequested|=shutdown;
    if(closingAllowed){emit closeAccepted();return true;}
    if(busy||edit.writing){closeWhenIdle=true;return false;}
    if(!edit.dirty()){closingAllowed=true;emit closeAccepted();return true;}
    if(shutdown){persistDraft(true);return false;}
    showDialog("close",trText("有未保存的修改","Unsaved changes"),trText("保存到系统、保留草稿或继续编辑。","Save to the target, keep a draft, or continue editing."));return false;
}
void EditorBridge::setActive(bool active){watcher.setInterval(active?2000:10000);}
void EditorBridge::appearance(envh::i18n::Lang lang,bool dark){args.setLang(lang);args.dark=dark;notice.clear();emit columnsChanged();rebuild();}
void EditorBridge::applyFocus(const OpenArgs& a){
    appearance(a.lang,a.dark);
    if(native->identity()!=NativeBackend(a).identity()){fail(trText("目标路径已变；请先保存或关闭当前编辑窗口，再打开新目标。","Target changed. Save or close this window before opening the new target."));return;}
    if(!finish())return;
    if(edit.kind=="env"&&!a.select.empty())for(const auto& v:edit.work.env)if(v.key()==qs(a.select).toCaseFolded()&&(v.scope!="system"||args.showSystem)){setScope(v.scope);filter.clear();break;}
    if(!a.filter.empty())filter=qs(a.filter);
    if(!a.select.empty()){const auto key=qs(a.select);for(const auto& id:visibleIds()){
        if(edit.kind=="env"){const auto* v=edit.env(id);if(v->key()==key.toCaseFolded()){edit.rows.select(id,visibleIds());break;}}
        else if(QString::number(edit.work.hosts.rows[size_t(edit.hostIndex(id))].origin+1)==key){edit.rows.select(id,visibleIds());break;}
    }}rebuild();
}

namespace {
struct WindowState {QPointer<QQuickWindow> window;QPointer<EditorBridge> bridge;};
class EditorController : public QObject {
public:
    std::unique_ptr<QQmlEngine> engine;
    std::map<std::string,WindowState> windows;
    std::mutex mutex;
    std::condition_variable closed;
    std::map<std::string,bool> opened;
    bool shuttingDown=false,releaseQueued=false;
    void mark(const std::string& kind,bool value){std::lock_guard<std::mutex> lock(mutex);opened[kind]=value;closed.notify_all();}
    void releaseWhenUnused(){
        if(releaseQueued)return;
        releaseQueued=true;
        // QObject::destroyed is emitted before its children are deleted. Wait
        // until the whole window tree (and any local QQmlComponent) is gone.
        QTimer::singleShot(0,this,[this]{
            releaseQueued=false;
            if(!windows.empty())return; // An editor may have reopened meanwhile.
            if(engine){engine->collectGarbage();engine.reset();logMemory("engine-released");}
            if(shuttingDown)QCoreApplication::quit();
        });
    }
    bool create(const OpenArgs& args,std::string& error){
        if(shuttingDown){error="E_SHUTDOWN";return false;}
        if(auto it=windows.find(args.kind);it!=windows.end()&&it->second.window){auto* win=it->second.window.data();it->second.bridge->applyFocus(args);win->showNormal();win->raise();win->requestActivate();return true;}
        if(!engine)engine=std::make_unique<QQmlEngine>();
        auto* bridge=new EditorBridge(args);
        QQmlComponent component(engine.get(),QUrl("qrc:/qt/qml/EnvHosts/Editor/qt/qml/Main.qml"));
        if(component.isError()){error=component.errorString().toStdString();delete bridge;releaseWhenUnused();return false;}
        auto* object=component.createWithInitialProperties({{"bridge",QVariant::fromValue(bridge)}});
        auto* window=qobject_cast<QQuickWindow*>(object);
        if(!window){error=component.errorString().toStdString();delete object;delete bridge;releaseWhenUnused();return false;}
        bridge->setParent(window);windows[args.kind]={window,bridge};mark(args.kind,true);
        connect(bridge,&EditorBridge::closeReady,window,[window]{window->close();});
        connect(bridge,&EditorBridge::closeAccepted,window,[window]{
            window->setPersistentGraphics(false);
            window->setPersistentSceneGraph(false);
            window->releaseResources();
            window->deleteLater();
        });
        connect(window,&QObject::destroyed,this,[this,kind=args.kind]{windows.erase(kind);mark(kind,false);releaseWhenUnused();});
        window->show();window->raise();window->requestActivate();bridge->load();return true;
    }
    void shutdown(){shuttingDown=true;if(windows.empty()){releaseWhenUnused();return;}for(auto& item:windows)if(item.second.bridge)item.second.bridge->requestClose(true);}
};
EditorController* controller=nullptr;
}
void initialize(){
    if(controller)return;
    // This editor only uses 2D controls. Avoid creating a hardware graphics
    // device and driver caches for a short-lived editing window.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    QQuickStyle::setStyle("Basic");
    controller=new EditorController;
    controller->setParent(qApp);
}
bool openEditor(const OpenArgs& a,std::string& error){
    if(!controller){error="E_QML_INIT";return false;}bool ok=false;
    auto fn=[&]{ok=controller->create(a,error);};if(QThread::currentThread()==qApp->thread())fn();else QMetaObject::invokeMethod(qApp,fn,Qt::BlockingQueuedConnection);return ok;
}
bool openEditorDeferred(const OpenArgs& a){
    if(!controller)return false;
    return QMetaObject::invokeMethod(controller,[a]{
        QElapsedTimer elapsed;elapsed.start();
        std::string error;
        if(!controller->create(a,error)&&!error.empty()){
            qWarning()<<"E_QML_OPEN:"<<QString::fromStdString(error);
            return;
        }
        qInfo()<<"qml-open"<<QString::fromStdString(a.kind)<<"ms="<<elapsed.elapsed();
        logMemory("window-opened");
    },Qt::QueuedConnection);
}
bool isEditorOpen(const std::string& kind){if(!controller)return false;std::lock_guard<std::mutex> lock(controller->mutex);return controller->opened[kind];}
void onExternalChange(const std::string& kind){if(controller)QMetaObject::invokeMethod(controller,[kind]{auto it=controller->windows.find(kind);if(it!=controller->windows.end()&&it->second.bridge)it->second.bridge->poll();},Qt::QueuedConnection);}
void onAppearanceChanged(envh::i18n::Lang lang,bool dark){if(controller)QMetaObject::invokeMethod(controller,[lang,dark]{for(auto& item:controller->windows)if(item.second.bridge)item.second.bridge->appearance(lang,dark);},Qt::QueuedConnection);}
void closeAllEditors(unsigned timeoutMs){
    if(!controller)return;QMetaObject::invokeMethod(controller,[]{controller->shutdown();},Qt::QueuedConnection);
    if(timeoutMs&&QThread::currentThread()!=qApp->thread()){
        std::unique_lock<std::mutex> lock(controller->mutex);
        const bool done=controller->closed.wait_for(lock,std::chrono::milliseconds(timeoutMs),[]{for(const auto& item:controller->opened)if(item.second)return false;return true;});
        if(!done)qWarning()<<"E_SHUTDOWN_DRAFT_TIMEOUT";
    }
}
void shutdownWhenReady(){closeAllEditors(0);}
} // namespace editorwin
