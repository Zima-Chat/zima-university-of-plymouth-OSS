#pragma once
// A compact line-based unified diff (LCS) for previewing proposed file writes.
#include <QString>

namespace diffutil {

// Returns a unified-diff-style string (lines prefixed with ' ', '+', '-') for
// turning oldText into newText. `path` is used in the @@ header line.
QString unified(const QString& oldText, const QString& newText, const QString& path);

// Count added / removed lines (for a concise "+N −M" summary).
void counts(const QString& oldText, const QString& newText, int& added, int& removed);

} // namespace diffutil
