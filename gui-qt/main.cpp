// Zima GUI (Qt 6) — entry point.
// Loads the brand-kit QSS, sets a sensible default font, and shows the chat
// window. All visual identity comes from theme.qss (a QSS translation of the
// handoff/ brand kit); this file just wires it up.

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QIcon>
#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Zima"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/AppIcon.png")));

    // Body font: CoFo Sans (heading/body) is licensed and not shipped, so fall
    // back to Segoe UI exactly as the brand kit intends. The brand logo itself
    // ships as assets/Logo.png and is rendered in the sidebar.
    QFont base(QStringLiteral("Segoe UI"), 10);
    app.setFont(base);

    QFile qss(QStringLiteral(":/theme.qss"));
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text))
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));

    MainWindow w;
    w.show();
    return app.exec();
}
