#include "Tools.h"
#include "Workspace.h"
#include "Diff.h"
#include "MemoryStore.h"
#include "ConversationStore.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QProcess>
#include <QRegularExpression>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QFontDatabase>
#include <QVector>

namespace {

constexpr int kMaxFileChars = 16000;   // cap content fed to the model
constexpr int kMaxOutputChars = 8000;
constexpr int kMaxSearchHits = 100;

QString cap(const QString& s, int max) {
    if (s.size() <= max) return s;
    return s.left(max) + QStringLiteral("\n…(truncated, %1 more chars)").arg(s.size() - max);
}

ToolOutcome err(const QString& note, const QString& modelMsg) {
    ToolOutcome o; o.ok = false; o.note = note; o.resultForModel = modelMsg; return o;
}

// ---- reversible edits: a backup of each applied write/delete ----
struct EditRecord {
    QString absPath;        // absolute path that was changed
    QString rel;            // workspace-relative path (for labels)
    QString prevContent;    // file content before the change
    bool    existedBefore;  // false → the change created the file
};
static QVector<EditRecord> g_undo;

// Modal that shows a unified diff and asks the user to apply or reject it.
bool approveDiff(QWidget* parent, const QString& path, const QString& diff,
                 int added, int removed) {
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Zima — review change"));
    dlg.resize(720, 520);
    auto* v = new QVBoxLayout(&dlg);
    auto* head = new QLabel(QStringLiteral("Apply changes to  %1   (+%2 −%3)")
                                .arg(path).arg(added).arg(removed));
    head->setObjectName(QStringLiteral("SectionLabel"));
    v->addWidget(head);
    auto* view = new QPlainTextEdit;
    view->setReadOnly(true);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->setPlainText(diff);
    v->addWidget(view, 1);
    auto* row = new QHBoxLayout;
    row->addStretch();
    auto* reject = new QPushButton(QStringLiteral("Reject"));
    auto* apply = new QPushButton(QStringLiteral("Apply"));
    apply->setObjectName(QStringLiteral("Primary"));
    row->addWidget(reject);
    row->addWidget(apply);
    v->addLayout(row);
    QObject::connect(reject, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(apply, &QPushButton::clicked, &dlg, &QDialog::accept);
    return dlg.exec() == QDialog::Accepted;
}

bool approveCommand(QWidget* parent, const QString& command) {
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Zima — run command"));
    dlg.resize(620, 240);
    auto* v = new QVBoxLayout(&dlg);
    auto* head = new QLabel(QStringLiteral("RUN COMMAND IN WORKSPACE"));
    head->setObjectName(QStringLiteral("SectionLabel"));
    v->addWidget(head);
    auto* cmd = new QPlainTextEdit;
    cmd->setReadOnly(true);
    cmd->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    cmd->setPlainText(command);
    cmd->setFixedHeight(90);
    v->addWidget(cmd);
    auto* warn = new QLabel(QStringLiteral(
        "This runs on your machine with your permissions. Only approve commands you trust."));
    warn->setObjectName(QStringLiteral("StatusLabel"));
    warn->setWordWrap(true);
    v->addWidget(warn);
    auto* row = new QHBoxLayout;
    row->addStretch();
    auto* reject = new QPushButton(QStringLiteral("Decline"));
    auto* runBtn = new QPushButton(QStringLiteral("Run"));
    runBtn->setObjectName(QStringLiteral("Primary"));
    row->addWidget(reject);
    row->addWidget(runBtn);
    v->addLayout(row);
    QObject::connect(reject, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(runBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    return dlg.exec() == QDialog::Accepted;
}

// ---- individual tools ----

ToolOutcome readFile(const QString& rel) {
    QString abs = Workspace::instance().resolve(rel);
    if (abs.isEmpty()) return err(QStringLiteral("✗ read_file %1 (blocked)").arg(rel),
        QStringLiteral("error: '%1' is outside the workspace or no folder is open.").arg(rel));
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return err(QStringLiteral("✗ read_file %1 (not found)").arg(rel),
                   QStringLiteral("error: cannot open '%1'.").arg(rel));
    QString content = QString::fromUtf8(f.readAll());
    int lines = content.count(QLatin1Char('\n')) + 1;
    ToolOutcome o;
    o.note = QStringLiteral("⏵ read_file  %1  (%2 lines)").arg(rel).arg(lines);
    o.resultForModel = QStringLiteral("Contents of %1:\n%2").arg(rel, cap(content, kMaxFileChars));
    return o;
}

ToolOutcome listDir(const QString& rel) {
    const QString r = rel.isEmpty() || rel == QStringLiteral(".") ? QString() : rel;
    QString abs = Workspace::instance().resolve(r.isEmpty() ? QStringLiteral(".") : r);
    if (abs.isEmpty()) return err(QStringLiteral("✗ list_dir %1 (blocked)").arg(rel),
        QStringLiteral("error: '%1' is outside the workspace or no folder is open.").arg(rel));
    QDir d(abs);
    if (!d.exists()) return err(QStringLiteral("✗ list_dir %1 (not found)").arg(rel),
                                QStringLiteral("error: directory '%1' not found.").arg(rel));
    QString out;
    const auto entries = d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                         QDir::DirsFirst | QDir::Name);
    for (const QFileInfo& fi : entries)
        out += (fi.isDir() ? QStringLiteral("[dir]  ") : QStringLiteral("       "))
             + fi.fileName() + QLatin1Char('\n');
    ToolOutcome o;
    o.note = QStringLiteral("⏵ list_dir  %1  (%2 entries)")
                 .arg(rel.isEmpty() ? QStringLiteral(".") : rel).arg(entries.size());
    o.resultForModel = QStringLiteral("Entries of %1:\n%2")
                           .arg(rel.isEmpty() ? QStringLiteral(".") : rel, out);
    return o;
}

ToolOutcome search(const QString& pattern) {
    const QString root = Workspace::instance().root();
    if (root.isEmpty()) return err(QStringLiteral("✗ search (no folder)"),
        QStringLiteral("error: no workspace folder is open."));
    QRegularExpression re(pattern);
    bool useRegex = re.isValid();
    QString out; int hits = 0;
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && hits < kMaxSearchHits) {
        const QString path = it.next();
        if (path.contains(QStringLiteral("/.git/"))) continue;
        QFileInfo fi(path);
        if (fi.size() > 1024 * 1024) continue; // skip large/binary
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString rel = QDir(root).relativeFilePath(path);
        int lineNo = 0;
        while (!f.atEnd() && hits < kMaxSearchHits) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            ++lineNo;
            const bool match = useRegex ? line.contains(re) : line.contains(pattern);
            if (match) {
                out += QStringLiteral("%1:%2: %3\n").arg(rel).arg(lineNo).arg(cap(line, 200));
                ++hits;
            }
        }
    }
    ToolOutcome o;
    o.note = QStringLiteral("⏵ search  \"%1\"  (%2 hits)").arg(pattern).arg(hits);
    o.resultForModel = hits ? QStringLiteral("Search results for '%1':\n%2").arg(pattern, out)
                            : QStringLiteral("No matches for '%1'.").arg(pattern);
    return o;
}

// Synchronous write used by tools::run as a fallback (the GUI normally drives
// previewWrite()/applyWrite() so the diff shows in the side panel instead).
ToolOutcome writeFile(QWidget* parent, const QString& rel, const QString& content) {
    WritePreview pv = tools::previewWrite(rel, content);
    if (!pv.ok) return err(pv.errNote, pv.errModel);
    if (pv.existedBefore && !approveDiff(parent, pv.rel, pv.diff, pv.added, pv.removed)) {
        ToolOutcome o; o.ok = false;
        o.note = QStringLiteral("✗ rejected write  %1").arg(pv.rel);
        o.resultForModel = QStringLiteral("The user rejected the change to '%1'.").arg(pv.rel);
        return o;
    }
    return tools::applyWrite(pv);
}

ToolOutcome runCommand(QWidget* parent, const QString& command) {
    const QString root = Workspace::instance().root();
    if (root.isEmpty()) return err(QStringLiteral("✗ run_command (no folder)"),
        QStringLiteral("error: no workspace folder is open; ask the user to open one."));
    if (command.trimmed().isEmpty()) return err(QStringLiteral("✗ run_command (empty)"),
        QStringLiteral("error: empty command."));
    if (!approveCommand(parent, command)) {
        ToolOutcome o; o.ok = false;
        o.note = QStringLiteral("✗ declined  %1").arg(command);
        o.resultForModel = QStringLiteral("The user declined to run that command.");
        return o;
    }
    QProcess proc;
    proc.setWorkingDirectory(root);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), command});
    QString output;
    int exitCode = -1;
    if (!proc.waitForStarted(5000)) {
        output = QStringLiteral("(failed to start)");
    } else {
        proc.waitForFinished(120000); // 2 min cap
        output = QString::fromLocal8Bit(proc.readAll());
        exitCode = proc.exitCode();
        if (proc.state() != QProcess::NotRunning) { proc.kill(); output += QStringLiteral("\n(killed: timeout)"); }
    }
    ToolOutcome o;
    o.ok = (exitCode == 0);
    o.note = QStringLiteral("▶ ran  %1  (exit %2)").arg(command).arg(exitCode);
    o.block = QStringLiteral("```\n$ %1\n%2\n```").arg(command, output.trimmed());
    o.resultForModel = QStringLiteral("Command: %1\nExit code: %2\nOutput:\n%3")
                           .arg(command).arg(exitCode).arg(cap(output, kMaxOutputChars));
    return o;
}

