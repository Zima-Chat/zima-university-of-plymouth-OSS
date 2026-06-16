#pragma once
// One chat message: a small uppercase role label followed by the body rendered
// as Markdown (assistant models format replies in Markdown — headings, lists,
// bold/italic, inline code, links, tables, and fenced code blocks). The body is
// a word-wrapped QLabel showing Markdown-converted rich text, so it auto-sizes
// to its content height and the transcript (not the message) scrolls.

#include <QWidget>
#include <QString>

class QLabel;

class MessageWidget : public QWidget {
    Q_OBJECT
public:
    enum Role { User, Assistant, System };
    explicit MessageWidget(Role role, QWidget* parent = nullptr);

    void setText(const QString& text);
    QString text() const { return text_; }
    Role role() const { return role_; }

private:
    Role role_;
    QString text_;
    QLabel* body_ = nullptr;
};
