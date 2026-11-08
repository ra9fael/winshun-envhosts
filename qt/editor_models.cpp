#include "editor_models.h"
#include <QSet>

namespace editorwin {
QVariant EditorRows::data(const QModelIndex& index,int role) const {
    if(!index.isValid()||index.row()<0||index.row()>=records.size()||role!=RecordRole)return {};
    return records[index.row()];
}
void EditorRows::sync(const QVector<QVariantMap>& values) {
    QSet<QString> keep;for(const auto& v:values)keep.insert(v["id"].toString());
    for(int i=int(records.size())-1;i>=0;){
        if(keep.contains(records[i]["id"].toString())){--i;continue;}
        const int end=i;while(i>=0&&!keep.contains(records[i]["id"].toString()))--i;
        beginRemoveRows({},i+1,end);records.remove(i+1,end-i);endRemoveRows();
    }
    if(records.isEmpty()&&!values.isEmpty()){beginInsertRows({},0,int(values.size())-1);records=values;endInsertRows();return;}
    // A sort is a permutation, not thousands of separate row moves. Preserve
    // persistent indexes by stable ID while notifying views of the new layout.
    if(records.size()==values.size()){
        bool reordered=false;QHash<QString,int> oldPositions,newPositions;
        for(int i=0;i<records.size();++i){oldPositions.insert(records[i]["id"].toString(),i);newPositions.insert(values[i]["id"].toString(),i);reordered|=records[i]["id"]!=values[i]["id"];}
        if(reordered){
            emit layoutAboutToBeChanged({},QAbstractItemModel::VerticalSortHint);
            const auto before=persistentIndexList();QModelIndexList after;
            for(const auto& p:before)after.append(index(newPositions.value(records[p.row()]["id"].toString()),p.column()));
            QVector<QVariantMap> sorted;sorted.reserve(records.size());
            for(const auto& v:values)sorted.append(records[oldPositions.value(v["id"].toString())]);
            records=std::move(sorted);changePersistentIndexList(before,after);
            emit layoutChanged({},QAbstractItemModel::VerticalSortHint);
        }
    }
    for(int i=0;i<values.size();++i){
        const QString id=values[i]["id"].toString();
        if(i>=records.size()||records[i]["id"].toString()!=id){
            int from=i+1;while(from<records.size()&&records[from]["id"].toString()!=id)++from;
            if(from<records.size()){
                if(beginMoveRows({},from,from,{},i)){records.move(from,i);endMoveRows();}
            }else{beginInsertRows({},i,i);records.insert(i,values[i]);endInsertRows();}
        }
        if(records[i]!=values[i]){records[i]=values[i];emit dataChanged(index(i),index(i),{RecordRole});}
    }
}
}
