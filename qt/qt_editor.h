#pragma once
#include "editor_models.h"
#include "write_service.h"
#include <QObject>
#include <QTimer>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QFont>

namespace editorwin {
class EditorBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
    Q_PROPERTY(EditorRows* rows READ rows CONSTANT)
    Q_PROPERTY(EditorRows* parts READ parts CONSTANT)
    Q_PROPERTY(EditorRows* backups READ backups CONSTANT)
    Q_PROPERTY(QVariantList columns READ columns NOTIFY columnsChanged)
public:
    explicit EditorBridge(OpenArgs args,QObject* parent=nullptr,std::shared_ptr<StoreBackend> backend={});
    QVariantMap state() const;
    EditorRows* rows(){return &rowModel;}
    EditorRows* parts(){return &partModel;}
    EditorRows* backups(){return &backupModel;}
    EditorSession& session(){return edit;}
    QVariantList columns() const;
    Q_INVOKABLE bool resizeColumn(const QString& key,qreal width);
    Q_INVOKABLE void saveColumns();
    Q_INVOKABLE void autoSizeColumn(const QString& key,const QFont& font);
    Q_INVOKABLE bool sortColumn(const QString& key);
    Q_INVOKABLE void rememberScroll(qreal y);
    Q_INVOKABLE bool activateCell(const QString& id,const QString& field);
    Q_INVOKABLE bool selectCell(const QString& id,int modifiers=0,bool part=false);
    Q_INVOKABLE void toggleSelected();
    QString trText(const QString& zh,const QString& en) const{return args.zh?zh:en;}
    Q_INVOKABLE QString trText(bool chinese,const QString& zh,const QString& en) const{return chinese?zh:en;}
    Q_INVOKABLE void load();
    Q_INVOKABLE bool navigate(const QString& page);
    Q_INVOKABLE bool setFilter(const QString& text);
    Q_INVOKABLE bool setScope(const QString& scope);
    Q_INVOKABLE bool select(const QString& id,int modifiers=0,bool part=false);
    Q_INVOKABLE void selectAll(bool part=false);
    Q_INVOKABLE void moveFocus(int delta,int modifiers=0,bool part=false);
    Q_INVOKABLE void clearSelection(bool part=false);
    Q_INVOKABLE QString beginEdit(const QString& row,const QString& field,const QString& part={});
    Q_INVOKABLE QString editText(const QString& row,const QString& field,const QString& part={})const {
        const auto& b=edit.buffer;return !b.token.isEmpty()&&b.row==row&&b.field==field&&b.part==part?b.text:edit.field(row,field,part);
    }
    Q_INVOKABLE void updateEdit(const QString& token,const QString& text,bool composing=false);
    Q_INVOKABLE bool finishEdit();
    Q_INVOKABLE void cancelEdit();
    Q_INVOKABLE void addRow(const QString& scope="user");
    Q_INVOKABLE void deleteSelected(bool deleted=true,bool part=false);
    Q_INVOKABLE void enableSelected(bool enabled);
    Q_INVOKABLE void addPart();
    Q_INVOKABLE void duplicateParts();
    Q_INVOKABLE void moveParts(int direction);
    Q_INVOKABLE void setDelimiter(const QString& value);
    Q_INVOKABLE bool setMode(const QString& mode);
    Q_INVOKABLE void history(bool redo=false);
    Q_INVOKABLE void copySelected(bool part=false,bool expanded=false);
    Q_INVOKABLE void requestSave(bool overwrite=false);
    Q_INVOKABLE void respond(const QString& action);
    Q_INVOKABLE void requestReload();
    Q_INVOKABLE void poll();
    Q_INVOKABLE void refreshBackups();
    Q_INVOKABLE void previewBackup(const QString& id);
    Q_INVOKABLE void setBackupScope(const QString& scope);
    Q_INVOKABLE void restoreBackup();
    Q_INVOKABLE void exportBackup();
    Q_INVOKABLE void deleteBackup();
    Q_INVOKABLE bool requestClose(bool shutdown=false);
    Q_INVOKABLE void setActive(bool active);
    void applyFocus(const OpenArgs& a);
    void appearance(envh::i18n::Lang lang,bool dark);
signals:
    void changed();
    void columnsChanged();
    void scopeChanging();
    void focusField(const QString& row,const QString& field,const QString& part);
    void closeReady();
    void closeAccepted();
private:
    OpenArgs args;
    EditorSession edit;
    std::shared_ptr<StoreBackend> store;
    std::shared_ptr<NativeBackend> native;
    EditorRows rowModel,partModel,backupModel;
    QTimer watcher,draftTimer;
    QString filter,scope="user",page,mode="parts",error,notice;
    struct ViewState {QString filter,sort;bool descending=false;Selection selection;qreal scroll=0;QStringList manualOrder;};
    QHash<QString,ViewState> views;
    QString sortKey;bool sortDescending=false;
    QStringList manualOrder;
    QHash<QString,qreal> columnWidths;
    qreal scrollPosition=0;
    QString dialog,dialogTitle,dialogText,reviewToken,reviewRevision,backupId,backupScope,backupAvailableScope,backupRevision,backupHash;
    QVariantList diff;
    TargetSnapshot external,backupSnapshot;
    WriteRequest request;
    QJsonObject draftObject;
    bool ready=false,busy=false,polling=false,conflicted=false,closeAfterWrite=false,closeWhenIdle=false,shutdownRequested=false,closingAllowed=false;
    QString pendingDelimiter;
    void rebuild();
    void adoptSnapshot(const TargetSnapshot& snapshot);
    QStringList visibleIds()const;
    void rememberScope();
    bool finish();
    void showDialog(const QString& type,const QString& title,const QString& text);
    void persistDraft(bool close=false);
    void saveConfirmed();
    QVariantList difference(const TargetSnapshot& from,const TargetSnapshot& to) const;
    void fail(const QString& code);
    template<class F,class C> void job(F work,C done) {
        using T=decltype(work());
        busy=true;emit changed();auto* w=new QFutureWatcher<T>(this);
        connect(w,&QFutureWatcher<T>::finished,this,[this,w,done]{auto result=w->result();w->deleteLater();busy=false;done(result);emit changed();
            if(closeWhenIdle&&!busy){closeWhenIdle=false;requestClose(shutdownRequested);}});
        w->setFuture(QtConcurrent::run(std::move(work)));
    }
};
void initialize();
bool openEditor(const OpenArgs& a,std::string& error);
// Queue window creation on the Qt GUI thread so a launcher action can answer
// immediately and let WinShun hide its search window before QML loads.
bool openEditorDeferred(const OpenArgs& a);
bool isEditorOpen(const std::string& kind);
void onExternalChange(const std::string& kind);
void onAppearanceChanged(envh::i18n::Lang lang,bool dark);
void closeAllEditors(unsigned timeoutMs);
void shutdownWhenReady();
}
