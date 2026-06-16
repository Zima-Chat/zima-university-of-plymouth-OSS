#include "MeshBackground.h"

#include <QPainter>
#include <QRadialGradient>
#include <QtMath>

MeshBackground::MeshBackground(QWidget* parent) : QWidget(parent) {
    // Paint our own pixels; children sit on top.
    setAttribute(Qt::WA_StyledBackground, false);
    clock_.start();
    // ~30 fps is plenty for a slow drift and keeps the CPU idle-quiet.
    ticker_.setInterval(33);
    connect(&ticker_, &QTimer::timeout, this, [this] { update(); });
    ticker_.start();
}

// One soft radial blob, fully transparent at its edge so blobs blend into a mesh.
static void blob(QPainter& p, const QRectF& area, qreal cx, qreal cy,
                 qreal radius, QColor color, int centerAlpha) {
    QRadialGradient g(cx, cy, radius);
    QColor c0 = color; c0.setAlpha(centerAlpha);
    QColor cm = color; cm.setAlpha(centerAlpha / 3);
    QColor c1 = color; c1.setAlpha(0);
    g.setColorAt(0.0, c0);
    g.setColorAt(0.5, cm);
    g.setColorAt(1.0, c1);
    p.fillRect(area, g);
}

void MeshBackground::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = rect();
    const qreal W = r.width(), H = r.height();
    const qreal big = qMax(W, H);

    // base canvas oklch(17.3% 0 0)
    p.fillRect(r, QColor(0x1A, 0x1A, 0x1A));

    const qreal t = clock_.elapsed() / 1000.0; // seconds

    // The auth-drift keyframes are intentionally gentle; expressed continuously
    // with separate x/y phases each blob traces a slow elliptical path that is
    // clearly in motion (≈10–16s loops) without distracting from the text.
    // Blob A — zima-600, upper-left.
    blob(p, r,
         W * (0.32 + 0.13 * qSin(t * 0.46)),
         H * (0.34 + 0.11 * qSin(t * 0.38 + 1.0)),
         big * (0.56 + 0.05 * qSin(t * 0.24)),
         QColor(0x39, 0x93, 0xC6), 72);

    // Blob B — zima-800 (deeper), lower-right.
    blob(p, r,
         W * (0.70 + 0.14 * qSin(t * 0.33 + 2.1)),
         H * (0.64 + 0.13 * qSin(t * 0.29 + 0.5)),
         big * (0.60 + 0.07 * qSin(t * 0.2 + 1.0)),
         QColor(0x15, 0x5F, 0x89), 82);

    // Blob C — zima-400 highlight, faint, sweeping across the top.
    blob(p, r,
         W * (0.52 + 0.20 * qSin(t * 0.52 + 3.0)),
         H * (0.18 + 0.12 * qSin(t * 0.44 + 1.5)),
         big * 0.40, QColor(0x7D, 0xC7, 0xEC), 40);

    // zima-950 vignette tint sunk into the bottom for depth (breathes slowly).
    blob(p, r, W * (0.5 + 0.06 * qSin(t * 0.17)), H * 1.05,
         big * 0.7, QColor(0x0A, 0x33, 0x4E), 90);

    // ---- stipple dot field (handoff: --dot-color rgba(255,255,255,0.08)) ----
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 14));
    const qreal step = 26.0;
    const qreal dot = 1.4;
    for (qreal y = (int(H) % int(step)) / 2.0; y < H; y += step)
        for (qreal x = (int(W) % int(step)) / 2.0; x < W; x += step)
            p.drawEllipse(QPointF(x, y), dot, dot);
}
