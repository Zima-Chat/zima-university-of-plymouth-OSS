#include "TranscriptView.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QTimer>
#include <QLabel>
#include <memory>

TranscriptView::TranscriptView(QWidget* parent) : QScrollArea(parent) {
    setObjectName(QStringLiteral("Transcript"));
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    // Let the animated mesh background show through the transcript.
    viewport()->setObjectName(QStringLiteral("TranscriptViewport"));
    viewport()->setAutoFillBackground(false);

    inner_ = new QWidget;
    inner_->setObjectName(QStringLiteral("TranscriptInner"));

    // Center the reading column (max ~760px) with comfortable margins.
    auto* outer = new QHBoxLayout(inner_);
    outer->setContentsMargins(28, 28, 28, 96);
    outer->addStretch(1);

    auto* column = new QWidget;
    column->setObjectName(QStringLiteral("TranscriptInner"));
    column->setMaximumWidth(760);
    column->setMinimumWidth(360);
    col_ = new QVBoxLayout(column);
    col_->setContentsMargins(0, 0, 0, 0);
    col_->setSpacing(24);
    col_->addStretch(1); // keeps messages top-aligned until they fill the view

    outer->addWidget(column, /*stretch*/ 6);
    outer->addStretch(1);

    setWidget(inner_);
}

bool TranscriptView::atBottom() const {
    auto* bar = verticalScrollBar();
    return bar->value() >= bar->maximum() - 8;
}

void TranscriptView::scrollToBottomLater() {
    // Defer until after layout has settled so maximum() reflects new content.
    QTimer::singleShot(0, this, [this] {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    });
}

void TranscriptView::addMessage(MessageWidget::Role role, const QString& text) {
    bool stick = atBottom();
    auto* m = new MessageWidget(role);
    m->setText(text);
    // insert before the trailing stretch
    col_->insertWidget(col_->count() - 1, m);
    if (role != MessageWidget::Assistant) current_ = nullptr;
    if (stick) scrollToBottomLater();
}

void TranscriptView::beginAssistant() {
    hideThinking();
    bool stick = atBottom();
    current_ = new MessageWidget(MessageWidget::Assistant);
    col_->insertWidget(col_->count() - 1, current_);
    if (stick) scrollToBottomLater();
}

void TranscriptView::showThinking() {
    if (thinking_) return;
    bool stick = atBottom();

    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);

    auto* role = new QLabel(QStringLiteral("ZIMA"));
    role->setObjectName(QStringLiteral("RoleAssistant"));
    v->addWidget(role);

    auto* body = new QLabel(QStringLiteral("Thinking"));
    body->setObjectName(QStringLiteral("Thinking"));
    v->addWidget(body);

    // cycle the trailing dots: Thinking → Thinking. → .. → …
    auto counter = std::make_shared<int>(0);
    auto* timer = new QTimer(w);
    connect(timer, &QTimer::timeout, body, [body, counter] {
        int n = (*counter)++ % 4;
        body->setText(QStringLiteral("Thinking") + QString(n, QLatin1Char('.')));
    });
    timer->start(380);

    thinking_ = w;
    col_->insertWidget(col_->count() - 1, w);
    if (stick) scrollToBottomLater();
}

void TranscriptView::hideThinking() {
    if (!thinking_) return;
    col_->removeWidget(thinking_);
    thinking_->deleteLater();
    thinking_ = nullptr;
}

void TranscriptView::appendAssistant(const QString& fullText) {
    if (!current_) beginAssistant();
    bool stick = atBottom();
    current_->setText(fullText);
    if (stick) scrollToBottomLater();
}

void TranscriptView::clear() {
    QLayoutItem* item;
    // remove everything except the trailing stretch
    while (col_->count() > 1) {
        item = col_->takeAt(0);
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }
    current_ = nullptr;
    thinking_ = nullptr;
}
