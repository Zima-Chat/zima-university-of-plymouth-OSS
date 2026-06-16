#pragma once
// A closable right-hand panel for a "real workspace" feel: it shows proposed
// diffs (with Apply/Reject inline — no popup) and file contents, with diff and
// light code syntax highlighting. Hidden until there's something to show.

#include <QFrame>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSyntaxHighlighter;

class EditorPanel : public QFrame {
    Q_OBJECT
public:
    explicit EditorPanel(QWidget* parent = nullptr);

    void showDiff(const QString& title, const QString& diffText);     // with Apply/Reject
    void showFile(const QString& title, const QString& content, const QString& name);
    void markResolved(const QString& title);   // diff applied/rejected → drop the buttons

signals:
    void applyRequested();
    void rejectRequested();
    void closeRequested();

private:
    void useDiffHighlighter(bool on);

    QLabel* title_ = nullptr;
    QPushButton* closeBtn_ = nullptr;
    QPlainTextEdit* view_ = nullptr;
    QWidget* actionBar_ = nullptr;
    QPushButton* applyBtn_ = nullptr;
    QPushButton* rejectBtn_ = nullptr;
    QSyntaxHighlighter* diffHl_ = nullptr;
    QSyntaxHighlighter* codeHl_ = nullptr;
};
