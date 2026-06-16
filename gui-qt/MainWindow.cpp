#include "MainWindow.h"
#include "AgentClient.h"
#include "TranscriptView.h"
#include "MeshBackground.h"
#include "ComposerWidget.h"
#include "EditorPanel.h"
#include "Workspace.h"
#include "Tools.h"
#include "MemoryStore.h"

#include <QThread>
#include <QWidget>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QDialog>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QTreeView>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFontDatabase>
#include <QRegularExpression>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QSplitter>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include "WinChrome.h"

// ---------------------------------------------------------------------------
// Agentic loop helpers
// ---------------------------------------------------------------------------

// The tool protocol injected into every turn. The cloud model reliably emits
// single-object JSON tool calls when instructed this way (verified).
static QString toolPreamble() {
    const QString root = Workspace::instance().isOpen()
        ? Workspace::instance().root() : QStringLiteral("(no folder open)");
    return QStringLiteral(
"You are Zima, a coding assistant in a desktop app with access to the user's workspace folder through tools.\n"
"Workspace root: %1\n\n"
"To use a tool, reply with EXACTLY ONE JSON object and nothing else, e.g.\n"
"  {\"tool\":\"read_file\",\"path\":\"src/main.cpp\"}\n"
"Inside JSON strings, escape newlines as \\n and double quotes as \\\". Only one tool per reply.\n\n"
"Tools:\n"
"- read_file{path}: read a workspace file.\n"
"- list_dir{path}: list a directory (path \"\" or \".\" = root).\n"
"- search{pattern}: search file contents across the workspace.\n"
"- write_file{path,content}: create or overwrite a file. The user reviews a diff and must approve; the edit can be undone.\n"
"- delete_file{path}: delete a workspace file. The user must confirm; it can be undone.\n"
"- run_command{command}: run a shell command in the workspace. The user must approve.\n"
"- remember{note}: save a durable fact to long-term memory (persists across ALL chats). Use it when "
"the user states a lasting preference, project fact, or decision worth keeping.\n"
"- recall{query}: search your long-term memory and past conversations for relevant context.\n\n"
"Work step by step: inspect with read-only tools before writing. Use recall when the user refers to "
"earlier work or you need prior context. After each tool you receive its result and continue. "
"When the task is done, reply in plain text (NO JSON) as your final answer to the user.").arg(root);
}

// Extract the first balanced {...} object from s (ignoring braces in strings).
static QString extractJsonObject(const QString& s) {
    int start = s.indexOf(QLatin1Char('{'));
    if (start < 0) return {};
    int depth = 0; bool inStr = false;
    for (int i = start; i < s.size(); ++i) {
        const QChar c = s[i];
        if (inStr) {
            if (c == QLatin1Char('\\')) { ++i; continue; }
            if (c == QLatin1Char('"')) inStr = false;
        } else if (c == QLatin1Char('"')) inStr = true;
        else if (c == QLatin1Char('{')) ++depth;
        else if (c == QLatin1Char('}')) { if (--depth == 0) return s.mid(start, i - start + 1); }
    }
    return {};
}

