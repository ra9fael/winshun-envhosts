// qt_editor.h - Qt Quick front-end for the editor window (migration prototype).
//
// Compiled instead of editor_win.h when the build sets ENVHOSTS_QML_EDITOR. It
// keeps the exact same free-function API (openEditor / isEditorOpen /
// onExternalChange / closeAllEditors), so main.cpp does not care which front-end
// it talks to. Qt Quick needs the GUI thread, so the protocol loop moves to a
// worker thread in this build (see main.cpp) and every call from the protocol
// thread is marshalled to the GUI thread.
//
// Scope of this slice: read-only env and hosts views (table, filter, detail
// panel, status bar). Editing, drafts, backups and the hosts raw tab are still
// served by the GDI window until their slices land.
//
// Code and comments are English.
#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <string>
#include <vector>

#include "editor_common.h"
#include "editor_model.h"

namespace editorwin {

// One flat row model for both targets: `cells` feeds the table, `data` feeds the
// detail panel, `kind` carries section/invalid/deleted styling.
class EditorRows : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { CellsRole = Qt::UserRole + 1, DataRole, KindRole };

    struct Row {
        QStringList cells;
        QVariantMap data;
        int kind = 0; // 0 normal, 1 section heading, 2 invalid, 3 deleted
    };

    int rowCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }
    QVariant data(const QModelIndex& idx, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void resetRows(QVector<Row> rows);

private:
    QVector<Row> m_rows;
};

// Per-window state: loads the model once, rebuilds the visible rows on filter or
// external change, and exposes the current row's detail to QML.
class EditorBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString kind READ kind CONSTANT)
    Q_PROPERTY(QString title READ title NOTIFY metaChanged)
    Q_PROPERTY(QStringList columns READ columns NOTIFY metaChanged)
    Q_PROPERTY(int primaryWidth READ primaryWidth NOTIFY metaChanged)
    Q_PROPERTY(bool dark READ dark NOTIFY metaChanged)
    Q_PROPERTY(bool writable READ writable NOTIFY metaChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(EditorRows* rows READ rows CONSTANT)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentRowChanged)
    Q_PROPERTY(QString detailTitle READ detailTitle NOTIFY currentRowChanged)
    Q_PROPERTY(QVariantList detailFields READ detailFields NOTIFY currentRowChanged)
    Q_PROPERTY(QString statusLine READ statusLine NOTIFY statusChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorChanged)

public:
    EditorBridge(OpenArgs args, QObject* parent = nullptr);

    QString kind() const { return QString::fromStdString(m_args.kind); }
    QString title() const;
    QStringList columns() const;
    int primaryWidth() const { return m_kind == "env" ? 260 : 300; }
    bool dark() const { return m_args.dark; }
    bool writable() const;
    QString filter() const { return m_filterText; }
    void setFilter(const QString& text);
    EditorRows* rows() { return &m_rows; }
    int currentIndex() const { return m_sel; }
    QString detailTitle() const;
    QVariantList detailFields() const;
    QString statusLine() const;
    QString errorText() const { return m_error; }

    Q_INVOKABLE void selectRow(int row);

    bool load(QString& error);
    void applyFocus(const OpenArgs& a);
    void reload();

signals:
    void metaChanged();
    void filterChanged();
    void currentRowChanged();
    void statusChanged();
    void errorChanged();
    void rowSelected(int row);

private:
    void rebuild();
    void setError(const QString& text);

    OpenArgs m_args;
    std::string m_kind;
    EditorRows m_rows;
    envh::edit::EnvModel m_env;
    envh::edit::HostsModel m_hosts;
    bool m_hasModel = false;
    QString m_filterText;
    QString m_error;
    int m_sel = 0;
    std::vector<int> m_visible;
};

// Must run once on the GUI thread, after the QGuiApplication exists and before
// the protocol loop starts. Creates the QML engine and the window controller.
void initialize();

// Protocol front-end: identical signatures to the Win32 implementation.
bool openEditor(const OpenArgs& a, std::string& error);
bool isEditorOpen(const std::string& kind);
void onExternalChange(const std::string& kind);
void closeAllEditors(unsigned timeoutMs);

} // namespace editorwin
