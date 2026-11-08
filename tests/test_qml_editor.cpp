#include "qt/qt_editor.h"
#include <QtTest>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QSignalSpy>
#include <QStyleHints>

using namespace editorwin;
namespace {
EnvValue variable(QString name,QString value,QString scope="user",QString type="REG_SZ") {
    EnvValue v;v.id=freshId();v.name=name;v.scope=scope;v.value=value;v.type=type;v.encode();return v;
}
TargetSnapshot fixture() {
    TargetSnapshot s;s.userWritable=true;s.systemWritable=true;s.hostsWritable=true;
    s.env={variable("A_PATH","A;B;C;D;E"),variable("B_HOME","C:\\Users\\Test"),variable("C_MODE","dev"),variable("Z_SYSTEM","readonly","system")};
    s.hosts="\xef\xbb\xbf# Test header\r\n127.0.0.1 alpha.test alias.test # first\r\n::1 beta.test\n# 10.0.0.1 gamma.test\r\nopaque unknown line";
    return s;
}
class FakeStore final: public StoreBackend {
public:
    TargetSnapshot data=fixture();QStringList calls;int failWrite=0,writes=0;bool failBackup=false,failReadback=false,failBroadcast=false;
    TargetSnapshot read() override {calls<<"read";auto s=data;if(failReadback&&writes)s.error="E_INJECT_READBACK";return s;}
    bool backup(const TargetSnapshot&,const QStringList&,QString& id,QString& error) override {
        calls<<"backup";if(failBackup){error="injected";return false;}id="recovery";return true;
    }
    bool writeEnv(const Change& c,QString& error) override {
        calls<<"write:"+c.name;if(++writes==failWrite){error="injected";return false;}
        data.env.erase(std::remove_if(data.env.begin(),data.env.end(),[&](const EnvValue& v){return v.key()==envKey(c.scope,c.name);}),data.env.end());
        if(c.hasAfter)data.env.append(c.after);return true;
    }
    bool writeHosts(const QByteArray& bytes,QString& error) override {calls<<"hosts";if(++writes==failWrite){data.hosts="partial";error="injected";return false;}data.hosts=bytes;return true;}
    bool broadcast() override {calls<<"broadcast";return !failBroadcast;}
};
WriteRequest plan(EditorSession& s) {
    WriteRequest r;r.kind=s.kind;r.token="test-review";r.revision=QString::number(s.revision);r.changes=s.changes();r.hosts=s.effective().hosts;
    if(s.kind=="hosts")r.fingerprints["hosts"]=s.base.fingerprint("hosts");else for(const auto& c:r.changes)r.fingerprints[c.scope]=s.base.fingerprint(c.scope);return r;
}
QQuickItem* item(QQuickItem* root,const QString& name) {
    if(root->objectName()==name)return root;for(auto* child:root->childItems())if(auto* found=item(child,name))return found;return nullptr;
}
void click(QQuickWindow* window,QQuickItem* target,QPointF point,Qt::KeyboardModifiers modifiers={}) {
    QTest::mouseClick(window,Qt::LeftButton,modifiers,target->mapToScene(point).toPoint());
}
void typeText(QQuickWindow* window,const QString& text) {
    for(const auto c:text){QKeyEvent press(QEvent::KeyPress,0,Qt::NoModifier,QString(c));QCoreApplication::sendEvent(window,&press);
        QKeyEvent release(QEvent::KeyRelease,0,Qt::NoModifier,QString(c));QCoreApplication::sendEvent(window,&release);}
}
}