// If `content` is (or begins with) a tool-call JSON object, parse it.
static bool parseToolCall(const QString& content, QString& name, QJsonObject& args) {
    QString s = content.trimmed();
    if (s.startsWith(QStringLiteral("```"))) {       // strip a ```json fence
        int nl = s.indexOf(QLatin1Char('\n'));
        if (nl >= 0) s = s.mid(nl + 1);
        if (s.endsWith(QStringLiteral("```"))) s.chop(3);
        s = s.trimmed();
    }
    if (!s.startsWith(QLatin1Char('{'))) return false;
    const QString obj = extractJsonObject(s);
    if (obj.isEmpty()) return false;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(obj.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject o = doc.object();
    if (!o.contains(QStringLiteral("tool"))) return false;
    name = o.value(QStringLiteral("tool")).toString();
    args = o;
    return !name.isEmpty();
}

// ---------------------------------------------------------------------------
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral(""));
    resize(1080, 720);

    auto* central = new MeshBackground;   // animated mesh layer behind everything
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildTopBar());

    // Middle: chat (transcript + composer) on the left, editor/diff panel on
    // the right in a resizable splitter (the panel is hidden until needed).
    transcript_ = new TranscriptView;
    auto* leftCol = new QWidget;
    auto* lv = new QVBoxLayout(leftCol);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->setSpacing(0);
    lv->addWidget(transcript_, 1);
    lv->addWidget(buildComposerArea());

    editor_ = new EditorPanel;
    editor_->hide();
    connect(editor_, &EditorPanel::applyRequested,  this, &MainWindow::onEditorApply);
    connect(editor_, &EditorPanel::rejectRequested, this, &MainWindow::onEditorReject);
    connect(editor_, &EditorPanel::closeRequested,  this, &MainWindow::onEditorClose);

    split_ = new QSplitter(Qt::Horizontal);
    split_->setObjectName(QStringLiteral("MainSplitter"));
    split_->setHandleWidth(1);
    split_->addWidget(leftCol);
    split_->addWidget(editor_);
    split_->setStretchFactor(0, 1);
    split_->setStretchFactor(1, 0);
    split_->setCollapsible(0, false);
    root->addWidget(split_, 1);

    setCentralWidget(central);
    applyDarkWindowChrome(winId());   // dark title bar, controls only

    // Left drawer (history + workspace + settings) overlaid on the content.
    buildDrawer(central);

    // ---- worker thread ----
    thread_ = new QThread(this);
    client_ = new AgentClient;
    client_->moveToThread(thread_);
    connect(thread_, &QThread::finished, client_, &QObject::deleteLater);

    connect(this, &MainWindow::requestComplete, client_, &AgentClient::requestComplete);
    connect(this, &MainWindow::requestTitle,   client_, &AgentClient::requestTitle);
    connect(this, &MainWindow::requestStatus, client_, &AgentClient::requestStatus);
    connect(this, &MainWindow::requestModels, client_, &AgentClient::requestModels);
    connect(this, &MainWindow::requestLogin,  client_, &AgentClient::configureCredential);

    connect(client_, &AgentClient::connected,     this, &MainWindow::onConnected);
    connect(client_, &AgentClient::disconnected,  this, &MainWindow::onDisconnected);
    connect(client_, &AgentClient::completeReply,  this, &MainWindow::onCompleteReply);
    connect(client_, &AgentClient::errorOccurred, this, &MainWindow::onError);
    connect(client_, &AgentClient::statusReceived,this, &MainWindow::onStatus);
    connect(client_, &AgentClient::modelsReceived,this, &MainWindow::onModels);
    connect(client_, &AgentClient::loginSucceeded,this, &MainWindow::onLoginSucceeded);
    connect(client_, &AgentClient::titleReady,    this, &MainWindow::onTitleReady);

    thread_->start();

    refreshHistory();
    newConversation();

    emit requestStatus();
    emit requestModels();
}

MainWindow::~MainWindow() {
    thread_->quit();
    thread_->wait(2000);
}

