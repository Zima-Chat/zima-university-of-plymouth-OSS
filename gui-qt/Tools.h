#pragma once
// ---------------------------------------------------------------------------
// Tools — the coding-assistant's local capabilities, sandboxed to the open
// workspace folder. Executed on the GUI thread (they show approval dialogs and
// run processes). Each call returns text fed back to the model plus a concise
// note (and optional code/diff/output block) shown in the transcript.
//
// Read-only tools (read_file, list_dir, search) run without prompting.
// Mutating/▶ tools (write_file/create_file, run_command) require the user to
// approve a diff or the command before anything happens.
// ---------------------------------------------------------------------------

#include <QString>
#include <QJsonObject>

class QWidget;

struct ToolOutcome {
    QString resultForModel;  // fed back into the conversation for the next turn
    QString note;            // one-line system chip for the transcript
    QString block;           // optional markdown block (code/diff/output) to show
    bool ok = true;
};

// A computed (but not yet applied) file write — lets the UI preview the diff in
// a panel and apply asynchronously after the user approves.
struct WritePreview {
    bool ok = false;
    bool existedBefore = false;
    QString abs, rel, oldText, content, diff;
    QString errNote, errModel;     // populated when ok == false
    int added = 0, removed = 0;
};

namespace tools {

// Run a tool by name with JSON args. `parent` anchors any approval dialogs.
ToolOutcome run(QWidget* parent, const QString& name, const QJsonObject& args);

// Write pipeline split so edits can be approved in a non-modal side panel:
//   previewWrite() resolves + diffs without touching disk;
//   applyWrite()   performs the write and records it for undo.
WritePreview previewWrite(const QString& rel, const QString& content);
ToolOutcome applyWrite(const WritePreview& pv);

// Reversible edits: every applied write/delete is backed up. canUndo() reports
// whether anything can be reverted; undoLast() shows the reverting diff for the
// user to accept or decline, then restores the previous file state.
bool canUndo();
ToolOutcome undoLast(QWidget* parent);

} // namespace tools
