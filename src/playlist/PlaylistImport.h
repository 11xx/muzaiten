#pragma once

// Playlist import: turns external playlist text into neutral ImportEntry rows
// that the matcher (PlaylistMatcher) resolves against the library. Pure
// parsing, no I/O beyond reading the given file — offline-testable.
//
// Accepted inputs:
//  - Plain text: one entry per line. Tolerates "Artist - Title",
//    "NN. Artist - Title", "Title — Artist" (en/em dashes), stray whitespace.
//  - M3U/M3U8: "#EXTINF:secs,Artist - Title" + path lines. The path is kept as
//    directPath so the matcher can resolve by path before falling back to text.
//  - CSV: our own export columns (ordinal,title,artist,album,duration_ms,path,
//    status,query,comment) or any csv with a header naming title/artist columns.

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

namespace PlaylistImport {

struct ImportEntry {
    QString rawLine;     // original input line (trimmed), kept for display/debug
    QString title;       // guessed title (may be the whole line)
    QString artist;      // guessed artist; empty when the line had no separator
    QString album;       // only from csv
    QString directPath;  // m3u/csv path; matcher tries this before text matching
    qint64 durationMs = 0;  // from EXTINF/csv; tiebreaker for matching
    QString externalId;  // service id/url (e.g. YouTube video id); not matched on yet
};

enum class Format { Auto, PlainText, M3U, Csv };

// Parse pasted/loaded text. Auto detection: leading "#EXTM3U"/"#EXTINF" → M3U;
// a first line that looks like a csv header with title/artist columns → Csv;
// otherwise plain text.
QVector<ImportEntry> parse(const QString &text, Format format = Format::Auto);

// Reads the file (utf-8) and parses it, picking the format from the extension
// (.m3u/.m3u8/.csv) before falling back to content detection.
QVector<ImportEntry> parseFile(const QString &filePath, QString *errorOut = nullptr);

// Split one free-text line into artist/title guesses. Exposed for tests.
ImportEntry parseLine(const QString &line);

// Lowercase, strip diacritics and punctuation, collapse whitespace — the
// normal form used to build match queries.
QString normalizeForMatch(const QString &text);

// normalizeForMatch + noise removal: parenthesized/bracketed segments that are
// just packaging ("(Official Video)", "[Remastered 2011]", "(feat. X)" …) and
// a trailing "feat./ft. …" tail.
QString stripTitleNoise(const QString &title);

} // namespace PlaylistImport

Q_DECLARE_METATYPE(PlaylistImport::ImportEntry)
Q_DECLARE_METATYPE(QVector<PlaylistImport::ImportEntry>)