// Slim top bar: hamburger (opens the drawer) + the brand logo.
QWidget* MainWindow::buildTopBar() {
    auto* bar = new QWidget;
    bar->setObjectName(QStringLiteral("TopBar"));
    bar->setFixedHeight(52);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(12, 8, 16, 8);
    h->setSpacing(10);

    auto* burger = new QPushButton(QStringLiteral("☰"));
    burger->setObjectName(QStringLiteral("Hamburger"));
    burger->setFixedSize(36, 36);
    burger->setCursor(Qt::PointingHandCursor);
    burger->setToolTip(QStringLiteral("Menu"));
    connect(burger, &QPushButton::clicked, this, &MainWindow::toggleDrawer);
    h->addWidget(burger);

    auto* logo = new QLabel;
    {
        QPixmap pm(QStringLiteral(":/assets/Logo.png"));
        const int targetW = 96;
        const qreal dpr = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
        QPixmap scaled = pm.scaledToWidth(int(targetW * dpr), Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        logo->setPixmap(scaled);
    }
    h->addWidget(logo);
    h->addStretch(1);

    undoBtn_ = new QPushButton(QStringLiteral("↶  Undo edit"));
    undoBtn_->setObjectName(QStringLiteral("TopButton"));
    undoBtn_->setCursor(Qt::PointingHandCursor);
    undoBtn_->setToolTip(QStringLiteral("Revert the last applied file change"));
    undoBtn_->setEnabled(false);
    connect(undoBtn_, &QPushButton::clicked, this, &MainWindow::onUndo);
    h->addWidget(undoBtn_);
    return bar;
}

void MainWindow::updateUndoButton() {
    if (undoBtn_) undoBtn_->setEnabled(tools::canUndo());
}

void MainWindow::onUndo() {
    ToolOutcome o = tools::undoLast(this);
    if (!o.note.isEmpty())
        recordMessage(MessageWidget::System, o.note);
    updateUndoButton();
}

void MainWindow::openEditorPanel() {
    if (!editor_->isVisible()) {
        editor_->show();
        const int w = split_->width();
        split_->setSizes({int(w * 0.55), int(w * 0.45)});
    }
}

// Writes: new files (and any new folders) apply with no prompt; edits to an
// existing file show a diff in the side panel and pause the loop for approval.
void MainWindow::handleWriteCall(const QJsonObject& args) {
    const QString rel = args.value(QStringLiteral("path")).toString();
    const QString content = args.value(QStringLiteral("content")).toString();
    WritePreview pv = tools::previewWrite(rel, content);

    if (!pv.ok) {
        recordMessage(MessageWidget::System, pv.errNote);
        convo_ += QStringLiteral("TOOL RESULT (write_file): %1\n\n").arg(pv.errModel);
        startTurn();
        return;
    }

    if (!pv.existedBefore) {                       // brand-new file → auto-apply
        ToolOutcome out = tools::applyWrite(pv);
        recordMessage(MessageWidget::System, out.note);
        updateUndoButton();
        editor_->showFile(QStringLiteral("✚ %1").arg(rel), content, rel);
        openEditorPanel();
        convo_ += QStringLiteral("TOOL RESULT (write_file): %1\n\n").arg(out.resultForModel);
        startTurn();
        return;
    }

    // existing file → review in the panel; loop resumes on Apply/Reject
    pendingWrite_ = pv;
    hasPending_ = true;
    editor_->showDiff(QStringLiteral("Review change · %1  (+%2 −%3)")
                          .arg(rel).arg(pv.added).arg(pv.removed), pv.diff);
    openEditorPanel();
    recordMessage(MessageWidget::System,
        QStringLiteral("✎ proposed an edit to **%1** — review it in the panel →").arg(rel));
}

void MainWindow::onEditorApply() {
    if (!hasPending_) return;
    hasPending_ = false;
    ToolOutcome out = tools::applyWrite(pendingWrite_);
    recordMessage(MessageWidget::System, out.note);
    updateUndoButton();
    editor_->markResolved(QStringLiteral("Applied · %1").arg(pendingWrite_.rel));
    convo_ += QStringLiteral("TOOL RESULT (write_file): %1\n\n").arg(out.resultForModel);
    startTurn();
}

void MainWindow::onEditorReject() {
    if (!hasPending_) return;
    hasPending_ = false;
    recordMessage(MessageWidget::System,
                  QStringLiteral("✗ rejected change · %1").arg(pendingWrite_.rel));
    editor_->markResolved(QStringLiteral("Rejected · %1").arg(pendingWrite_.rel));
    convo_ += QStringLiteral("TOOL RESULT (write_file): The user rejected the change to '%1'. "
                             "Do not retry it without a different approach.\n\n").arg(pendingWrite_.rel);
    startTurn();
}

void MainWindow::onEditorClose() {
    if (hasPending_) onEditorReject();   // closing a pending review counts as reject
    editor_->hide();
}

// Composer pill (with embedded send button) plus the model picker beneath it,
// both constrained to a centered reading column.
QWidget* MainWindow::buildComposerArea() {
    auto* area = new QWidget;
    auto* outer = new QHBoxLayout(area);
    outer->setContentsMargins(28, 6, 28, 18);
    outer->addStretch(1);

    auto* col = new QWidget;
    col->setMaximumWidth(760);
    col->setMinimumWidth(360);
    auto* cv = new QVBoxLayout(col);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(6);

    // Folder selector + model picker, side by side, above the input.
    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(2, 0, 0, 0);
    topRow->setSpacing(10);
    folderBtn_ = new QPushButton(QStringLiteral("📁  Open folder…"));
    folderBtn_->setObjectName(QStringLiteral("FolderButton"));
    folderBtn_->setCursor(Qt::PointingHandCursor);
    connect(folderBtn_, &QPushButton::clicked, this, &MainWindow::onOpenFolder);
    topRow->addWidget(folderBtn_);

    models_ = new QComboBox;
    models_->setObjectName(QStringLiteral("ModelSelect"));
    models_->setMinimumWidth(280);
    models_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    topRow->addWidget(models_);
    topRow->addStretch(1);
    cv->addLayout(topRow);

    composer_ = new ComposerWidget;
    connect(composer_, &ComposerWidget::send, this, &MainWindow::onSend);
    cv->addWidget(composer_);

    outer->addWidget(col, 6);
    outer->addStretch(1);
    return area;
}

// The slide-out left drawer: New chat, conversation history, workspace folder &
// file tree, status, and Settings. Built as an overlay child of `overlayParent`.
void MainWindow::buildDrawer(QWidget* overlayParent) {
    scrim_ = new QPushButton(overlayParent);
    scrim_->setObjectName(QStringLiteral("Scrim"));
    scrim_->setCursor(Qt::ArrowCursor);
    scrim_->hide();
    connect(scrim_, &QPushButton::clicked, this, &MainWindow::closeDrawer);

    drawer_ = new QFrame(overlayParent);
    drawer_->setObjectName(QStringLiteral("Drawer"));
    drawer_->setFixedWidth(kDrawerWidth);
    auto* v = new QVBoxLayout(drawer_);
    v->setContentsMargins(18, 18, 18, 18);
    v->setSpacing(10);

    auto* newChat = new QPushButton(QStringLiteral("＋  New chat"));
    newChat->setObjectName(QStringLiteral("DrawerPrimary"));
    connect(newChat, &QPushButton::clicked, this, [this] { onNewChat(); closeDrawer(); });
    v->addWidget(newChat);

    v->addSpacing(8);
    auto* histLabel = new QLabel(QStringLiteral("HISTORY"));
    histLabel->setObjectName(QStringLiteral("SectionLabel"));
    v->addWidget(histLabel);
    history_ = new QListWidget;
    history_->setObjectName(QStringLiteral("HistoryList"));
    history_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(history_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        onHistoryActivated(it); closeDrawer();
    });
    connect(history_, &QListWidget::customContextMenuRequested, this, &MainWindow::onHistoryMenu);
    v->addWidget(history_, 3);

    v->addSpacing(6);
    auto* wsLabel = new QLabel(QStringLiteral("FILES"));
    wsLabel->setObjectName(QStringLiteral("SectionLabel"));
    v->addWidget(wsLabel);

    fsModel_ = new QFileSystemModel(this);
    tree_ = new QTreeView;
    tree_->setObjectName(QStringLiteral("FileTree"));
    tree_->setModel(fsModel_);
    tree_->setHeaderHidden(true);
    for (int c = 1; c < fsModel_->columnCount(); ++c) tree_->hideColumn(c);
    tree_->setRootIsDecorated(true);
    connect(tree_, &QTreeView::doubleClicked, this, &MainWindow::onFileActivated);
    v->addWidget(tree_, 4);

    auto* footer = new QWidget;
    auto* fh = new QHBoxLayout(footer);
    fh->setContentsMargins(0, 0, 0, 0);
    fh->setSpacing(8);
    statusDot_ = new QLabel;
    statusDot_->setFixedSize(9, 9);
    fh->addWidget(statusDot_);
    statusLabel_ = new QLabel(QStringLiteral("connecting…"));
    statusLabel_->setObjectName(QStringLiteral("StatusLabel"));
    fh->addWidget(statusLabel_, 1);
    auto* settings = new QPushButton(QStringLiteral("⚙  Settings"));
    connect(settings, &QPushButton::clicked, this, [this] { onSettings(); });
    fh->addWidget(settings);
    v->addWidget(footer);

    drawer_->move(-kDrawerWidth, 0);
    drawerAnim_ = new QPropertyAnimation(drawer_, "pos", this);
    drawerAnim_->setDuration(220);
    drawerAnim_->setEasingCurve(QEasingCurve::OutCubic);

    updateStatusLabel();
    layoutOverlay();
}

