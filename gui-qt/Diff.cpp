#include "Diff.h"
#include <QStringList>
#include <vector>
#include <algorithm>

namespace diffutil {

// Classic LCS over lines, then walk back to emit a unified diff body.
static void lcsDiff(const QStringList& a, const QStringList& b,
                    QString* out, int* added, int* removed) {
    const int n = a.size(), m = b.size();
    // dp[i][j] = LCS length of a[i:], b[j:]
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (int i = n - 1; i >= 0; --i)
        for (int j = m - 1; j >= 0; --j)
            dp[i][j] = (a[i] == b[j]) ? dp[i + 1][j + 1] + 1
                                      : std::max(dp[i + 1][j], dp[i][j + 1]);
    int i = 0, j = 0, add = 0, rem = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            if (out) *out += QStringLiteral(" ") + a[i] + QLatin1Char('\n');
            ++i; ++j;
        } else if (dp[i + 1][j] >= dp[i][j + 1]) {
            if (out) *out += QStringLiteral("-") + a[i] + QLatin1Char('\n');
            ++i; ++rem;
        } else {
            if (out) *out += QStringLiteral("+") + b[j] + QLatin1Char('\n');
            ++j; ++add;
        }
    }
    while (i < n) { if (out) *out += QStringLiteral("-") + a[i] + QLatin1Char('\n'); ++i; ++rem; }
    while (j < m) { if (out) *out += QStringLiteral("+") + b[j] + QLatin1Char('\n'); ++j; ++add; }
    if (added) *added = add;
    if (removed) *removed = rem;
}

QString unified(const QString& oldText, const QString& newText, const QString& path) {
    const QStringList a = oldText.isEmpty() ? QStringList() : oldText.split(QLatin1Char('\n'));
    const QStringList b = newText.isEmpty() ? QStringList() : newText.split(QLatin1Char('\n'));
    QString body;
    lcsDiff(a, b, &body, nullptr, nullptr);
    QString header = QStringLiteral("--- a/%1\n+++ b/%1\n").arg(path);
    return header + body;
}

void counts(const QString& oldText, const QString& newText, int& added, int& removed) {
    const QStringList a = oldText.isEmpty() ? QStringList() : oldText.split(QLatin1Char('\n'));
    const QStringList b = newText.isEmpty() ? QStringList() : newText.split(QLatin1Char('\n'));
    lcsDiff(a, b, nullptr, &added, &removed);
}

} // namespace diffutil
