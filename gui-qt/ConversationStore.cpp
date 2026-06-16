#include "ConversationStore.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>

ConversationStore::ConversationStore() {
    dir_ = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
         + QStringLiteral("/conversations");
    QDir().mkpath(dir_);
}

QString ConversationStore::newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString ConversationStore::pathFor(const QString& id) const {
    return dir_ + QLatin1Char('/') + id + QStringLiteral(".json");
}

void ConversationStore::save(const Conversation& c) const {
    if (c.id.isEmpty()) return;
    QJsonObject o;
    o[QStringLiteral("id")] = c.id;
    o[QStringLiteral("title")] = c.title;
    o[QStringLiteral("context")] = c.context;
    o[QStringLiteral("created")] = c.created.toString(Qt::ISODate);
    o[QStringLiteral("updated")] = c.updated.toString(Qt::ISODate);
    QJsonArray msgs;
    for (const StoredMessage& m : c.messages) {
        QJsonObject mo;
        mo[QStringLiteral("role")] = m.role;
        mo[QStringLiteral("text")] = m.text;
        msgs.append(mo);
    }
    o[QStringLiteral("messages")] = msgs;

    QFile f(pathFor(c.id));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

Conversation ConversationStore::load(const QString& id) const {
    Conversation c;
    QFile f(pathFor(id));
    if (!f.open(QIODevice::ReadOnly)) return c;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    c.id = o.value(QStringLiteral("id")).toString();
    c.title = o.value(QStringLiteral("title")).toString();
    c.context = o.value(QStringLiteral("context")).toString();
    c.created = QDateTime::fromString(o.value(QStringLiteral("created")).toString(), Qt::ISODate);
    c.updated = QDateTime::fromString(o.value(QStringLiteral("updated")).toString(), Qt::ISODate);
    for (const QJsonValue& v : o.value(QStringLiteral("messages")).toArray()) {
        const QJsonObject mo = v.toObject();
        StoredMessage m;
        m.role = mo.value(QStringLiteral("role")).toInt();
        m.text = mo.value(QStringLiteral("text")).toString();
        c.messages.append(m);
    }
    return c;
}

QList<ConversationMeta> ConversationStore::list() const {
    QList<ConversationMeta> out;
    QDir d(dir_);
    const auto files = d.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString& fn : files) {
        QFile f(d.filePath(fn));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        ConversationMeta m;
        m.id = o.value(QStringLiteral("id")).toString();
        m.title = o.value(QStringLiteral("title")).toString();
        m.updated = QDateTime::fromString(o.value(QStringLiteral("updated")).toString(), Qt::ISODate);
        if (!m.id.isEmpty()) out.append(m);
    }
    std::sort(out.begin(), out.end(), [](const ConversationMeta& a, const ConversationMeta& b) {
        return a.updated > b.updated;
    });
    return out;
}

void ConversationStore::remove(const QString& id) const {
    QFile::remove(pathFor(id));
}

QStringList ConversationStore::search(const QString& query, int maxHits) const {
    QStringList hits;
    const QString q = query.trimmed();
    if (q.isEmpty()) return hits;
    // Newest first so the most recent context surfaces.
    for (const ConversationMeta& meta : list()) {
        if (hits.size() >= maxHits) break;
        const Conversation c = load(meta.id);
        for (const StoredMessage& m : c.messages) {
            if (hits.size() >= maxHits) break;
            const int idx = m.text.indexOf(q, 0, Qt::CaseInsensitive);
            if (idx < 0) continue;
            const int from = qMax(0, idx - 80);
            QString excerpt = m.text.mid(from, 280).simplified();
            hits << QStringLiteral("[%1] …%2…").arg(c.title.isEmpty()
                        ? QStringLiteral("untitled") : c.title, excerpt);
        }
    }
    return hits;
}