void MainWindow::layoutOverlay() {
    QWidget* c = centralWidget();
    if (!c) return;
    if (scrim_) scrim_->setGeometry(c->rect());
    if (drawer_) {
        drawer_->resize(kDrawerWidth, c->height());
        if (drawerAnim_ && drawerAnim_->state() != QAbstractAnimation::Running)
            drawer_->move(drawerOpen_ ? 0 : -kDrawerWidth, 0);
    }
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    layoutOverlay();
}

void MainWindow::toggleDrawer() { drawerOpen_ ? closeDrawer() : openDrawer(); }

void MainWindow::openDrawer() {
    if (drawerOpen_) return;
    drawerOpen_ = true;
    layoutOverlay();
    scrim_->show();
    scrim_->raise();
    drawer_->raise();
    drawerAnim_->stop();
    drawerAnim_->setStartValue(drawer_->pos());
    drawerAnim_->setEndValue(QPoint(0, 0));
    drawerAnim_->start();
}

void MainWindow::closeDrawer() {
    if (!drawerOpen_) return;
    drawerOpen_ = false;
    drawerAnim_->stop();
    drawerAnim_->setStartValue(drawer_->pos());
    drawerAnim_->setEndValue(QPoint(-kDrawerWidth, 0));
    drawerAnim_->start();
    // hide the scrim once the slide-out finishes
    disconnect(drawerAnim_, &QPropertyAnimation::finished, nullptr, nullptr);
    connect(drawerAnim_, &QPropertyAnimation::finished, this, [this] {
        if (!drawerOpen_ && scrim_) scrim_->hide();
    });
}

