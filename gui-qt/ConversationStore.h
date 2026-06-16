#pragma once
// ---------------------------------------------------------------------------
// ConversationStore — local-only chat history.
// ---------------------------------------------------------------------------
// Conversations are saved as JSON files under the per-user *local* app data
// folder (QStandardPaths::AppLocalDataLocation, i.e. %LOCALAPPDATA%\Zima on
// Windows — non-roaming, never synced). This is purely local disk I/O: nothing
// here touches the network or the cloud.
// ---------------------------------------------------------------------------

#include <QString>
#include <QList>
#include <QDateTime>

struct StoredMessage {
    int role = 0;        // MessageWidget::Role (0 user, 1 assistant, 2 system)
    QString text;
};

struct Conversation {
    QString id;
    QString title;
    QString context;     // the model-facing convo_ string, for resuming
    QDateTime created;
    QDateTime updated;
    QList<StoredMessage> messages;
};

struct ConversationMeta {
    QString id;
    QString title;
    QDateTime updated;
};

class ConversationStore {
public:
    ConversationStore();

    QString directory() const { return dir_; }
    void save(const Conversation& c) const;
    Conversation load(const QString& id) const;
    QList<ConversationMeta> list() const;   // newest first
    void remove(const QString& id) const;

    // Search across all saved conversations for `query` (case-insensitive).
    // Returns formatted excerpt lines, e.g. "[Title] …matching text…".
    QStringList search(const QString& query, int maxHits = 20) const;

    static QString newId();

private:
    QString pathFor(const QString& id) const;
    QString dir_;
};
