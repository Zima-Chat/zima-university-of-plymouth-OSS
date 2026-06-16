#pragma once
// Scrolling chat transcript: a centered reading column of MessageWidgets.
// Keeps the view pinned to the newest content while streaming, unless the user
// has scrolled up to read history.

#include <QScrollArea>
#include "MessageWidget.h"

class QVBoxLayout;

class TranscriptView : public QScrollArea {
    Q_OBJECT
public:
    explicit TranscriptView(QWidget* parent = nullptr);

    void addMessage(MessageWidget::Role role, const QString& text);
    void beginAssistant();                       // start a new streaming turn
    void appendAssistant(const QString& fullText);// replace current turn's text
    void showThinking();                         // animated "Thinking…" placeholder
    void hideThinking();
    void clear();

private:
    bool atBottom() const;
    void scrollToBottomLater();

    QWidget* inner_ = nullptr;
    QVBoxLayout* col_ = nullptr;
    MessageWidget* current_ = nullptr; // the streaming assistant message, if any
    QWidget* thinking_ = nullptr;      // transient "Thinking…" indicator
};