void MainWindow::onSend() {
    const QString text = composer_->text().trimmed();
    if (text.isEmpty()) return;
    composer_->clear();

    if (current_.title.isEmpty())                 // title = first user message
        current_.title = text.simplified().left(48);
    recordMessage(MessageWidget::User, text);

    convo_ += QStringLiteral("USER: %1\n\n").arg(text);
    currentModel_ = models_->currentText();
    toolRounds_ = 0;
    startTurn();
}

void MainWindow::startTurn() {
    transcript_->showThinking();
    emit requestComplete(buildPrompt(), currentModel_);
}

QString MainWindow::buildPrompt() const {
    QString memory;
    const QString notes = MemoryStore::instance().digest();
    if (!notes.isEmpty())
        memory += QStringLiteral("\n\n=== Long-term memory ===\n") + notes;

    // Make the agent aware of other saved chats it can recall (titles only;
    // it fetches detail on demand via the recall tool — a lightweight context loop).
    QStringList recent;
    for (const ConversationMeta& m : store_.list()) {
        if (m.id == current_.id || m.title.isEmpty()) continue;
        recent << QStringLiteral("- %1 (%2)").arg(m.title,
                    m.updated.toString(QStringLiteral("yyyy-MM-dd")));
        if (recent.size() >= 8) break;
    }
    if (!recent.isEmpty())
        memory += QStringLiteral("\n\n=== Other saved conversations (use recall to read) ===\n")
                + recent.join(QLatin1Char('\n'));

    return toolPreamble() + memory
         + QStringLiteral("\n\n=== Conversation so far ===\n") + convo_
         + QStringLiteral("\nContinue. If you need a tool, reply with ONE JSON object; "
                          "otherwise give your final answer in plain text.");
}