ToolOutcome deleteFile(QWidget* parent, const QString& rel) {
    QString abs = Workspace::instance().resolve(rel);
    if (abs.isEmpty()) return err(QStringLiteral("✗ delete_file %1 (blocked)").arg(rel),
        QStringLiteral("error: '%1' is outside the workspace or no folder is open.").arg(rel));
    QFileInfo fi(abs);
    if (!fi.exists() || !fi.isFile())
        return err(QStringLiteral("✗ delete_file %1 (not found)").arg(rel),
                   QStringLiteral("error: file '%1' does not exist.").arg(rel));

    auto r = QMessageBox::warning(parent, QStringLiteral("Zima — delete file"),
        QStringLiteral("Delete \"%1\"?\nThis can be undone.").arg(rel),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r != QMessageBox::Yes) {
        ToolOutcome o; o.ok = false;
        o.note = QStringLiteral("✗ declined delete  %1").arg(rel);
        o.resultForModel = QStringLiteral("The user declined to delete '%1'.").arg(rel);
        return o;
    }
    QString prev;
    { QFile f(abs); if (f.open(QIODevice::ReadOnly | QIODevice::Text)) prev = QString::fromUtf8(f.readAll()); }
    if (!QFile::remove(abs))
        return err(QStringLiteral("✗ delete_file %1 (failed)").arg(rel),
                   QStringLiteral("error: could not delete '%1'.").arg(rel));
    g_undo.push_back({abs, rel, prev, true});   // undo recreates the file
    ToolOutcome o;
    o.note = QStringLiteral("🗑 deleted  %1").arg(rel);
    o.resultForModel = QStringLiteral("Deleted '%1'. This can be undone by the user.").arg(rel);
    return o;
}

