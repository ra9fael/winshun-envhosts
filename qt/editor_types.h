#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QUuid>
#include <QVector>
#include <algorithm>

namespace editorwin {

inline QString freshId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
inline QString digest(const QByteArray& bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); }
inline QString envKey(const QString& scope, const QString& name) { return scope + "/" + name.toCaseFolded(); }
inline QByteArray utf16Bytes(const QString& text) {
    QByteArray bytes; bytes.reserve(text.size() * 2);
    for (QChar c : text) { bytes.append(char(c.unicode() & 255)); bytes.append(char(c.unicode() >> 8)); }
    return bytes;
}
inline QString fromUtf16Bytes(const QByteArray& bytes) {
    QString text; text.reserve(bytes.size() / 2);
    for (qsizetype i = 0; i + 1 < bytes.size(); i += 2)
        text.append(QChar(quint16(quint8(bytes[i])) | (quint16(quint8(bytes[i+1])) << 8)));
    return text;
}

struct EnvValue {
    QString id, scope, name, type = "REG_SZ", value, formatError;
    quint32 nativeType = 1;
    QByteArray bytes;
    bool deleted = false, isNew = false;
    QString key() const { return envKey(scope, name); }
    void encode() {
        nativeType = type == "REG_EXPAND_SZ" ? 2 : type == "REG_MULTI_SZ" ? 7 : 1;
        QString encoded = value;
        if (nativeType == 7) {
            encoded.replace('\n', QChar(0));
            encoded += QChar(0);
            if (!value.isEmpty()) encoded += QChar(0);
        } else encoded += QChar(0);
        if (nativeType == 7 && value.isEmpty()) encoded += QChar(0);
        bytes = utf16Bytes(encoded);
    }
    QJsonObject json() const {
        return {{"id",id},{"scope",scope},{"name",name},{"type",type},{"value",value},
                {"nativeType",int(nativeType)},{"bytes",QString::fromLatin1(bytes.toBase64())},
                {"formatError",formatError},{"deleted",deleted},{"isNew",isNew}};
    }
    static EnvValue fromJson(const QJsonObject& j) {
        EnvValue v; v.id=j["id"].toString();v.scope=j["scope"].toString();v.name=j["name"].toString();
        v.type=j["type"].toString();v.value=j["value"].toString();v.nativeType=quint32(j["nativeType"].toInt(1));
        v.bytes=QByteArray::fromBase64(j["bytes"].toString().toLatin1());v.formatError=j["formatError"].toString();
        v.deleted=j["deleted"].toBool();v.isNew=j["isNew"].toBool();return v;
    }
};
inline bool sameValue(const EnvValue& a, const EnvValue& b) {
    return a.scope == b.scope && a.name == b.name && a.nativeType == b.nativeType && a.bytes == b.bytes;
}

struct TargetSnapshot {
    QVector<EnvValue> env;
    QByteArray hosts;
    bool userWritable=false, systemWritable=false, hostsWritable=false;
    QString error;
    QJsonObject json() const {
        QJsonArray vars; for (const auto& v : env) vars.append(v.json());
        return {{"vars",vars},{"hostsBase64",QString::fromLatin1(hosts.toBase64())}};
    }
    QString fingerprint(const QString& scope) const {
        if (scope == "hosts") return digest(hosts);
        QVector<EnvValue> sorted;
        for (const auto& v : env) if (v.scope == scope && !v.deleted) sorted.append(v);
        std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.key()<b.key();});
        QJsonArray data;
        for (const auto& v : sorted) data.append(QJsonArray{v.name, int(v.nativeType), QString::fromLatin1(v.bytes.toBase64())});
        return digest(QJsonDocument(data).toJson(QJsonDocument::Compact));
    }
};

// A visible-order selection; focus and the range anchor are independent of the set.
struct Selection {
    QSet<QString> ids;
    QString current, anchor;
    void prune(const QStringList& visible) {
        const QSet<QString> allowed(visible.begin(),visible.end()); ids.intersect(allowed);
        if (!allowed.contains(current)) current=visible.value(0);
        if (!allowed.contains(anchor)) anchor=current;
    }
    // Keep identity across readback; removed rows fall forward, then backward,
    // within the previous visible order. An explicitly cleared selection stays clear.
    void restore(const QStringList& before, const QStringList& visible) {
        const QSet<QString> allowed(visible.begin(),visible.end());
        const bool hadSelection=!ids.isEmpty();
        ids.intersect(allowed);
        if (!allowed.contains(current)) {
            const int position=int(before.indexOf(current));
            const auto nearby=[&](const QSet<QString>& candidates) {
                for (int i=position+1;i<before.size();++i) if (candidates.contains(before[i])) return before[i];
                for (int i=position-1;i>=0;--i) if (candidates.contains(before[i])) return before[i];
                for (const auto& id:visible) if (candidates.contains(id)) return id;
                return QString{};
            };
            current=nearby(ids.isEmpty()?allowed:ids);
            if (hadSelection&&ids.isEmpty()&&!current.isEmpty()) ids.insert(current);
        }
        if (!allowed.contains(anchor)) anchor=current;
    }
    void select(const QString& id, const QStringList& visible, bool ctrl=false, bool shift=false) {
        if (!visible.contains(id)) return;
        if (shift && visible.contains(anchor)) {
            if (!ctrl) ids.clear();
            const int a=int(visible.indexOf(anchor)), b=int(visible.indexOf(id));
            for (int i=std::min(a,b);i<=std::max(a,b);++i) ids.insert(visible[i]);
        } else if (ctrl) {
            if (ids.contains(id)) ids.remove(id); else ids.insert(id);
            anchor=id;
        } else { ids={id};anchor=id; }
        current=id;
    }
    void all(const QStringList& visible) { ids=QSet<QString>(visible.begin(),visible.end());current=visible.value(0);anchor=current; }
    QStringList ordered(const QStringList& visible) const {
        QStringList out;for(const auto& id:visible)if(ids.contains(id))out.append(id);return out;
    }
};

struct Change {
    QString operation, scope, name;
    EnvValue before, after;
    bool hasBefore=false, hasAfter=false;
};

} // namespace editorwin