void MainWindow::onNewChat() {
    saveCurrent();
    newConversation();
    refreshHistory();
}

// ---- conversation history (local only) ----
QString MainWindow::titlePrompt() const {
    return QStringLiteral(
        "Generate a concise title (3 to 6 words, Title Case) for the following conversation. "
        "Reply with ONLY the title — no quotes, no trailing punctuation, no preamble.\n\n%1")
        .arg(convo_.left(2000));
}

void MainWindow::onTitleReady(const QString& raw) {
    QString t = raw.trimmed();
    t = t.section(QLatin1Char('\n'), 0, 0).trimmed();   // first line only
    t.remove(QLatin1Char('"'));
    t = t.trimmed();
    if (t.isEmpty()) return;
    if (t.size() > 60) t = t.left(60);
    current_.title = t;
    saveCurrent();
    refreshHistory();
}

void MainWindow::newConversation() {
    current_ = Conversation{};
    current_.id = ConversationStore::newId();
    current_.created = current_.updated = QDateTime::currentDateTime();
    convo_.clear();
    toolRounds_ = 0;
    titleGenerated_ = false;
    transcript_->clear();
    // greeting is shown but not persisted (it isn't part of the conversation)
    transcript_->addMessage(MessageWidget::System,
        QStringLiteral("Connected to the Zima agent over a local encrypted channel. "
        "Open a folder to give me file & command access, pick a model, and tell me what to "
        "build. I can read, search, write (you review a diff), and run commands (you approve). "
        "Conversations are saved locally on this machine only. Enter sends · Shift+Enter for a newline."));
}

void MainWindow::recordMessage(int role, const QString& text) {
    transcript_->addMessage(static_cast<MessageWidget::Role>(role), text);
    current_.messages.append({role, text});
    current_.updated = QDateTime::currentDateTime();
    saveCurrent();
}

void MainWindow::saveCurrent() {
    if (current_.title.isEmpty()) return; // nothing meaningful to save yet
    current_.context = convo_;
    store_.save(current_);
}

void MainWindow::refreshHistory() {
    if (!history_) return;
    history_->clear();
    for (const ConversationMeta& m : store_.list()) {
        auto* item = new QListWidgetItem(
            m.title.isEmpty() ? QStringLiteral("(untitled)") : m.title);
        item->setData(Qt::UserRole, m.id);
        item->setToolTip(m.updated.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        history_->addItem(item);
    }
}

void MainWindow::loadConversation(const QString& id) {
    Conversation c = store_.load(id);
    if (c.id.isEmpty()) return;
    saveCurrent();              // preserve the one we're leaving
    current_ = c;
    convo_ = c.context;
    toolRounds_ = 0;
    titleGenerated_ = true;   // a saved chat already has its title
    transcript_->clear();
    for (const StoredMessage& m : c.messages)
        transcript_->addMessage(static_cast<MessageWidget::Role>(m.role), m.text);
}

void MainWindow::onHistoryActivated(QListWidgetItem* item) {
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty() || id == current_.id) return;
    loadConversation(id);
}

void MainWindow::onHistoryMenu(const QPoint& pos) {
    QListWidgetItem* item = history_->itemAt(pos);
    if (!item) return;
    const QString id = item->data(Qt::UserRole).toString();
    QMenu menu(this);
    QAction* del = menu.addAction(QStringLiteral("Delete conversation"));
    if (menu.exec(history_->mapToGlobal(pos)) == del) {
        store_.remove(id);
        if (id == current_.id) newConversation();
        refreshHistory();
    }
}

