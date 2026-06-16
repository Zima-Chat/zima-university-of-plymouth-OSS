#pragma once
// ---------------------------------------------------------------------------
// Zima GUI (Qt) — AgentClient
// ---------------------------------------------------------------------------
// Owns the encrypted named-pipe channel to zima-agent. Lives in its own QThread
// so blocking pipe reads never stall the UI. Public methods are invoked as
// queued slots (cross-thread); results come back as Qt signals delivered on the
// GUI thread. The handshake mirrors the CLI: connect → (spawn agent if down) →
// authenticate with the session token → derive the channel key → encrypt.
// ---------------------------------------------------------------------------

#include <QObject>
#include <QString>

class WindowsIpcClient;

class AgentClient : public QObject {
    Q_OBJECT
public:
    explicit AgentClient(QObject* parent = nullptr);
    ~AgentClient() override;

public slots:
    void sendPrompt(const QString& prompt, const QString& model);
    void requestComplete(const QString& prompt, const QString& model); // non-stream
    void requestTitle(const QString& prompt, const QString& model);    // best-effort title
    void configureCredential(const QString& apiKey);
    void requestStatus();
    void requestModels();

signals:
    void connected();
    void disconnected(const QString& reason);
    void chunk(const QString& delta);     // one streamed UTF-8 delta
    void turnFinished();                   // assistant turn complete
    void completeReply(const QString& content); // full non-stream reply
    void titleReady(const QString& title);      // generated conversation title
    void errorOccurred(const QString& message);
    void statusReceived(bool locked, const QString& model);
    void modelsReceived(const QString& json);
    void loginSucceeded();

private:
    bool ensure();                         // connect + authenticate if needed
    WindowsIpcClient* client_ = nullptr;
};
