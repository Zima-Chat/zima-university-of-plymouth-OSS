#pragma once
// The chat input: a rounded "pill" frame containing a borderless text edit with
// a circular ↑ send button floated in its bottom-right corner (Claude-style).
// Enter sends; Shift+Enter inserts a newline.

#include <QFrame>

class QTextEdit;
class QPushButton;

class ComposerWidget : public QFrame {
    Q_OBJECT
public:
    explicit ComposerWidget(QWidget* parent = nullptr);

    QString text() const;
    void clear();
    void focusInput();
    void setEnabledSend(bool on);

signals:
    void send();

protected:
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void relayout();      // position the editor + button
    void updateHeight();  // grow the pill to fit the text (clamped)

    QTextEdit* edit_ = nullptr;
    QPushButton* btn_ = nullptr;
    int minH_ = 40;       // single-line pill, matches the send button height
    int maxH_ = 200;      // then it scrolls
};