void MainWindow::onSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Zima — Settings"));
    dlg.setMinimumWidth(420);
    auto* v = new QVBoxLayout(&dlg);
    v->setContentsMargins(20, 18, 20, 18);
    v->setSpacing(10);

    auto* heading = new QLabel(QStringLiteral("API KEY"));
    heading->setObjectName(QStringLiteral("SectionLabel"));
    v->addWidget(heading);

    const QString state = !connected_ ? QStringLiteral("agent: disconnected")
                        : locked_ ? QStringLiteral("agent: connected · locked (no key)")
                                  : QStringLiteral("agent: connected · ready (%1)").arg(statusModel_);
    auto* status = new QLabel(state);
    status->setObjectName(QStringLiteral("StatusLabel"));
    v->addWidget(status);

    auto* hint = new QLabel(QStringLiteral(
        "Your key is sent once to the local agent, which stores it in the OS secret store. "
        "This window never writes it to disk."));
    hint->setObjectName(QStringLiteral("StatusLabel"));
    hint->setWordWrap(true);
    v->addWidget(hint);

    auto* edit = new QLineEdit;
    edit->setEchoMode(QLineEdit::Password);
    edit->setPlaceholderText(QStringLiteral("Zima API key"));
    v->addWidget(edit);

    // ---- memory (clearable local cache) ----
    v->addSpacing(8);
    auto* memHeading = new QLabel;
    memHeading->setObjectName(QStringLiteral("SectionLabel"));
    v->addWidget(memHeading);
    auto* memList = new QListWidget;
    memList->setObjectName(QStringLiteral("HistoryList"));
    memList->setMaximumHeight(140);
    v->addWidget(memList);

    auto refreshMem = [memHeading, memList] {
        memHeading->setText(QStringLiteral("MEMORY (%1 notes)").arg(MemoryStore::instance().count()));
        memList->clear();
        for (const MemoryNote& n : MemoryStore::instance().all())
            memList->addItem(n.text);
    };
    refreshMem();

    auto* clearBtn = new QPushButton(QStringLiteral("Clear memory"));
    connect(clearBtn, &QPushButton::clicked, &dlg, [this, &dlg, refreshMem] {
        if (MemoryStore::instance().count() == 0) return;
        if (QMessageBox::question(&dlg, QStringLiteral("Clear memory"),
                QStringLiteral("Delete all %1 remembered notes? This can't be undone.")
                    .arg(MemoryStore::instance().count())) == QMessageBox::Yes) {
            MemoryStore::instance().clear();
            refreshMem();
        }
    });
    v->addWidget(clearBtn);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(edit, &QLineEdit::returnPressed, &dlg, &QDialog::accept);

    if (dlg.exec() == QDialog::Accepted && !edit->text().isEmpty())
        emit requestLogin(edit->text());
}

// ---- worker signals ----
void MainWindow::onConnected() {
    connected_ = true;
    updateStatusLabel();
}

void MainWindow::onDisconnected(const QString& reason) {
    connected_ = false;
    transcript_->hideThinking();
    transcript_->addMessage(MessageWidget::System,
                            QStringLiteral("Disconnected: %1").arg(reason));
    updateStatusLabel();
}

