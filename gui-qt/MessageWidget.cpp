#include "MessageWidget.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QTextDocument>
#include <QPalette>

// Styles applied while converting Markdown → rich text. Qt's rich-text engine
// supports a subset of CSS; this covers the common Markdown output.
static QString documentCss() {
    return QStringLiteral(
        "a { color: #7DC7EC; text-decoration: none; }"
        "h1,h2,h3,h4,h5 { color: #E9E9E9; }"
        "code { background-color: #2C2C2C; color: #8ED0FF;"
        "       font-family: 'Consolas','CoFo Sans Mono',monospace; }"
        "pre { background-color: #2C2C2C; color: #7E8C9C;"
        "      font-family: 'Consolas','CoFo Sans Mono',monospace; }"
        "blockquote { color: #A3A3A3; }"
        "th,td { border: 1px solid #424242; padding: 2px 8px; }");
}

// Markdown (GitHub dialect) → HTML for display in a QLabel.
static QString markdownToHtml(const QString& md) {
    QTextDocument doc;
    doc.setDefaultStyleSheet(documentCss());
    doc.setMarkdown(md, QTextDocument::MarkdownDialectGitHub);
    return doc.toHtml();
}

MessageWidget::MessageWidget(Role role, QWidget* parent)
    : QWidget(parent), role_(role) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);

    auto* roleLabel = new QLabel(
        role_ == User ? QStringLiteral("YOU")
      : role_ == Assistant ? QStringLiteral("ZIMA")
      : QStringLiteral("SYSTEM"));
    roleLabel->setObjectName(role_ == User ? QStringLiteral("RoleUser")
                            : role_ == Assistant ? QStringLiteral("RoleAssistant")
                            : QStringLiteral("RoleSystem"));
    v->addWidget(roleLabel);

    body_ = new QLabel;
    body_->setObjectName(role_ == System ? QStringLiteral("MsgBodySystem")
                                         : QStringLiteral("MsgBody"));
    body_->setTextFormat(Qt::RichText);
    body_->setWordWrap(true);
    body_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    body_->setOpenExternalLinks(true);
    body_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    body_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    QPalette pal = body_->palette();
    pal.setColor(QPalette::Link, QColor(0x7D, 0xC7, 0xEC));
    body_->setPalette(pal);
    v->addWidget(body_);
}

void MessageWidget::setText(const QString& text) {
    text_ = text;
    body_->setText(markdownToHtml(text));
}
