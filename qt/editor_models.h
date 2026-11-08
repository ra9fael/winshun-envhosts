#pragma once
#include <QAbstractListModel>
#include <QVariantMap>
#include <QVector>

namespace editorwin {
class EditorRows : public QAbstractListModel {
    Q_OBJECT
public:
    explicit EditorRows(QObject* parent=nullptr):QAbstractListModel(parent){}
    enum { RecordRole=Qt::UserRole+1 };
    int rowCount(const QModelIndex& parent={}) const override {return parent.isValid()?0:int(records.size());}
    QVariant data(const QModelIndex& index,int role) const override;
    QHash<int,QByteArray> roleNames() const override {return {{RecordRole,"record"}};}
    void sync(const QVector<QVariantMap>& values);
    Q_INVOKABLE QVariantMap get(int index) const {return index>=0&&index<records.size()?records[index]:QVariantMap{};}
private:
    QVector<QVariantMap> records;
};
}