ToolOutcome rememberNote(const QString& note) {
    if (note.trimmed().isEmpty())
        return err(QStringLiteral("✗ remember (empty)"), QStringLiteral("error: empty note."));
    MemoryStore::instance().add(note);
    ToolOutcome o;
    o.note = QStringLiteral("✸ remembered  “%1”").arg(note.simplified().left(60));
    o.resultForModel = QStringLiteral("Saved to long-term memory.");
    return o;
}

ToolOutcome recall(const QString& query) {
    QStringList lines;
    // long-term memory notes
    for (const MemoryNote& n : MemoryStore::instance().all())
        if (n.text.contains(query, Qt::CaseInsensitive))
            lines << QStringLiteral("(memory) ") + n.text;
    // past conversations
    ConversationStore cs;
    lines += cs.search(query, 20);

    ToolOutcome o;
    o.note = QStringLiteral("⏵ recall  \"%1\"  (%2 hits)").arg(query).arg(lines.size());
    o.resultForModel = lines.isEmpty()
        ? QStringLiteral("No memory or past conversation matched '%1'.").arg(query)
        : QStringLiteral("Recall results for '%1':\n%2").arg(query, lines.join(QLatin1Char('\n')));
    return o;
}

} // namespace

namespace tools {

WritePreview previewWrite(const QString& rel, const QString& content) {
    WritePreview pv;
    pv.rel = rel;
    pv.content = content;
    pv.abs = Workspace::instance().resolve(rel);
    if (pv.abs.isEmpty()) {
        pv.errNote = QStringLiteral("✗ write_file %1 (blocked)").arg(rel);
        pv.errModel = QStringLiteral("error: '%1' is outside the workspace or no folder is open.").arg(rel);
        return pv;
    }
    pv.existedBefore = QFileInfo::exists(pv.abs);
    { QFile f(pv.abs); if (f.open(QIODevice::ReadOnly | QIODevice::Text)) pv.oldText = QString::fromUtf8(f.readAll()); }
    diffutil::counts(pv.oldText, content, pv.added, pv.removed);
    pv.diff = diffutil::unified(pv.oldText, content, rel);
    pv.ok = true;
    return pv;
}

ToolOutcome applyWrite(const WritePreview& pv) {
    QFileInfo fi(pv.abs);
    if (!fi.dir().exists()) QDir().mkpath(fi.dir().absolutePath());   // new folders, no prompt
    QFile f(pv.abs);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return err(QStringLiteral("✗ write_file %1 (failed)").arg(pv.rel),
                   QStringLiteral("error: cannot write '%1'.").arg(pv.rel));
    f.write(pv.content.toUtf8());
    f.close();
    g_undo.push_back({pv.abs, pv.rel, pv.oldText, pv.existedBefore});   // reversible
    ToolOutcome o;
    o.note = pv.existedBefore
        ? QStringLiteral("✎ wrote  %1  (+%2 −%3)").arg(pv.rel).arg(pv.added).arg(pv.removed)
        : QStringLiteral("✚ created  %1  (%2 lines)").arg(pv.rel).arg(pv.added);
    o.resultForModel = pv.existedBefore
        ? QStringLiteral("Wrote '%1' (%2 added, %3 removed). Applied; the user can undo it.")
              .arg(pv.rel).arg(pv.added).arg(pv.removed)
        : QStringLiteral("Created '%1' (%2 lines). The user can undo it.").arg(pv.rel).arg(pv.added);
    return o;
}

ToolOutcome run(QWidget* parent, const QString& name, const QJsonObject& args) {
    const QString path = args.value(QStringLiteral("path")).toString();
    if (name == QStringLiteral("read_file"))  return readFile(path);
    if (name == QStringLiteral("list_dir"))   return listDir(path);
    if (name == QStringLiteral("search"))     return search(args.value(QStringLiteral("pattern")).toString());
    if (name == QStringLiteral("write_file") || name == QStringLiteral("create_file"))
        return writeFile(parent, path, args.value(QStringLiteral("content")).toString());
    if (name == QStringLiteral("run_command"))
        return runCommand(parent, args.value(QStringLiteral("command")).toString());
    if (name == QStringLiteral("delete_file"))
        return deleteFile(parent, path);
    if (name == QStringLiteral("remember"))
        return rememberNote(args.value(QStringLiteral("note")).toString());
    if (name == QStringLiteral("recall"))
        return recall(args.value(QStringLiteral("query")).toString());
    return err(QStringLiteral("✗ unknown tool: %1").arg(name),
               QStringLiteral("error: unknown tool '%1'. Available: read_file, list_dir, search, "
                              "write_file, delete_file, run_command, remember, recall.").arg(name));
}

bool canUndo() { return !g_undo.isEmpty(); }

// Revert the most recent applied write/delete, presented as a diff to approve.
ToolOutcome undoLast(QWidget* parent) {
    if (g_undo.isEmpty())
        return err(QStringLiteral("nothing to undo"), QString());
    const EditRecord r = g_undo.last();

    QString cur;
    { QFile f(r.absPath); if (f.open(QIODevice::ReadOnly | QIODevice::Text)) cur = QString::fromUtf8(f.readAll()); }
    const bool existsNow = QFileInfo::exists(r.absPath);

    if (r.existedBefore) {
        // restore previous content (covers both edits and deletions)
        int added = 0, removed = 0;
        diffutil::counts(cur, r.prevContent, added, removed);
        const QString diff = diffutil::unified(cur, r.prevContent, r.rel);
        if (!approveDiff(parent, r.rel, diff, added, removed)) {
            ToolOutcome o; o.ok = false; o.note = QStringLiteral("undo cancelled"); return o;
        }
        QFileInfo fi(r.absPath);
        if (!fi.dir().exists()) QDir().mkpath(fi.dir().absolutePath());
        QFile f(r.absPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return err(QStringLiteral("✗ undo failed (%1)").arg(r.rel), QString());
        f.write(r.prevContent.toUtf8());
        f.close();
    } else {
        // the change had created the file → undo removes it
        if (existsNow) {
            auto res = QMessageBox::question(parent, QStringLiteral("Zima — undo"),
                QStringLiteral("Undo will delete the newly-created file \"%1\". Continue?").arg(r.rel),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (res != QMessageBox::Yes) { ToolOutcome o; o.ok = false; o.note = QStringLiteral("undo cancelled"); return o; }
            QFile::remove(r.absPath);
        }
    }
    g_undo.pop_back();
    ToolOutcome o;
    o.note = QStringLiteral("↶ reverted  %1").arg(r.rel);
    return o;
}

} // namespace tools
