#pragma once
// The open project folder. All file/shell tools are sandboxed to this root:
// absolute paths and any path escaping the root (via "..") are refused, so the
// assistant can only touch files inside the folder the user explicitly opened.

#include <QString>
#include <QDir>

class Workspace {
public:
    static Workspace& instance() { static Workspace w; return w; }

    bool isOpen() const { return !root_.isEmpty(); }
    QString root() const { return root_; }
    void setRoot(const QString& dir) { root_ = QDir(dir).absolutePath(); }
    void close() { root_.clear(); }

    // Resolve a workspace-relative path to an absolute path inside the root.
    // Returns empty on any escape attempt or when no workspace is open.
    QString resolve(const QString& rel) const {
        if (root_.isEmpty()) return {};
        QString norm = QDir::fromNativeSeparators(rel).trimmed();
        if (norm.startsWith(QLatin1Char('/')) || QDir::isAbsolutePath(norm)) return {};
        for (const QString& part : norm.split(QLatin1Char('/')))
            if (part == QStringLiteral("..")) return {};
        QString abs = QDir(root_).absoluteFilePath(norm);
        // Defence in depth: the resolved path must still live under the root.
        QString canonRoot = QDir(root_).absolutePath() + QLatin1Char('/');
        QString absSlash = QDir::cleanPath(abs) + QLatin1Char('/');
        if (!absSlash.startsWith(canonRoot) && QDir::cleanPath(abs) != QDir(root_).absolutePath())
            return {};
        return QDir::cleanPath(abs);
    }

private:
    QString root_;
};
