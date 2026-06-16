#include "EditorPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSyntaxHighlighter>
#include <QRegularExpression>
#include <QFontDatabase>
#include <QTextDocument>
#include <QTextCharFormat>
#include <QColor>
#include <QVector>

// ---------------------------------------------------------------------------
// Highlighters (no Q_OBJECT needed — they only override highlightBlock).
// Colors come from the brand kit's diff / code-syntax palettes.
// ---------------------------------------------------------------------------
namespace {

class DiffHighlighter : public QSyntaxHighlighter {
public:
    using QSyntaxHighlighter::QSyntaxHighlighter;
protected:
    void highlightBlock(const QString& text) override {
        QTextCharFormat fmt;
        if (text.startsWith(QStringLiteral("@@"))) {
            fmt.setForeground(QColor(0x7D, 0xC7, 0xEC));            // accent
            fmt.setFontWeight(QFont::DemiBold);
        } else if (text.startsWith(QStringLiteral("+++")) || text.startsWith(QStringLiteral("---"))) {
            fmt.setForeground(QColor(0x85, 0x85, 0x85));            // muted header
        } else if (text.startsWith(QLatin1Char('+'))) {
            fmt.setForeground(QColor(0x5F, 0xD3, 0x5F));            // added (green)
            fmt.setBackground(QColor(0x1E, 0x2E, 0x1E));
        } else if (text.startsWith(QLatin1Char('-'))) {
            fmt.setForeground(QColor(0xF0, 0x68, 0x5B));            // removed (red)
            fmt.setBackground(QColor(0x33, 0x1E, 0x1E));
        } else {
            fmt.setForeground(QColor(0x9A, 0xB8, 0xCC));            // context
        }
        setFormat(0, text.length(), fmt);
    }
};

// A generic, language-agnostic code highlighter: comments, strings, numbers,
// and a common keyword set, using the brand syntax colors.
class CodeHighlighter : public QSyntaxHighlighter {
public:
    explicit CodeHighlighter(QTextDocument* doc) : QSyntaxHighlighter(doc) {
        QTextCharFormat kw; kw.setForeground(QColor(0x8E, 0xD0, 0xFF));
        const QStringList words = {
            "int","long","short","char","bool","void","float","double","auto","const","static",
            "class","struct","enum","namespace","template","typename","public","private","protected",
            "return","if","else","for","while","do","switch","case","break","continue","new","delete",
            "true","false","null","nullptr","none","None","True","False","def","function","var","let",
            "import","from","include","package","func","fn","pub","use","match","async","await","this","self"};
        for (const QString& w : words)
            rules_.push_back({QRegularExpression(QStringLiteral("\\b%1\\b").arg(w)), kw});

        QTextCharFormat num; num.setForeground(QColor(0x8E, 0xD0, 0xFF));
        rules_.push_back({QRegularExpression(QStringLiteral("\\b[0-9][0-9a-fA-FxX._]*\\b")), num});

        QTextCharFormat str; str.setForeground(QColor(0x9A, 0xB8, 0xCC));
        rules_.push_back({QRegularExpression(QStringLiteral("\"[^\"]*\"|'[^']*'")), str});

        cmt_.setForeground(QColor(0x4E, 0x7A, 0x96));
        cmtRe_ = QRegularExpression(QStringLiteral("//[^\n]*|#[^\n]*"));
    }
protected:
    void highlightBlock(const QString& text) override {
        for (const Rule& r : rules_) {
            auto it = r.re.globalMatch(text);
            while (it.hasNext()) {
                const auto m = it.next();
                setFormat(m.capturedStart(), m.capturedLength(), r.fmt);
            }
        }
        auto it = cmtRe_.globalMatch(text);
        while (it.hasNext()) { const auto m = it.next(); setFormat(m.capturedStart(), m.capturedLength(), cmt_); }
    }
private:
    struct Rule { QRegularExpression re; QTextCharFormat fmt; };
    QVector<Rule> rules_;
    QRegularExpression cmtRe_;
    QTextCharFormat cmt_;
};

} // namespace

// ---------------------------------------------------------------------------
EditorPanel::EditorPanel(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("EditorPanel"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("EditorHeader"));
    auto* hh = new QHBoxLayout(header);
    hh->setContentsMargins(14, 8, 8, 8);
    title_ = new QLabel(QStringLiteral("Workspace"));
    title_->setObjectName(QStringLiteral("EditorTitle"));
    hh->addWidget(title_, 1);
    closeBtn_ = new QPushButton(QStringLiteral("✕"));
    closeBtn_->setObjectName(QStringLiteral("EditorClose"));
    closeBtn_->setFixedSize(28, 28);
    closeBtn_->setCursor(Qt::PointingHandCursor);
    connect(closeBtn_, &QPushButton::clicked, this, &EditorPanel::closeRequested);
    hh->addWidget(closeBtn_);
    v->addWidget(header);

    view_ = new QPlainTextEdit;
    view_->setObjectName(QStringLiteral("EditorView"));
    view_->setReadOnly(true);
    view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    view_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    v->addWidget(view_, 1);

    actionBar_ = new QWidget;
    auto* ab = new QHBoxLayout(actionBar_);
    ab->setContentsMargins(14, 8, 14, 12);
    ab->addStretch(1);
    rejectBtn_ = new QPushButton(QStringLiteral("Reject"));
    rejectBtn_->setCursor(Qt::PointingHandCursor);
    connect(rejectBtn_, &QPushButton::clicked, this, &EditorPanel::rejectRequested);
    ab->addWidget(rejectBtn_);
    applyBtn_ = new QPushButton(QStringLiteral("Apply"));
    applyBtn_->setObjectName(QStringLiteral("Primary"));
    applyBtn_->setCursor(Qt::PointingHandCursor);
    connect(applyBtn_, &QPushButton::clicked, this, &EditorPanel::applyRequested);
    ab->addWidget(applyBtn_);
    v->addWidget(actionBar_);

    diffHl_ = new DiffHighlighter(static_cast<QTextDocument*>(nullptr));
    codeHl_ = new CodeHighlighter(nullptr);
}

void EditorPanel::useDiffHighlighter(bool on) {
    diffHl_->setDocument(on ? view_->document() : nullptr);
    codeHl_->setDocument(on ? nullptr : view_->document());
}

void EditorPanel::showDiff(const QString& title, const QString& diffText) {
    title_->setText(title);
    useDiffHighlighter(true);
    view_->setPlainText(diffText);
    actionBar_->show();
}

void EditorPanel::showFile(const QString& title, const QString& content, const QString&) {
    title_->setText(title);
    useDiffHighlighter(false);
    view_->setPlainText(content);
    actionBar_->hide();
}

void EditorPanel::markResolved(const QString& title) {
    title_->setText(title);
    actionBar_->hide();
}
