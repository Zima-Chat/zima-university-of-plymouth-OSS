#pragma once
// ---------------------------------------------------------------------------
// MemoryStore — the assistant's long-term memory (local only).
// ---------------------------------------------------------------------------
// Durable facts the agent chooses to remember across conversations, saved as
// JSON under the per-user *local* app data folder (never synced/cloud). These
// notes are injected into every prompt so the agent has continuity, and the
// `recall` tool can also search them + past conversations on demand.
// ---------------------------------------------------------------------------

#include <QString>
#include <QList>
#include <QDateTime>

struct MemoryNote {
    QDateTime time;
    QString text;
};

class MemoryStore {
public:
    static MemoryStore& instance();

    void add(const QString& note);
    void clear();
    int count() const { return notes_.size(); }
    QList<MemoryNote> all() const { return notes_; }

    // Bulleted digest of the most recent notes, capped for the prompt.
    QString digest(int maxNotes = 40, int maxChars = 4000) const;

private:
    MemoryStore();
    void load();
    void save() const;

    QString file_;
    QList<MemoryNote> notes_;
};