// The heart of the agentic loop: each model reply is either a tool call (run it,
// feed the result back, ask again) or the final plain-text answer.
void MainWindow::onCompleteReply(const QString& content) {
    transcript_->hideThinking();

    QString name; QJsonObject args;
    constexpr int kMaxToolRounds = 24;
    if (toolRounds_ < kMaxToolRounds && parseToolCall(content, name, args)) {
        ++toolRounds_;
        convo_ += QStringLiteral("ASSISTANT (tool call): %1\n").arg(content.trimmed());

        // Writes go through the side panel: new files apply automatically, edits
        // to existing files pause the loop for the user's Apply/Reject.
        if (name == QStringLiteral("write_file") || name == QStringLiteral("create_file")) {
            handleWriteCall(args);
            return;
        }

        ToolOutcome out = tools::run(this, name, args);
        QString chip = out.note;
        if (!out.block.isEmpty()) chip += QLatin1Char('\n') + out.block;
        recordMessage(MessageWidget::System, chip);
        updateUndoButton();

        convo_ += QStringLiteral("TOOL RESULT (%1): %2\n\n").arg(name, out.resultForModel);
        startTurn();                       // continue the loop
        return;
    }

    if (toolRounds_ >= kMaxToolRounds)
        transcript_->addMessage(MessageWidget::System,
            QStringLiteral("Stopped after %1 tool steps. Send another message to continue.")
                .arg(toolRounds_));

    const QString ans = content.trimmed();
    if (!ans.isEmpty()) {
        recordMessage(MessageWidget::Assistant, ans);
        convo_ += QStringLiteral("ASSISTANT: %1\n\n").arg(ans);
        // Let the model name the conversation once, after the first real reply.
        if (!titleGenerated_) {
            titleGenerated_ = true;
            emit requestTitle(titlePrompt(), currentModel_);
        }
    }
}

void MainWindow::onError(const QString& message) {
    transcript_->hideThinking();
    transcript_->addMessage(MessageWidget::System, QStringLiteral("Error: %1").arg(message));
}

// ---- workspace ----
void MainWindow::onOpenFolder() {
    const QString start = Workspace::instance().isOpen()
        ? Workspace::instance().root() : QDir::homePath();
    const QString dir = QFileDialog::getExistingDirectory(this,
        QStringLiteral("Open workspace folder"), start);
    if (dir.isEmpty()) return;
    setWorkspacePath(dir);
}

void MainWindow::setWorkspacePath(const QString& dir) {
    Workspace::instance().setRoot(dir);
    fsModel_->setRootPath(dir);
    tree_->setRootIndex(fsModel_->index(dir));
    if (folderBtn_)
        folderBtn_->setText(QStringLiteral("📁  %1").arg(QDir(dir).dirName()));
    transcript_->addMessage(MessageWidget::System,
        QStringLiteral("Workspace opened: %1\nI can now read, search, write, and run commands here.")
            .arg(QDir::toNativeSeparators(dir)));
}

void MainWindow::onFileActivated(const QModelIndex& index) {
    if (!fsModel_ || fsModel_->isDir(index)) return;
    const QString path = fsModel_->filePath(index);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    const QString rel = QDir(Workspace::instance().root()).relativeFilePath(path);
    editor_->showFile(rel, QString::fromUtf8(f.readAll()), rel);
    openEditorPanel();
    closeDrawer();
}

void MainWindow::onStatus(bool locked, const QString& model) {
    connected_ = true;
    locked_ = locked;
    statusModel_ = model;
    updateStatusLabel();
}

void MainWindow::onModels(const QString& json) {
    // Pull each "id":"..." out of the models JSON (shape varies; a scan is robust).
    models_->clear();
    static const QRegularExpression re(QStringLiteral("\"id\"\\s*:\\s*\"([^\"]+)\""));
    auto it = re.globalMatch(json);
    while (it.hasNext())
        models_->addItem(it.next().captured(1));
    if (models_->count() > 0) models_->setCurrentIndex(0);
}

void MainWindow::onLoginSucceeded() {
    transcript_->addMessage(MessageWidget::System,
                            QStringLiteral("API key saved. You're ready to chat."));
    emit requestStatus();
}

void MainWindow::updateStatusLabel() {
    const char* dot = !connected_ ? "#E06A5E"      // error red
                    : locked_     ? "#E2B14F"      // warning amber
                                  : "#3FC18E";     // success green
    statusDot_->setStyleSheet(
        QStringLiteral("background-color: %1; border-radius: 4px;").arg(QLatin1String(dot)));
    QString text = !connected_ ? QStringLiteral("disconnected")
                 : locked_ ? QStringLiteral("locked · %1").arg(statusModel_)
                           : QStringLiteral("ready · %1").arg(statusModel_);
    statusLabel_->setText(text);
}
