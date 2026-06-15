#pragma once

#include <QString>
#include <QtTypes>

// Small, dependency-free helpers for turning machine quantities into the compact
// human strings the UI shows, and back. Kept generic (no Track/widget coupling)
// so any view or settings dialog can reuse them.
namespace humanquantity {

// Compact track duration for display: "m:ss" (minutes are not capped at 59, to
// preserve the long-standing display). Empty string for non-positive input.
QString formatDuration(qint64 ms);

// Parse a colon-separated clock string ("ss", "m:ss", "h:mm:ss") to
// milliseconds. Blank components count as zero; returns 0 for empty input.
qint64 parseDuration(const QString &text);

// Human file size with binary units (e.g. "4.3 MB"). Empty for non-positive input.
QString formatSize(qint64 bytes);

} // namespace humanquantity
