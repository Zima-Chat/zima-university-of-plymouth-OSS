#include "ComposerWidget.h"

#include <QTextEdit>
#include <QPushButton>
#include <QKeyEvent>
#include <QtGlobal>

ComposerWidget::ComposerWidget(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("Composer"));

    edit_ = new QTextEdit(this);
    edit_->setObjectName(QStringLiteral("ComposerEdit"));
    edit_->setFrameShape(QFrame::NoFrame);
    edit_->setAcceptRichText(false);
    edit_->setPlaceholderText(
        QStringLiteral("Message Zima — ask a question or describe code to build…"));
    edit_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    edit_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    edit_->document()->setDocumentMargin(2);
    edit_->installEventFilter(this);
    connect(edit_, &QTextEdit::textChanged, this, [this] { updateHeight(); });

    btn_ = new QPushButton(QStringLiteral("↑"), this);
    btn_->setObjectName(QStringLiteral("SendButton"));
    btn_->setFixedSize(34, 34);
    btn_->setCursor(Qt::PointingHandCursor);
    btn_->setToolTip(QStringLiteral("Send  (Enter)"));
    connect(btn_, &QPushButton::clicked, this, &ComposerWidget::send);

    setFixedHeight(minH_);
}

QString ComposerWidget::text() const { return edit_->toPlainText(); }
void ComposerWidget::clear() { edit_->clear(); }              // textChanged → reshrinks
void ComposerWidget::focusInput() { edit_->setFocus(); }
void ComposerWidget::setEnabledSend(bool on) { btn_->setEnabled(on); }

void ComposerWidget::relayout() {
    const int W = width(), H = height();
    const int leftPad = 16;
    const int rightReserve = 50;     // room for the circular send button
    const int topPad = 6;
    edit_->setGeometry(leftPad, topPad, qMax(10, W - leftPad - rightReserve), H - 2 * topPad);
    edit_->document()->setTextWidth(edit_->width());
    btn_->move(W - btn_->width() - 5, (H - btn_->height()) / 2);  // vertically centered
}

void ComposerWidget::updateHeight() {
    edit_->document()->setTextWidth(edit_->width());
    const int contentH = int(edit_->document()->size().height()) + 12; // + chrome
    const int h = qBound(minH_, contentH, maxH_);
    edit_->setVerticalScrollBarPolicy(
        contentH > maxH_ ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    if (h != height()) setFixedHeight(h);  // guard re-entrant resize
    relayout();
}

void ComposerWidget::resizeEvent(QResizeEvent* e) {
    QFrame::resizeEvent(e);
    relayout();
}

bool ComposerWidget::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == edit_ && ev->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) &&
            !(ke->modifiers() & Qt::ShiftModifier)) {
            emit send();
            return true; // Shift+Enter falls through to insert a newline
        }
    }
    return QFrame::eventFilter(obj, ev);
}
