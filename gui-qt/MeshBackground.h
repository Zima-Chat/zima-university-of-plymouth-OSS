#pragma once
// ---------------------------------------------------------------------------
// MeshBackground — the Zima "auth shader drift" mesh layer
// ---------------------------------------------------------------------------
// A live, slowly-moving blue mesh gradient painted behind the chat, recreating
// the brand kit's @keyframes auth-drift-a / auth-drift-b (handoff/05-motion):
// large soft radial blobs that translate (±3–5%) and scale (1.04–1.06) on a long
// loop, over the dark canvas, with a faint white stipple dot field on top. Blue-
// cyan only — the kit forbids purple/"AI gradient" hues. Used as the window's
// central widget; child surfaces paint on top of it.
// ---------------------------------------------------------------------------

#include <QWidget>
#include <QElapsedTimer>
#include <QTimer>

class MeshBackground : public QWidget {
    Q_OBJECT
public:
    explicit MeshBackground(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QElapsedTimer clock_;
    QTimer ticker_;
};
