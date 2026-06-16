#pragma once
// The Zima chat window, laid out like Claude: a slim top bar with a hamburger,
// a centered transcript, and a composer with an embedded send button plus the
// model picker beneath it. The hamburger slides out a left drawer holding the
// local conversation history, the workspace folder/file-tree, and Settings.
// Owns the AgentClient worker thread and runs the agentic tool loop.

#include <QMainWindow>
#include <QString>
#include "ConversationStore.h"
#include "Tools.h"

class QThread;
class QComboBox;
class QLabel;
class QPushButton;
class QTreeView;
class QFileSystemModel;
class QModelIndex;
class QListWidget;
class QListWidgetItem;
class QPoint;
class QJsonObject;
class QFrame;
class QPropertyAnimation;
class QSplitter;
class AgentClient;
class TranscriptView;
class ComposerWidget;
class EditorPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    void requestComplete(const QString& prompt, const QString& model);
    void requestTitle(const QString& prompt, const QString& model);
    void requestStatus();
    void requestModels();
    void requestLogin(const QString& apiKey);

protected:
    void resizeEvent(QResizeEvent* e) override;

private slots:
    void onSend();
    void onNewChat();
    void onSettings();
    void onOpenFolder();
    void onFileActivated(const QModelIndex& index);
    void onHistoryActivated(QListWidgetItem* item);
    void onHistoryMenu(const QPoint& pos);
    void toggleDrawer();
    void onUndo();
    void onEditorApply();
    void onEditorReject();
    void onEditorClose();
    void onConnected();
    void onDisconnected(const QString& reason);
    void onCompleteReply(const QString& content);
    void onError(const QString& message);
    void onStatus(bool locked, const QString& model);
    void onModels(const QString& json);
    void onLoginSucceeded();
    void onTitleReady(const QString& title);

private:
    QWidget* buildTopBar();
    QWidget* buildComposerArea();
    void buildDrawer(QWidget* overlayParent);
    void layoutOverlay();
    void openDrawer();
    void closeDrawer();
    void startTurn();                 // ask the model for its next action
    QString buildPrompt() const;      // tool preamble + conversation
    void handleWriteCall(const QJsonObject& args);  // async write w/ panel preview
    void openEditorPanel();
    void updateStatusLabel();
    void updateUndoButton();
    void setWorkspacePath(const QString& dir);

    // conversation history (local only)
    void newConversation();
    void recordMessage(int role, const QString& text);  // show + persist
    void saveCurrent();
    void refreshHistory();
    void loadConversation(const QString& id);
    QString titlePrompt() const;      // ask the model to name the conversation

    // worker
    QThread* thread_ = nullptr;
    AgentClient* client_ = nullptr;

    // widgets
    TranscriptView* transcript_ = nullptr;
    QComboBox* models_ = nullptr;
    ComposerWidget* composer_ = nullptr;
    QLabel* statusDot_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QTreeView* tree_ = nullptr;
    QFileSystemModel* fsModel_ = nullptr;
    QPushButton* folderBtn_ = nullptr;   // folder selector above the composer
    QPushButton* undoBtn_ = nullptr;     // revert last applied edit
    QListWidget* history_ = nullptr;

    // left drawer overlay
    QFrame* drawer_ = nullptr;
    QPushButton* scrim_ = nullptr;
    QPropertyAnimation* drawerAnim_ = nullptr;
    bool drawerOpen_ = false;
    static constexpr int kDrawerWidth = 300;

    // right editor/diff panel
    EditorPanel* editor_ = nullptr;
    QSplitter* split_ = nullptr;
    WritePreview pendingWrite_;
    bool hasPending_ = false;

    // conversation history (local only)
    ConversationStore store_;
    Conversation current_;
    bool titleGenerated_ = false;  // has the LLM named the current chat yet

    // agentic-loop state
    QString convo_;              // running conversation fed back to the model
    QString currentModel_;       // model chosen for the in-flight turn
    int toolRounds_ = 0;         // tool calls used in the current turn
    bool connected_ = false;
    bool locked_ = true;
    QString statusModel_ = QStringLiteral("—");
};