class QmlEditorTests:public QObject {
    Q_OBJECT
private slots:
    void selectionRanges() {
        Selection s;const QStringList v{"a","b","c","d","e"};
        s.select("b",v);s.select("d",v,true);QCOMPARE(s.ordered(v),QStringList({"b","d"}));
        s.select("a",v,false,true);QCOMPARE(s.ordered(v),QStringList({"a","b","c","d"}));
        s.prune({"b","e"});QCOMPARE(s.ordered(v),QStringList({"b"}));QCOMPARE(s.current,QString("b"));
        s.all({"b","e"});QCOMPARE(s.ids.size(),2);s.select("b",v,true);QVERIFY(!s.ids.contains("b"));
    }
    void batchDeleteAndUndo() {
        auto snap=fixture();snap.systemWritable=false;EditorSession s;s.load(snap);auto ids=s.visible();
        QCOMPARE(s.deleteRows(ids,true),3);QCOMPARE(s.undoCount(),1);QCOMPARE(s.changes().size(),3);
        QVERIFY(s.history(false));QVERIFY(!s.dirty());QCOMPARE(s.visible(),ids);
        QVERIFY(s.history(true));QCOMPARE(s.changes().size(),3);
        QCOMPARE(s.deleteRows(ids,false),3);QVERIFY(!s.dirty());QCOMPARE(s.undoCount(),2);
    }
    void partsBatchAndMetadata() {
        EditorSession s;s.load(fixture());const auto row=s.rows.current;const auto p=s.ensureParts(row);QVERIFY(p);const auto ids=s.partIds(row);QCOMPARE(ids.size(),5);
        QVERIFY(s.deleteParts(row,{ids[1],ids[3]},true));QCOMPARE(s.env(row)->value,QString("A;C;E"));QCOMPARE(s.undoCount(),1);
        QCOMPARE(s.partIds(row),ids);QVERIFY(s.work.parts[row].items[1].deleted);
        QVERIFY(s.history(false));QCOMPARE(s.env(row)->value,QString("A;B;C;D;E"));
        const auto added=s.addPart(row);QVERIFY(!added.isEmpty());QVERIFY(s.deleteParts(row,{added},true));QVERIFY(!s.partIds(row).contains(added));
        QVERIFY(s.history(false));QVERIFY(s.partIds(row).contains(added));
        QVERIFY(s.delimiter(row,"|"));QCOMPARE(s.partIds(row).size(),1);QVERIFY(s.history(false));QCOMPARE(s.work.parts[row].delimiter,QString(";"));
    }
    void moveStableBlocks() {
        EditorSession s;s.load(fixture());const auto row=s.rows.current;s.ensureParts(row);const auto ids=s.partIds(row);QCOMPARE(ids.size(),5);
        QVERIFY(s.moveParts(row,{ids[0],ids[1]},-1));QCOMPARE(s.env(row)->value,QString("A;B;C;D;E"));QCOMPARE(s.undoCount(),0);
        QVERIFY(s.moveParts(row,{ids[1],ids[2]},-1));QCOMPARE(s.env(row)->value,QString("B;C;A;D;E"));
        s.history(false);s.moveParts(row,{ids[1],ids[3]},1);QCOMPARE(s.env(row)->value,QString("A;C;B;E;D"));
        s.history(false);s.moveParts(row,{ids[3],ids[4]},1);QCOMPARE(s.env(row)->value,QString("A;B;C;D;E"));QCOMPARE(s.undoCount(),0);
    }
    void buffersAndIme() {
        EditorSession s;s.load(fixture());const auto id=s.rows.current;const auto token=s.beginEdit(id,"name");
        s.updateEdit(token,"invalid=name");QVERIFY(s.dirty());QVERIFY(!s.finishEdit());QCOMPARE(s.env(id)->name,QString("A_PATH"));QVERIFY(s.buffer.pending());
        s.updateEdit(token,"VALID",true);QVERIFY(!s.finishEdit());s.updateEdit(token,"VALID",false);QVERIFY(s.finishEdit());QCOMPARE(s.undoCount(),1);
        const auto next=s.beginEdit(id,"value");s.updateEdit(next,"temporary");s.cancelEdit();QCOMPARE(s.env(id)->value,QString("A;B;C;D;E"));
        QVERIFY(s.history(false));QVERIFY(!s.dirty());
    }
    void multiSzPreservesSemicolons() {
        auto snap=fixture();snap.env={variable("MULTI","A;B\nC","user","REG_MULTI_SZ")};EditorSession s;s.load(snap);const auto id=s.rows.current;
        QVERIFY(!s.ensureParts(id));QVERIFY(s.editField(id,"name","MULTI2"));QCOMPARE(s.env(id)->bytes,snap.env[0].bytes);
        QVERIFY(!s.editField(id,"value","A\n\nB"));QVERIFY(s.editField(id,"value","A;B\nD"));
        QCOMPARE(s.env(id)->bytes,utf16Bytes(QString("A;B")+QChar(0)+"D"+QChar(0)+QChar(0)));
    }
    void hostsBatchRawAndUndo() {
        EditorSession s("hosts");s.load(fixture());const auto original=s.effective().hosts;const auto ids=s.visible();
        QCOMPARE(s.enableRows(ids,false),2);QCOMPARE(s.undoCount(),1);QVERIFY(s.effective().hosts.contains("# ::1"));
        QVERIFY(s.history(false));QCOMPARE(s.effective().hosts,original);
        QCOMPARE(s.deleteRows({ids[1],ids[2]},true),2);QCOMPARE(s.undoCount(),1);s.history(false);QCOMPARE(s.effective().hosts,original);
        auto raw=s.rawText();raw.replace("alpha.test","new.test");QVERIFY(s.editField({},"raw",raw));
        QVERIFY(s.effective().hosts.startsWith("\xef\xbb\xbf# Test header\r\n"));QVERIFY(s.effective().hosts.contains("::1 beta.test\n"));QVERIFY(!s.effective().hosts.endsWith('\n'));
        QVERIFY(s.validation().isEmpty());s.history(false);QCOMPARE(s.effective().hosts,original);
        QVERIFY(s.editField({},"raw",""));QCOMPARE(s.rawText(),QString());s.history(false);QCOMPARE(s.effective().hosts,original);
    }
    void draftsKeepPendingAndIds() {
        EditorSession s;s.load(fixture());auto id=s.rows.current;s.ensureParts(id);auto ids=s.partIds(id);s.deleteParts(id,{ids[2]},true);
        auto token=s.beginEdit(id,"name");s.updateEdit(token,"not=valid");auto draft=s.draft("test-target");
        EditorSession restored;restored.load(fixture());QVERIFY(restored.restoreDraft(draft,"test-target"));QCOMPARE(restored.partIds(id),ids);
        QVERIFY(restored.work.parts[id].items[2].deleted);QVERIFY(restored.buffer.pending());QCOMPARE(restored.buffer.text,QString("not=valid"));
        QVERIFY(!restored.restoreDraft(draft,"different-target"));
    }
    void hostsDraftRoundtrip() {
        EditorSession s("hosts");s.load(fixture());s.editField({},"raw",s.rawText()+"\n10.0.0.5 extra.test\n");auto expected=s.effective().hosts;
        EditorSession r("hosts");r.load(fixture());QVERIFY(r.restoreDraft(s.draft("hosts-fixture"),"hosts-fixture"));QCOMPARE(r.effective().hosts,expected);QCOMPARE(r.work.hostIds,s.work.hostIds);
    }
    void legacyAndCorruptDrafts() {
        EditorSession s;s.load(fixture());QJsonObject row{{"name","A_PATH"},{"scope","user"},{"type","REG_SZ"},{"value","legacy"},{"inBase",true}};
        QVERIFY(s.restoreDraft(QJsonObject{{"rows",QJsonArray{row}}},"isolated"));QVERIFY(s.baselineUnknown);QCOMPARE(s.changes().size(),1);QCOMPARE(s.work.env.size(),4);
        auto bad=s.draft("isolated");auto state=bad["state"].toObject();state["hostIds"]=QJsonArray{"duplicate","duplicate"};bad["state"]=state;
        const auto before=s.effective().fingerprint("user");QVERIFY(!s.restoreDraft(bad,"isolated"));QCOMPARE(s.effective().fingerprint("user"),before);
        auto legacy=row;legacy["type"]="REG_MULTI_SZ";QVERIFY(!s.restoreDraft(QJsonObject{{"rows",QJsonArray{legacy}}},"isolated"));QCOMPARE(s.error,QString("E_DRAFT_MULTI_LEGACY"));
    }
    void backupBeforeWriteAndReadback() {
        FakeStore store;EditorSession s;s.load(store.data);s.editField(s.rows.current,"value","changed");auto r=WriteService::execute(store,plan(s));
        QCOMPARE(r.state,QString("Succeeded"));QCOMPARE(store.calls,QStringList({"read","backup","write:A_PATH","read","broadcast"}));QCOMPARE(r.backupId,QString("recovery"));
    }
    void backupFailureBlocksEveryWrite() {
        FakeStore store;store.failBackup=true;EditorSession s;s.load(store.data);s.deleteRows(s.visible(),true);
        const auto r=WriteService::execute(store,plan(s));QCOMPARE(r.state,QString("Rejected"));QCOMPARE(store.writes,0);QVERIFY(r.error.startsWith("E_BACKUP"));
    }
    void conflictsAndPermissions() {
        FakeStore store;EditorSession s;s.load(store.data);s.editField(s.rows.current,"value","changed");auto request=plan(s);
        store.data.env.append(variable("OUTSIDE","external"));QCOMPARE(WriteService::execute(store,request).state,QString("Conflict"));QCOMPARE(store.writes,0);
        request.fingerprints["user"]=store.data.fingerprint("user");store.data.userWritable=false;
        QCOMPARE(WriteService::execute(store,request).error,QString("E_DENIED"));QCOMPARE(store.writes,0);
        store.data.userWritable=true;QCOMPARE(WriteService::execute(store,request).state,QString("Succeeded"));
        QVERIFY(std::any_of(store.data.env.begin(),store.data.env.end(),[](const EnvValue& v){return v.name=="OUTSIDE"&&v.value=="external";}));
    }
    void rejectInvalidPlanBeforeBackup() {
        FakeStore store;EditorSession s;s.load(store.data);s.deleteRows(s.visible(),true);auto p=plan(s);p.fingerprints.remove("system");
        QCOMPARE(WriteService::execute(store,p).state,QString("Rejected"));QVERIFY(!store.calls.contains("backup"));QCOMPARE(store.writes,0);
    }
    void partialReadbackBroadcastFailures() {
        FakeStore store;EditorSession s;s.load(store.data);s.deleteRows(s.visible(),true);store.failWrite=2;
        auto r=WriteService::execute(store,plan(s));QCOMPARE(r.state,QString("PartialFailure"));QCOMPARE(r.completed.size(),1);QCOMPARE(r.failed.size(),1);QCOMPARE(r.notExecuted.size(),2);QVERIFY(!store.calls.contains("broadcast"));
        FakeStore read;read.failReadback=true;s.load(read.data);s.editField(s.rows.current,"value","changed");QCOMPARE(WriteService::execute(read,plan(s)).state,QString("ReadbackFailure"));
        FakeStore broadcast;broadcast.failBroadcast=true;s.load(broadcast.data);s.editField(s.rows.current,"value","changed");QCOMPARE(WriteService::execute(broadcast,plan(s)).state,QString("StoredButBroadcastFailed"));
    }
    void hostsPartialWriteHasRecovery() {
        FakeStore store;store.failWrite=1;EditorSession s("hosts");s.load(store.data);s.editField({},"raw","127.0.0.1 only.test\n");
        auto r=WriteService::execute(store,plan(s));QCOMPARE(r.state,QString("PartialFailure"));QCOMPARE(r.backupId,QString("recovery"));QCOMPARE(r.actual.hosts,QByteArray("partial"));
    }
    void backendDraftsAndSharedBackups() {
        QTemporaryDir dir;OpenArgs a;a.kind="env";a.dataDir=dir.path().toStdString();NativeBackend backend(a);QString error;
        EditorSession session;session.load(fixture());session.editField(session.rows.current,"value","QML draft");
        const auto draft=session.draft(backend.identity());QDir().mkpath(dir.path()+"/drafts");
        QFile legacy(dir.path()+"/drafts/env.json");QVERIFY(legacy.open(QIODevice::WriteOnly));legacy.write(QJsonDocument(draft).toJson());legacy.close();
        QCOMPARE(backend.readDraft(error),draft);QVERIFY2(error.isEmpty(),qPrintable(error));
        QVERIFY(QFileInfo::exists(dir.path()+"/drafts/qml/env.json"));QVERIFY(!QFileInfo::exists(legacy.fileName()));
        QDir().mkpath(dir.path()+"/drafts/gdi");QFile gdi(dir.path()+"/drafts/gdi/env.json");QVERIFY(gdi.open(QIODevice::WriteOnly));gdi.write("{\"rows\":[]}");gdi.close();
        QVERIFY(backend.removeDraft(error));QVERIFY(QFileInfo::exists(gdi.fileName()));QVERIFY(backend.readDraft(error).isEmpty());
        QDir().mkpath(dir.path()+"/backups/env");QFile backup(dir.path()+"/backups/env/legacy.json");QVERIFY(backup.open(QIODevice::WriteOnly));
        backup.write("{\"schemaVersion\":1,\"scopes\":[\"system\"],\"vars\":[{\"name\":\"PATH\",\"scope\":\"system\",\"type\":\"REG_EXPAND_SZ\",\"value\":\"%ROOT%;C:\\\\Tools\"}]}");backup.close();
        TargetSnapshot restored;QString scope;QVERIFY2(backend.readBackup("legacy",restored,scope,error),qPrintable(error));
        QCOMPARE(scope,QString("system"));QCOMPARE(restored.env.size(),1);QCOMPARE(restored.env[0].type,QString("REG_EXPAND_SZ"));
        QVERIFY(backup.open(QIODevice::WriteOnly|QIODevice::Truncate));backup.write("{\"vars\":[{\"name\":\"MULTI\",\"type\":\"REG_MULTI_SZ\",\"value\":\"A;B\"}]}");backup.close();
        QVERIFY(!backend.readBackup("legacy",restored,scope,error));QCOMPARE(error,QString("E_BACKUP_MULTI_LEGACY"));
    }
    void isolatedNativeStorage() {
        QTemporaryDir dir;QVERIFY(dir.isValid());const auto key=QString("Software\\EnvHostsQmlTests\\")+freshId();
        HKEY user=nullptr,system=nullptr;const auto userKey=(key+"\\User").toStdWString(),systemKey=(key+"\\System").toStdWString();
        QCOMPARE(RegCreateKeyExW(HKEY_CURRENT_USER,userKey.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&user,nullptr),LONG(ERROR_SUCCESS));
        QCOMPARE(RegCreateKeyExW(HKEY_CURRENT_USER,systemKey.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&system,nullptr),LONG(ERROR_SUCCESS));RegCloseKey(user);RegCloseKey(system);
        struct Cleanup {std::wstring key;~Cleanup(){RegDeleteTreeW(HKEY_CURRENT_USER,key.c_str());}} cleanup{key.toStdWString()};
        OpenArgs a;a.kind="env";a.userSubkey=userKey;a.sysSubkey=L"HKCU\\"+systemKey;a.dataDir=dir.path().toStdString();NativeBackend native(a);QString error;
        Change c;c.scope="user";c.name="MULTI";c.hasAfter=true;c.after=variable("MULTI","A;B\n中文","user","REG_MULTI_SZ");QVERIFY(native.writeEnv(c,error));
        c.scope="system";c.after.scope="system";QVERIFY(native.writeEnv(c,error));auto read=native.read();QVERIFY2(read.error.isEmpty(),qPrintable(read.error));QCOMPARE(read.env.size(),2);QCOMPARE(read.env[0].value,QString("A;B\n中文"));QCOMPARE(read.env[0].bytes,c.after.bytes);
        QString backup;QVERIFY(native.backup(read,{"user","system"},backup,error));TargetSnapshot restored;QString scope;QVERIFY(native.readBackup(backup,restored,scope,error));QCOMPARE(scope,QString("all"));QCOMPARE(restored.fingerprint("user"),read.fingerprint("user"));
        a.kind="hosts";a.hostsPath=(dir.path()+"/hosts").toStdWString();QFile hosts(QString::fromStdWString(a.hostsPath));QVERIFY(hosts.open(QIODevice::WriteOnly));hosts.write(fixture().hosts);hosts.close();NativeBackend h(a);
        EditorSession session("hosts");session.load(h.read());session.editField({},"raw","127.0.0.1 isolated.test\n");QCOMPARE(WriteService::execute(h,plan(session)).state,QString("Succeeded"));QCOMPARE(h.read().hosts,QByteArray("127.0.0.1 isolated.test\r\n").prepend("\xef\xbb\xbf"));
        a.dataDir=(dir.path()+"/blocked").toStdString();QFile block(QString::fromStdString(a.dataDir));QVERIFY(block.open(QIODevice::WriteOnly));block.write("file");block.close();NativeBackend blocked(a);QVERIFY(!blocked.backup(read,{"hosts"},backup,error));
    }
    void realQmlMultiSelectionAndScreenshots() {
        QTemporaryDir dir;auto store=std::make_shared<FakeStore>();OpenArgs a;a.kind="env";a.dataDir=dir.path().toStdString();EditorBridge bridge(a,nullptr,store);
        QQmlEngine engine;QStringList warnings;connect(&engine,&QQmlEngine::warnings,this,[&](const QList<QQmlError>& list){for(const auto& w:list)warnings<<w.toString();});
        QQmlComponent component(&engine,QUrl::fromLocalFile(QStringLiteral(ENVHOSTS_SOURCE_DIR "/qt/qml/Main.qml")));
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));std::unique_ptr<QObject> object(component.createWithInitialProperties({{"bridge",QVariant::fromValue(&bridge)}}));
        QVERIFY2(object,qPrintable(component.errorString()));auto* window=qobject_cast<QQuickWindow*>(object.get());QVERIFY(window);window->show();bridge.load();QTRY_VERIFY(bridge.state()["ready"].toBool());QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* list=item(window->contentItem(),"rowsList");QVERIFY(list);QTRY_VERIFY(list->height()>100);
        click(window,list,{300,14});click(window,list,{300,42},Qt::ControlModifier);QCOMPARE(bridge.state()["selected"].toInt(),2);
        click(window,list,{300,70},Qt::ShiftModifier);QCOMPARE(bridge.state()["selected"].toInt(),2);
        list->forceActiveFocus();QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);QCOMPARE(bridge.state()["selected"].toInt(),3);
        QTest::keyClick(window,Qt::Key_Delete);QCOMPARE(bridge.session().changes().size(),3);QCOMPARE(bridge.session().undoCount(),1);
        bridge.history();QVERIFY(!bridge.session().dirty());bridge.setFilter("HOME");QCOMPARE(bridge.state()["selected"].toInt(),1);bridge.setFilter("");
        bridge.select(bridge.session().visible()[0]);auto* parts=item(window->contentItem(),"partsList");QVERIFY(parts);
        QTest::qWait(100);QCOMPARE(bridge.parts()->rowCount(),5);QVERIFY(parts->height()>=56);
        click(window,parts,{180,14});click(window,parts,{180,42},Qt::ControlModifier);QCOMPARE(bridge.state()["partSelected"].toInt(),2);
        parts->forceActiveFocus();QTest::keyClick(window,Qt::Key_Delete);QCOMPARE(bridge.session().env(bridge.session().rows.current)->value,QString("C;D;E"));bridge.history();
        auto* name=item(window->contentItem(),"nameField");QVERIFY(name);name->forceActiveFocus();QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);
        const auto selection=bridge.state()["selected"].toInt();typeText(window,"RENAMED");QVERIFY(bridge.session().buffer.pending());QCOMPARE(bridge.state()["selected"].toInt(),selection);
        QTest::keyClick(window,Qt::Key_Return);QCOMPARE(bridge.session().env(bridge.session().rows.current)->name,QString("RENAMED"));
        QTest::keyClick(window,Qt::Key_Z,Qt::ControlModifier);QCOMPARE(bridge.session().env(bridge.session().rows.current)->name,QString("A_PATH"));
        QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);typeText(window,"invalid=name");QTest::keyClick(window,Qt::Key_Return);QVERIFY(bridge.session().buffer.pending());
        QTest::keyClick(window,Qt::Key_Escape);QVERIFY(!bridge.session().buffer.pending());QCOMPARE(name->property("text").toString(),QString("A_PATH"));
        bridge.addRow();QTest::qWait(70);name=item(window->contentItem(),"nameField");QVERIFY(name);name->forceActiveFocus();QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);typeText(window,"NEW_NAME");QTest::keyClick(window,Qt::Key_Return);
        QCOMPARE(bridge.session().env(bridge.session().rows.current)->name,QString("NEW_NAME"));bridge.history();bridge.history();bridge.select(bridge.session().visible()[0]);
        QDir().mkpath(QStringLiteral(ENVHOSTS_SOURCE_DIR "/build-qml/evidence"));
        QTest::qWait(150);QVERIFY(window->grabWindow().save(QStringLiteral(ENVHOSTS_SOURCE_DIR "/build-qml/evidence/repair-environment-light.png")));
        bridge.appearance(envh::i18n::Lang::EnUS,true);QTest::qWait(150);QVERIFY(window->grabWindow().save(QStringLiteral(ENVHOSTS_SOURCE_DIR "/build-qml/evidence/repair-environment-dark.png")));
        auto* delimiter=item(window->contentItem(),"partsDelimiterControls");auto* status=item(window->contentItem(),"windowStatus");QVERIFY(delimiter&&status);
        QVERIFY(parts->mapToScene({0,parts->height()}).y()<=delimiter->mapToScene({0,0}).y());QVERIFY(delimiter->mapToScene({0,delimiter->height()}).y()<=status->mapToScene({0,0}).y());
        window->resize(900,640);QTest::qWait(80);QVERIFY(parts->height()>=56);QVERIFY(delimiter->mapToScene({0,delimiter->height()}).y()<=status->mapToScene({0,0}).y());
        bridge.deleteSelected();bridge.requestSave();QTRY_COMPARE(bridge.state()["dialog"].toString(),QString("review"));QTest::qWait(80);bridge.respond("cancel");
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
        bridge.history();bridge.requestClose();
    }
    void backupPreviewRestoreIsUndoable() {
        QTemporaryDir dir;auto store=std::make_shared<FakeStore>();OpenArgs a;a.kind="env";a.dataDir=dir.path().toStdString();NativeBackend files(a);QString id,error;
        auto saved=fixture();saved.env[0].value="restored";saved.env[0].encode();saved.env[3].value="system-restore";saved.env[3].encode();QVERIFY(files.backup(saved,{"user","system"},id,error));
        EditorBridge bridge(a,nullptr,store);bridge.load();QTRY_VERIFY(bridge.state()["ready"].toBool());bridge.previewBackup(id);QTRY_COMPARE(bridge.state()["backupId"].toString(),id);
        QCOMPARE(bridge.state()["diff"].toList().size(),2);bridge.setBackupScope("user");QCOMPARE(bridge.state()["diff"].toList().size(),1);QVERIFY(!bridge.session().dirty());bridge.restoreBackup();QCOMPARE(bridge.state()["dialog"].toString(),QString("restore"));bridge.respond("confirm");
        QTRY_VERIFY(bridge.session().dirty());QCOMPARE(store->writes,0);QCOMPARE(bridge.session().undoCount(),1);QCOMPARE(bridge.session().env(bridge.session().rows.current)->value,QString("restored"));
        QVERIFY(!QDir(dir.path()+"/drafts/qml/recovery").entryList({"*.json"},QDir::Files).isEmpty());bridge.history();QVERIFY(!bridge.session().dirty());
    }
    void repairHostsProjectionAndScopes() {
        EditorSession hosts("hosts");const auto snap=fixture();hosts.load(snap);QCOMPARE(hosts.visible().size(),3);QCOMPARE(hosts.effective().hosts,snap.hosts);
        for(const auto& id:hosts.visible())QVERIFY(!hosts.work.hosts.rows[size_t(hosts.hostIndex(id))].isSection);
        QCOMPARE(hosts.visible("header").size(),0);QCOMPARE(hosts.visible("gamma").size(),1);
        hosts.enableRows(hosts.visible(),false);QVERIFY(hosts.effective().hosts.contains("# Test header\r\n"));QVERIFY(hosts.effective().hosts.endsWith("opaque unknown line"));hosts.history(false);QCOMPARE(hosts.effective().hosts,snap.hosts);
        QTemporaryDir dir;auto store=std::make_shared<FakeStore>();OpenArgs a;a.kind="env";a.dataDir=dir.path().toStdString();EditorBridge b(a,nullptr,store);b.load();QTRY_VERIFY(b.state()["ready"].toBool());
        QCOMPARE(b.state()["scope"].toString(),QString("user"));QCOMPARE(b.rows()->rowCount(),3);b.selectAll();const auto selection=b.session().rows.ids;
        b.deleteSelected();QCOMPARE(b.session().changes().size(),3);QVERIFY(b.setScope("system"));QCOMPARE(b.rows()->rowCount(),1);QCOMPARE(b.state()["selected"].toInt(),1);b.rememberScroll(32);
        const auto sys=b.session().rows.current;b.deleteSelected();QCOMPARE(b.state()["userChanges"].toInt(),3);QCOMPARE(b.state()["systemChanges"].toInt(),1);
        QVERIFY(b.setScope("user"));QCOMPARE(b.session().rows.ids,selection);b.history();QCOMPARE(b.session().changes().size(),3);b.history();QVERIFY(!b.session().dirty());
        QVERIFY(b.setScope("system"));QCOMPARE(b.session().rows.current,sys);QCOMPARE(b.state()["scroll"].toDouble(),32.0);b.addRow("system");QCOMPARE(b.state()["scope"].toString(),QString("system"));QCOMPARE(b.session().env(b.session().rows.current)->scope,QString("system"));b.history();
        QVERIFY(b.setScope("user"));const auto id=b.session().rows.current;auto token=b.beginEdit(id,"name");b.updateEdit(token,"bad=name");QVERIFY(!b.setScope("system"));QVERIFY(!b.sortColumn("value"));QVERIFY(!b.resizeColumn("name",250));QCOMPARE(b.session().rows.current,id);b.cancelEdit();
        token=b.beginEdit(id,"name");b.updateEdit(token,"composing",true);QVERIFY(!b.setScope("system"));b.cancelEdit();QVERIFY(b.setScope("system"));
        OpenArgs focus=a;focus.select=store->data.env[0].key().toStdString();b.applyFocus(focus);QCOMPARE(b.state()["scope"].toString(),QString("user"));
        focus.select=store->data.env[3].key().toStdString();b.applyFocus(focus);QCOMPARE(b.state()["scope"].toString(),QString("system"));QCOMPARE(b.session().rows.current,sys);
        a.showSystem=false;EditorBridge hidden(a,nullptr,store);hidden.load();QTRY_VERIFY(hidden.state()["ready"].toBool());QVERIFY(!hidden.setScope("system"));const auto n=hidden.session().work.env.size();hidden.addRow("system");QCOMPARE(hidden.session().work.env.size(),n);hidden.applyFocus(focus);QCOMPARE(hidden.state()["scope"].toString(),QString("user"));
        store->data.systemWritable=false;b.poll();QTRY_VERIFY(!b.state()["systemWritable"].toBool());b.selectAll();b.deleteSelected();QVERIFY(!b.session().dirty());
    }
    void repairColumnStateAndLargeProjection() {
        QTemporaryDir dir;auto store=std::make_shared<FakeStore>();OpenArgs a;a.kind="env";a.dataDir=dir.path().toStdString();EditorBridge b(a,nullptr,store);b.load();QTRY_VERIFY(b.state()["ready"].toBool());
        QSignalSpy reset(b.rows(),&QAbstractItemModel::modelReset);QSignalSpy changes(b.rows(),&QAbstractItemModel::dataChanged);
        QVERIFY(b.resizeColumn("name",220));b.saveColumns();QCOMPARE(b.columns()[0].toMap()["width"].toDouble(),220.0);QCOMPARE(changes.count(),0);QVERIFY(!b.session().dirty());
        QFont font("Segoe UI");font.setPixelSize(13);b.autoSizeColumn("name",font);QVERIFY(b.columns()[0].toMap()["width"].toDouble()<220);QVERIFY(b.resizeColumn("name",-10));QCOMPARE(b.columns()[0].toMap()["width"].toDouble(),100.0);
        b.resizeColumn("name",231);b.saveColumns();EditorBridge reopened(a,nullptr,store);QCOMPARE(reopened.columns()[0].toMap()["width"].toDouble(),231.0);QVERIFY(reopened.columns()[0].toMap()["explicit"].toBool());QVERIFY(!reopened.columns()[1].toMap()["explicit"].toBool());
        b.selectAll();const auto selected=b.session().rows.ids;QVERIFY(b.sortColumn("name"));QVERIFY(b.sortColumn("name"));QCOMPARE(b.rows()->get(0)["name"].toString(),QString("C_MODE"));QCOMPARE(b.session().rows.ids,selected);QCOMPARE(reset.count(),0);
        auto many=fixture();many.env.clear();for(int i=0;i<10000;++i)many.env.append(variable(QString("V%1").arg(i,5,10,QChar('0')),QString("C:/Test/%1").arg(i)));
        b.session().load(many);QElapsedTimer timer;timer.start();b.setFilter("");const auto loadMs=timer.elapsed();QCOMPARE(b.rows()->rowCount(),10000);
        QPersistentModelIndex firstIndex(b.rows()->index(0));const auto firstId=firstIndex.data(EditorRows::RecordRole).toMap()["id"];
        timer.restart();b.sortColumn("name");const auto sortMs=timer.elapsed();QCOMPARE(firstIndex.data(EditorRows::RecordRole).toMap()["id"],firstId);QCOMPARE(firstIndex.row(),9999);
        timer.restart();for(int i=0;i<200;++i)b.resizeColumn("value",180+i);const auto resizeMs=timer.elapsed();
        timer.restart();b.autoSizeColumn("value",font);const auto fitMs=timer.elapsed();timer.restart();b.setFilter("V099");const auto filterMs=timer.elapsed();QCOMPARE(b.rows()->rowCount(),100);QCOMPARE(reset.count(),0);
        qInfo()<<"REPAIR_10000 projection_ms"<<loadMs<<"sort_ms"<<sortMs<<"resize_200_ms"<<resizeMs<<"fit_ms"<<fitMs<<"filter_ms"<<filterMs;
        QVERIFY(sortMs<2000);QVERIFY(resizeMs<1000);QVERIFY(fitMs<1000);QVERIFY(filterMs<2000);
    }
    void repairRealCellsColumnsAndStateColors() {
        QTemporaryDir dir;auto store=std::make_shared<FakeStore>();OpenArgs a;a.kind="env";a.dataDir=dir.path().toStdString();EditorBridge b(a,nullptr,store);QQmlEngine engine;QStringList warnings;
        connect(&engine,&QQmlEngine::warnings,this,[&](const QList<QQmlError>& list){for(const auto& w:list)warnings<<w.toString();});
        QQmlComponent component(&engine,QUrl::fromLocalFile(QStringLiteral(ENVHOSTS_SOURCE_DIR "/qt/qml/Main.qml")));std::unique_ptr<QObject> object(component.createWithInitialProperties({{"bridge",QVariant::fromValue(&b)}}));QVERIFY2(object,qPrintable(component.errorString()));auto* w=qobject_cast<QQuickWindow*>(object.get());w->show();b.load();QTRY_VERIFY(b.state()["ready"].toBool());QVERIFY(QTest::qWaitForWindowExposed(w));QTest::qWait(100);
        const auto first=b.rows()->get(0)["id"].toString(),second=b.rows()->get(1)["id"].toString();b.select(first);b.select(second,Qt::ControlModifier);const auto selection=b.session().rows.ids;
        auto doubleCell=[&](const QString& field){auto* cell=item(w->contentItem(),"cell_"+first+"_"+field);if(!cell)return false;QTest::mouseDClick(w,Qt::LeftButton,{},cell->mapToScene(QPointF(20,cell->height()/2)).toPoint());return true;};
        QVERIFY(doubleCell("name"));QTRY_COMPARE(w->activeFocusItem()->objectName(),QString("nameField"));QCOMPARE(b.session().rows.ids,selection);
        QVERIFY(doubleCell("value"));QTRY_COMPARE(b.state()["mode"].toString(),QString("raw"));QTRY_COMPARE(w->activeFocusItem()->objectName(),QString("valueField"));
        QVERIFY(doubleCell("type"));auto* type=item(w->contentItem(),"typeField");QVERIFY(type);auto* popup=type->property("popup").value<QObject*>();QVERIFY(popup);QTRY_VERIFY(popup->property("visible").toBool());QTest::keyClick(w,Qt::Key_Escape);QVERIFY(!b.session().dirty());
        auto* grip=item(w->contentItem(),"columnGrip_name");QVERIFY(grip);const auto point=grip->mapToScene(QPointF(4,14)).toPoint();QTest::mousePress(w,Qt::LeftButton,{},point);QTest::mouseMove(w,point+QPoint(45,0),30);QTest::mouseRelease(w,Qt::LeftButton,{},point+QPoint(45,0));QVERIFY(b.columns()[0].toMap()["width"].toDouble()>200);QVERIFY(!b.session().dirty());
        grip->forceActiveFocus();QTest::keyClick(w,Qt::Key_Return);QVERIFY(b.columns()[0].toMap()["width"].toDouble()<200);
        auto* valueCell=item(w->contentItem(),"cell_"+first+"_value");QVERIFY(valueCell);const auto valueWidth=valueCell->width();QVERIFY(valueWidth>320);
        auto* valueGrip=item(w->contentItem(),"columnGrip_value");QVERIFY(valueGrip);const auto vp=valueGrip->mapToScene(QPointF(4,14)).toPoint();QTest::mousePress(w,Qt::LeftButton,{},vp);QTest::mouseMove(w,vp-QPoint(60,0),30);QTest::mouseRelease(w,Qt::LeftButton,{},vp-QPoint(60,0));QCOMPARE(valueCell->width(),valueWidth-60);
        QVERIFY(doubleCell("state"));QCOMPARE(b.state()["dialog"].toString(),QString("info"));QVERIFY(!b.session().dirty());b.respond("cancel");
        auto* caption=item(w->contentItem(),"columnCaption_name");QVERIFY(caption);QTest::mouseDClick(w,Qt::LeftButton,{},caption->mapToScene({20,14}).toPoint());QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval()+30);QCOMPARE(b.state()["sortKey"].toString(),QString("name"));QVERIFY(!b.state()["sortDescending"].toBool());QCOMPARE(b.session().rows.ids,selection);
        QVERIFY(b.sortColumn("name"));QTest::qWait(50);QCOMPARE(b.rows()->get(0)["name"].toString(),QString("C_MODE"));QVERIFY(doubleCell("name"));QCOMPARE(b.session().rows.current,first);QCOMPARE(b.session().rows.ids,selection);
        auto* filter=item(w->contentItem(),"filterField");auto* add=item(w->contentItem(),"addRowButton");auto* rowItem=item(w->contentItem(),"row_"+first);QVERIFY(filter&&add&&rowItem);QVERIFY(filter->height()<=30);QVERIFY(add->height()<=30);QVERIFY(rowItem->height()<=30);QVERIFY(type->height()<=30);
        auto* system=item(w->contentItem(),"systemScopeButton");click(w,system,{30,12});QTRY_COMPARE(b.state()["scope"].toString(),QString("system"));QCOMPARE(b.rows()->rowCount(),1);auto* user=item(w->contentItem(),"userScopeButton");click(w,user,{30,12});QCOMPARE(b.session().rows.ids,selection);
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
        a.kind="hosts";EditorBridge hosts(a,nullptr,store);QQmlComponent hc(&engine,QUrl::fromLocalFile(QStringLiteral(ENVHOSTS_SOURCE_DIR "/qt/qml/Main.qml")));std::unique_ptr<QObject> ho(hc.createWithInitialProperties({{"bridge",QVariant::fromValue(&hosts)}}));QVERIFY(ho);auto* hw=qobject_cast<QQuickWindow*>(ho.get());hw->show();hosts.load();QTRY_VERIFY(hosts.state()["ready"].toBool());QTest::qWait(100);QCOMPARE(hosts.rows()->rowCount(),3);
        const auto hid=hosts.rows()->get(0)["id"].toString();hosts.select(hid);hosts.deleteSelected();QTest::qWait(80);auto* deleted=item(hw->contentItem(),"row_"+hid);QVERIFY(deleted);QCOMPARE(deleted->property("color").value<QColor>(),QColor("#ffedf0"));QVERIFY(deleted->property("selected").toBool());QVERIFY(deleted->property("deleted").toBool());
        QVERIFY(hw->grabWindow().save(QStringLiteral(ENVHOSTS_SOURCE_DIR "/build-qml/evidence/repair-hosts-light.png")));hosts.appearance(envh::i18n::Lang::EnUS,true);QTest::qWait(100);QCOMPARE(deleted->property("color").value<QColor>(),QColor("#482a35"));QVERIFY(hw->grabWindow().save(QStringLiteral(ENVHOSTS_SOURCE_DIR "/build-qml/evidence/repair-hosts-dark.png")));
        hosts.history();hosts.selectAll();hosts.toggleSelected();QCOMPARE(hosts.session().undoCount(),1);for(const auto& id:hosts.session().visible())QVERIFY(!hosts.session().work.hosts.rows[size_t(hosts.session().hostIndex(id))].commented);hosts.history();QCOMPARE(hosts.session().effective().hosts,fixture().hosts);
        auto* check=item(hw->contentItem(),"rowSelection_"+hid);QVERIFY(check);const auto count=hosts.state()["selected"].toInt();check->forceActiveFocus();QTest::keyClick(hw,Qt::Key_Space);QCOMPARE(hosts.state()["selected"].toInt(),count-1);QVERIFY(!hosts.session().dirty());
        auto* hcell=item(hw->contentItem(),"cell_"+hosts.rows()->get(1)["id"].toString()+"_ip");QVERIFY(hcell);click(hw,hcell,{20,14});QCOMPARE(hosts.state()["selected"].toInt(),count-1);QTest::keyClick(hw,Qt::Key_Space);QCOMPARE(hosts.session().undoCount(),1);hosts.history();QCOMPARE(hosts.session().effective().hosts,fixture().hosts);
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
    }
    void repairRealScopeScrollAndSelection() {
        QTemporaryDir dir;auto store=std::make_shared<FakeStore>();store->data.env.clear();for(const auto& scope:{QString("user"),QString("system")})for(int i=0;i<100;++i)store->data.env.append(variable(QString("V%1").arg(i,3,10,QChar('0')),"A;B;C",scope));
        OpenArgs a;a.kind="env";a.dataDir=dir.path().toStdString();EditorBridge b(a,nullptr,store);QQmlEngine engine;QQmlComponent c(&engine,QUrl::fromLocalFile(QStringLiteral(ENVHOSTS_SOURCE_DIR "/qt/qml/Main.qml")));std::unique_ptr<QObject> object(c.createWithInitialProperties({{"bridge",QVariant::fromValue(&b)}}));QVERIFY2(object,qPrintable(c.errorString()));auto* w=qobject_cast<QQuickWindow*>(object.get());w->show();b.load();QTRY_VERIFY(b.state()["ready"].toBool());QVERIFY(QTest::qWaitForWindowExposed(w));QTest::qWait(80);
        auto* list=item(w->contentItem(),"rowsList");QVERIFY(list);b.select(b.rows()->get(25)["id"].toString());const auto userId=b.session().rows.current;QVERIFY(list->setProperty("contentY",700.0));QTest::qWait(30);
        click(w,item(w->contentItem(),"systemScopeButton"),{30,12});QTRY_COMPARE(b.state()["scope"].toString(),QString("system"));QTest::qWait(50);b.select(b.rows()->get(40)["id"].toString());const auto systemId=b.session().rows.current;QVERIFY(list->setProperty("contentY",1120.0));QTest::qWait(30);
        click(w,item(w->contentItem(),"userScopeButton"),{30,12});QTRY_COMPARE(b.session().rows.current,userId);QTRY_COMPARE(list->property("contentY").toDouble(),700.0);
        click(w,item(w->contentItem(),"systemScopeButton"),{30,12});QTRY_COMPARE(b.session().rows.current,systemId);QTRY_COMPARE(list->property("contentY").toDouble(),1120.0);QVERIFY(!b.session().dirty());
    }
    void saveKeepsEnvSelectionAndScopes() {
        QTemporaryDir dir;auto store=std::make_shared<FakeStore>();
        store->data.env.append(variable("B_HOME","system-home","system"));
        store->data.env.append(variable("C_MODE","system-mode","system"));
        OpenArgs a;a.kind="env";a.dataDir=dir.path().toStdString();EditorBridge b(a,nullptr,store);
        b.load();QTRY_VERIFY(b.state()["ready"].toBool());
        const auto home=b.rows()->get(1)["id"].toString(),mode=b.rows()->get(2)["id"].toString();
        b.select(mode);b.select(home,Qt::ShiftModifier);const auto userSelection=b.session().rows;
        auto token=b.beginEdit(home,"name");b.updateEdit(token,"Z_HOME");QVERIFY(b.finishEdit());
        b.sortColumn("name");b.sortColumn("name");b.rememberScroll(84);
        QVERIFY(b.setScope("system"));const auto sys=b.rows()->get(1)["id"].toString();
        b.select(b.rows()->get(0)["id"].toString());b.select(sys,Qt::ControlModifier);const auto systemSelection=b.session().rows;
        token=b.beginEdit(sys,"value");b.updateEdit(token,"updated-system");QVERIFY(b.finishEdit());
        store->failBroadcast=true;QSignalSpy reset(b.rows(),&QAbstractItemModel::modelReset);
        b.requestSave();QTRY_COMPARE(b.state()["dialog"].toString(),QString("review"));b.respond("confirm");
        QTRY_VERIFY(!b.state()["busy"].toBool());QVERIFY(!b.session().dirty());
        QCOMPARE(b.state()["scope"].toString(),QString("system"));QCOMPARE(b.session().rows.current,sys);
        QCOMPARE(b.session().rows.ids,systemSelection.ids);QCOMPARE(b.session().rows.anchor,systemSelection.anchor);
        QVERIFY(b.setScope("user"));QCOMPARE(b.session().rows.current,home);QCOMPARE(b.session().env(home)->name,QString("Z_HOME"));
        QCOMPARE(b.session().rows.ids,userSelection.ids);QCOMPARE(b.session().rows.anchor,userSelection.anchor);
        QCOMPARE(b.state()["sortKey"].toString(),QString("name"));QVERIFY(b.state()["sortDescending"].toBool());
        QCOMPARE(b.state()["scroll"].toDouble(),84.0);QCOMPARE(reset.count(),0);
        b.select(home);b.addRow();const auto added=b.session().rows.current;
        QStringList order;for(int i=0;i<b.rows()->rowCount();++i)order.append(b.rows()->get(i)["id"].toString());
        store->failBroadcast=false;b.requestSave();QTRY_COMPARE(b.state()["dialog"].toString(),QString("review"));b.respond("confirm");
        QTRY_VERIFY(!b.state()["busy"].toBool());QVERIFY(!b.session().dirty());QCOMPARE(b.session().rows.current,added);
        QCOMPARE(b.session().rows.ids,QSet<QString>{added});QVERIFY(!b.session().env(added)->isNew);
        for(int i=0;i<order.size();++i)QCOMPARE(b.rows()->get(i)["id"].toString(),order[i]);
        QVERIFY(b.setScope("system"));QCOMPARE(b.session().rows.current,sys);QCOMPARE(b.session().rows.ids,systemSelection.ids);
    }
    void saveKeepsPartsAndReloadUsesActualValues() {
        QTemporaryDir dir;auto store=std::make_shared<FakeStore>();store->data.env[1]=variable("B_HOME","A|B|B|D");
        OpenArgs a;a.kind="env";a.dataDir=dir.path().toStdString();EditorBridge b(a,nullptr,store);
        b.load();QTRY_VERIFY(b.state()["ready"].toBool());const auto row=b.rows()->get(1)["id"].toString();
        b.select(row);b.setDelimiter("|");const auto ids=b.session().partIds(row);QCOMPARE(ids.size(),4);
        b.select(ids[1],0,true);b.duplicateParts();const auto copy=b.state()["partCurrent"].toString();
        auto token=b.beginEdit(row,"value",copy);b.updateEdit(token,"COPY");QVERIFY(b.finishEdit());
        b.select(ids[0],0,true);b.deleteSelected(true,true);
        b.select(copy,0,true);b.select(ids[2],Qt::ControlModifier,true);b.moveParts(-1);
        const auto selected=b.session().partSelections.value(row);auto expected=b.session().partIds(row);expected.removeAll(ids[0]);
        const auto value=b.session().env(row)->value;QSignalSpy removed(b.parts(),&QAbstractItemModel::rowsRemoved);
        QSignalSpy reset(b.parts(),&QAbstractItemModel::modelReset);
        b.requestSave();QTRY_COMPARE(b.state()["dialog"].toString(),QString("review"));b.respond("confirm");
        QTRY_VERIFY(!b.state()["busy"].toBool());QVERIFY(!b.session().dirty());QCOMPARE(b.session().rows.current,row);
        QCOMPARE(b.state()["delimiter"].toString(),QString("|"));QCOMPARE(b.session().partIds(row),expected);
        QCOMPARE(b.session().partSelections[row].ids,selected.ids);QCOMPARE(b.state()["partCurrent"].toString(),selected.current);
        QCOMPARE(b.session().partSelections[row].anchor,selected.anchor);QCOMPARE(b.session().env(row)->value,value);
        QCOMPARE(reset.count(),0);QCOMPARE(removed.count(),1);QCOMPARE(b.session().undoCount(),0);
        for(const auto& part:b.session().work.parts[row].items){QVERIFY(!part.isNew);QVERIFY(!part.deleted);}
        // An external reload keeps this variable and delimiter, but adopts fresh data.
        for(auto& v:store->data.env)if(v.key()==envKey("user","B_HOME")){v.value="NEW|VALUES";v.encode();}
        b.poll();QTRY_COMPARE(b.session().env(row)->value,QString("NEW|VALUES"));QCOMPARE(b.session().rows.current,row);
        QCOMPARE(b.state()["delimiter"].toString(),QString("|"));QCOMPARE(b.parts()->rowCount(),2);
        QCOMPARE(b.parts()->get(0)["value"].toString(),QString("NEW"));QCOMPARE(b.state()["partSelected"].toInt(),0);
        for(const auto& id:b.session().partIds(row))QVERIFY(!expected.contains(id));
    }
    void saveDeletedCurrentChoosesVisibleNeighbor() {
        QTemporaryDir dir;auto store=std::make_shared<FakeStore>();
        OpenArgs a;a.kind="env";a.dataDir=dir.path().toStdString();EditorBridge b(a,nullptr,store);
        b.load();QTRY_VERIFY(b.state()["ready"].toBool());b.sortColumn("name");b.sortColumn("name");
        const auto next=b.rows()->get(2)["id"].toString();b.select(b.rows()->get(1)["id"].toString());b.deleteSelected();
        b.requestSave();QTRY_COMPARE(b.state()["dialog"].toString(),QString("review"));b.respond("confirm");
        QTRY_VERIFY(!b.state()["busy"].toBool());QVERIFY(!b.session().dirty());QCOMPARE(b.session().rows.current,next);
        QCOMPARE(b.session().rows.ids,QSet<QString>{next});QCOMPARE(b.rows()->get(1)["id"].toString(),next);
        // Deleting the last visible row falls backward, never across scopes.
        const auto previous=b.rows()->get(0)["id"].toString();b.deleteSelected();
        b.requestSave();QTRY_COMPARE(b.state()["dialog"].toString(),QString("review"));b.respond("confirm");
        QTRY_VERIFY(!b.state()["busy"].toBool());QVERIFY(!b.session().dirty());QCOMPARE(b.session().rows.current,previous);
        QCOMPARE(b.session().rows.ids,QSet<QString>{previous});b.deleteSelected();
        b.requestSave();QTRY_COMPARE(b.state()["dialog"].toString(),QString("review"));b.respond("confirm");
        QTRY_VERIFY(!b.state()["busy"].toBool());QVERIFY(!b.session().dirty());QCOMPARE(b.rows()->rowCount(),0);
        QVERIFY(b.session().rows.current.isEmpty());QVERIFY(b.session().rows.ids.isEmpty());QVERIFY(b.state()["detail"].toMap().isEmpty());
        QVERIFY(b.setScope("system"));QCOMPARE(b.rows()->rowCount(),1);
    }
    void saveRealQmlKeepsSelectionAndScroll() {
        QTemporaryDir dir;auto store=std::make_shared<FakeStore>();store->data.env.clear();
        for(int i=0;i<100;++i)store->data.env.append(variable(QString("V%1").arg(i,3,10,QChar('0')),"A;B;C"));
        OpenArgs a;a.kind="env";a.dataDir=dir.path().toStdString();EditorBridge b(a,nullptr,store);QQmlEngine engine;
        QQmlComponent c(&engine,QUrl::fromLocalFile(QStringLiteral(ENVHOSTS_SOURCE_DIR "/qt/qml/Main.qml")));
        std::unique_ptr<QObject> object(c.createWithInitialProperties({{"bridge",QVariant::fromValue(&b)}}));QVERIFY2(object,qPrintable(c.errorString()));
        auto* w=qobject_cast<QQuickWindow*>(object.get());w->show();b.load();QTRY_VERIFY(b.state()["ready"].toBool());QVERIFY(QTest::qWaitForWindowExposed(w));
        auto* list=item(w->contentItem(),"rowsList");QVERIFY(list);const auto row=b.rows()->get(40)["id"].toString();
        b.select(row);QVERIFY(list->setProperty("contentY",1008.0));QTest::qWait(50);
        const auto before=list->property("contentY").toDouble();auto token=b.beginEdit(row,"value");b.updateEdit(token,"UPDATED");QVERIFY(b.finishEdit());
        click(w,item(w->contentItem(),"saveButton"),{30,12});QTRY_COMPARE(b.state()["dialog"].toString(),QString("review"));
        auto* confirm=item(w->contentItem(),"dialogConfirm");QVERIFY(confirm);QTRY_VERIFY(confirm->isVisible());click(w,confirm,{30,12});
        QTRY_VERIFY(!b.state()["busy"].toBool());QVERIFY(!b.session().dirty());QCOMPARE(b.state()["currentId"].toString(),row);
        QTRY_COMPARE(list->property("currentIndex").toInt(),40);QTRY_COMPARE(list->property("contentY").toDouble(),before);
        QCOMPARE(b.state()["detail"].toMap()["value"].toString(),QString("UPDATED"));QCOMPARE(b.state()["selected"].toInt(),1);
    }
    void insertionOrderAndUndo() {
        EditorSession s;s.load(fixture());const auto row=s.rows.current;const auto original=s.visible({},"user");s.ensureParts(row);const auto parts=s.partIds(row);
        s.partSelections[row].select(parts[2],parts);s.partSelections[row].select(parts[0],parts,true);const auto added=s.addPart(row);
        QCOMPARE(s.partIds(row),QStringList({parts[0],parts[1],parts[2],added,parts[3],parts[4]}));QCOMPARE(s.env(row)->value,QString("A;B;C;;D;E"));QCOMPARE(s.undoCount(),1);
        QVERIFY(s.history(false));QCOMPARE(s.partIds(row),parts);QVERIFY(s.history(true));QCOMPARE(s.partIds(row)[3],added);QVERIFY(s.work.parts[row].items[3].isNew);
        s.partSelections[row].select(added,s.partIds(row));const auto next=s.addPart(row);QCOMPARE(s.partIds(row)[4],next);s.partSelections[row].ids.clear();const auto appended=s.addPart(row);QCOMPARE(s.partIds(row).last(),appended);
        s.rows.select(original[1],original);const auto env=s.addRow();QCOMPARE(s.visible({},"user"),QStringList({original[0],original[1],env,original[2]}));
        s.rows.ids.clear();const auto tail=s.addRow();QCOMPARE(s.visible({},"user").last(),tail);QCOMPARE(s.visible({},"system").size(),1);
        EditorSession h("hosts");auto snap=fixture();snap.hosts="\xef\xbb\xbf# Header\r\n127.0.0.1 first.test\r\n\n# Group\r\n::1 last.test\n# tail";h.load(snap);const auto ids=h.visible();h.rows.select(ids[0],ids);
        const auto inserted=h.addRow();QCOMPARE(h.visible(),QStringList({ids[0],inserted,ids[1]}));const auto serialized=h.effective().hosts;
        QVERIFY(serialized.startsWith("\xef\xbb\xbf# Header\r\n127.0.0.1 first.test\r\n127.0.0.1 new.test\r\n\n# Group\r\n"));QVERIFY(serialized.endsWith("::1 last.test\n# tail"));QCOMPARE(h.undoCount(),1);
        QVERIFY(h.history(false));QCOMPARE(h.effective().hosts,snap.hosts);QVERIFY(h.history(true));QCOMPARE(h.visible()[1],inserted);QCOMPARE(h.effective().hosts,serialized);
        h.rows.select(inserted,h.visible());const auto second=h.addRow();QCOMPARE(h.visible()[2],second);QCOMPARE(h.effective().hosts.count("new.test"),2);
        h.deleteRows({ids[0]},true);QVERIFY(h.effective().hosts.startsWith("\xef\xbb\xbf# Header\r\n127.0.0.1 new.test\r\n"));
        EditorSession restored("hosts");restored.load(snap);QVERIFY(restored.restoreDraft(h.draft("insertion"),"insertion"));QCOMPARE(restored.effective().hosts,h.effective().hosts);QCOMPARE(restored.work.hostIds,h.work.hostIds);
        FakeStore backend;backend.data=snap;QCOMPARE(WriteService::execute(backend,plan(h)).state,QString("Succeeded"));QCOMPARE(backend.data.hosts,h.effective().hosts);
        h.rows.ids.clear();h.addRow();QVERIFY(h.effective().hosts.endsWith("# tail\n127.0.0.1 new.test"));
        QTemporaryDir dir;auto store=std::make_shared<FakeStore>();OpenArgs a;a.kind="env";a.dataDir=dir.path().toStdString();EditorBridge b(a,nullptr,store);b.load();QTRY_VERIFY(b.state()["ready"].toBool());
        b.sortColumn("name");b.sortColumn("name");const auto before0=b.rows()->get(0)["id"].toString(),before1=b.rows()->get(1)["id"].toString(),before2=b.rows()->get(2)["id"].toString();
        b.select(before1);b.select(before0,Qt::ControlModifier);b.addRow();const auto fresh=b.session().rows.current;QCOMPARE(b.rows()->get(0)["id"].toString(),before0);QCOMPARE(b.rows()->get(1)["id"].toString(),before1);QCOMPARE(b.rows()->get(2)["id"].toString(),fresh);QCOMPARE(b.rows()->get(3)["id"].toString(),before2);QVERIFY(b.state()["sortKey"].toString().isEmpty());
        b.setScope("system");b.setScope("user");QCOMPARE(b.rows()->get(2)["id"].toString(),fresh);b.history();QCOMPARE(b.rows()->rowCount(),3);b.history(true);QCOMPARE(b.rows()->get(2)["id"].toString(),fresh);
        b.setFilter("C_MODE");b.select(before0);b.addRow();QCOMPARE(b.rows()->get(1)["id"].toString(),b.session().rows.current);QVERIFY(b.state()["filter"].toString().isEmpty());
    }
    void insertionRealButtonsAndGreenState() {
        QTemporaryDir dir;auto store=std::make_shared<FakeStore>();QQmlEngine engine;QStringList warnings;connect(&engine,&QQmlEngine::warnings,this,[&](const QList<QQmlError>& list){for(const auto& e:list)warnings<<e.toString();});
        for(const auto& kind:{QString("env"),QString("hosts")}){
            OpenArgs a;a.kind=kind.toStdString();a.dataDir=(dir.path()+"/"+kind).toStdString();EditorBridge b(a,nullptr,store);QQmlComponent c(&engine,QUrl::fromLocalFile(QStringLiteral(ENVHOSTS_SOURCE_DIR "/qt/qml/Main.qml")));std::unique_ptr<QObject> object(c.createWithInitialProperties({{"bridge",QVariant::fromValue(&b)}}));QVERIFY2(object,qPrintable(c.errorString()));auto* w=qobject_cast<QQuickWindow*>(object.get());w->show();b.load();QTRY_VERIFY(b.state()["ready"].toBool());QVERIFY(QTest::qWaitForWindowExposed(w));QTest::qWait(80);
            const auto first=b.rows()->get(0)["id"].toString(),second=b.rows()->get(1)["id"].toString();b.select(first);
            if(kind=="env"){
                const auto old=b.session().partIds(first);b.select(old[0],0,true);auto* button=item(w->contentItem(),"addPartButton");QVERIFY(button);click(w,button,{20,12});
                const auto fresh=b.state()["partCurrent"].toString();QCOMPARE(b.parts()->get(1)["id"].toString(),fresh);QCOMPARE(b.parts()->get(2)["id"].toString(),old[1]);QTRY_VERIFY(item(w->contentItem(),"part_"+fresh));auto* part=item(w->contentItem(),"part_"+fresh);
                QTRY_COMPARE(w->activeFocusItem()->objectName(),QString("valueField"));QCOMPARE(part->property("color").value<QColor>(),QColor("#e7f5ed"));QVERIFY(part->property("selected").toBool());
                typeText(w,"INSERTED");QTest::keyClick(w,Qt::Key_Return);QCOMPARE(b.session().env(first)->value,QString("A;INSERTED;B;C;D;E"));QTest::qWait(80);QVERIFY(w->grabWindow().save(QStringLiteral(ENVHOSTS_SOURCE_DIR "/build-qml/evidence/insertion-parts-light.png")));
                b.appearance(envh::i18n::Lang::EnUS,true);QTest::qWait(50);QCOMPARE(part->property("color").value<QColor>(),QColor("#203e35"));QVERIFY(w->grabWindow().save(QStringLiteral(ENVHOSTS_SOURCE_DIR "/build-qml/evidence/insertion-parts-dark.png")));b.history();b.history();QCOMPARE(b.parts()->rowCount(),5);b.appearance(envh::i18n::Lang::ZhCN,false);
            }
            auto* add=item(w->contentItem(),"addRowButton");QVERIFY(add);click(w,add,{20,12});const auto fresh=b.session().rows.current;QCOMPARE(b.rows()->get(1)["id"].toString(),fresh);QCOMPARE(b.rows()->get(2)["id"].toString(),second);QTRY_VERIFY(item(w->contentItem(),"row_"+fresh));auto* r=item(w->contentItem(),"row_"+fresh);QCOMPARE(r->property("color").value<QColor>(),QColor("#e7f5ed"));QVERIFY(r->property("selected").toBool());
            b.appearance(envh::i18n::Lang::EnUS,true);QTest::qWait(50);QCOMPARE(r->property("color").value<QColor>(),QColor("#203e35"));b.history();QCOMPARE(b.rows()->rowCount(),3);b.history(true);QCOMPARE(b.rows()->get(1)["id"].toString(),fresh);
        }
        QCOMPARE(store->writes,0);QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
    }
    void realQmlHostsPagesAndCloseReview() {
        QTemporaryDir dir;auto store=std::make_shared<FakeStore>();OpenArgs a;a.kind="hosts";a.dataDir=dir.path().toStdString();EditorBridge bridge(a,nullptr,store);QQmlEngine engine;
        QStringList warnings;connect(&engine,&QQmlEngine::warnings,this,[&](const QList<QQmlError>& list){for(const auto& w:list)warnings<<w.toString();});
        QQmlComponent component(&engine,QUrl::fromLocalFile(QStringLiteral(ENVHOSTS_SOURCE_DIR "/qt/qml/Main.qml")));
        std::unique_ptr<QObject> object(component.createWithInitialProperties({{"bridge",QVariant::fromValue(&bridge)}}));QVERIFY2(object,qPrintable(component.errorString()));auto* window=qobject_cast<QQuickWindow*>(object.get());window->show();bridge.load();QTRY_VERIFY(bridge.state()["ready"].toBool());
        QVERIFY(bridge.navigate("raw"));QTRY_VERIFY(item(window->contentItem(),"rawField"));QVERIFY(bridge.navigate("hosts"));
        bridge.selectAll();bridge.enableSelected(false);QCOMPARE(bridge.session().undoCount(),1);QVERIFY(!bridge.requestClose());bridge.respond("save");QTRY_COMPARE(bridge.state()["dialog"].toString(),QString("review"));
        QSignalSpy closed(&bridge,&EditorBridge::closeAccepted);bridge.respond("confirm");QTRY_VERIFY(!bridge.session().dirty());QTRY_COMPARE(closed.count(),1);
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
    }
};
int main(int argc,char** argv) {QGuiApplication app(argc,argv);QQuickStyle::setStyle("Basic");QmlEditorTests tests;return QTest::qExec(&tests,argc,argv);}
#include "test_qml_editor.moc"
