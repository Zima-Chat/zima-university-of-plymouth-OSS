#include "MemoryStore.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

MemoryStore& MemoryStore::instance() {
    static MemoryStore s;
    return s;
}

MemoryStore::MemoryStore() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dir);
    file_ = dir + QStringLiteral("/memory.json");
    load();
}

void MemoryStore::load() {
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        MemoryNote n;
        n.time = QDateTime::fromString(o.value(QStringLiteral("time")).toString(), Qt::ISODate);
        n.text = o.value(QStringLiteral("note")).toString();
        if (!n.text.isEmpty()) notes_.append(n);
    }
}

void MemoryStore::save() const {
    QJsonArray arr;
    for (const MemoryNote& n : notes_) {
        QJsonObject o;
        o[QStringLiteral("time")] = n.time.toString(Qt::ISODate);
        o[QStringLiteral("note")] = n.text;
        arr.append(o);
    }
    QFile f(file_);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

void MemoryStore::add(const QString& note) {
    const QString t = note.trimmed();
    if (t.isEmpty()) return;
    // de-dupe exact repeats
    for (const MemoryNote& n : notes_) if (n.text == t) return;
    notes_.append({QDateTime::currentDateTime(), t});
    save();
}

void MemoryStore::clear() {
    notes_.clear();
    save();
}

QString MemoryStore::digest(int maxNotes, int maxChars) const {
    if (notes_.isEmpty()) return {};
    QString out;
    const int start = qMax(0, notes_.size() - maxNotes);
    for (int i = start; i < notes_.size(); ++i) {
        out += QStringLiteral("- ") + notes_[i].text + QLatin1Char('\n');
        if (out.size() > maxChars) { out += QStringLiteral("…\n"); break; }
    }
    return out;
}
