#include "playlist/PlaylistImport.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QTextStream>

#include <cmath>

namespace PlaylistImport {

namespace {

// "Artist - Title" separators, tried in order: spaced hyphen, en dash, em dash.
const QStringList kArtistTitleSeparators = {
    QStringLiteral(" - "), QStringLiteral(" – "), QStringLiteral(" — "),
};

// Words that mark a parenthesized segment as packaging noise rather than part
// of the title ("(Official Video)", "[Remastered 2011]", "(feat. X)" …).
bool isNoiseSegment(const QString &segment)
{
    static const QRegularExpression noise(QStringLiteral(
        "\\b(feat|ft|featuring|official|video|audio|lyric|lyrics|visualizer|"
        "remaster(ed)?|mono|stereo|live|version|edit|mix|hd|hq|mv|explicit|"
        "bonus|deluxe|single|album|out now|full)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return noise.match(segment).hasMatch();
}

QString stripLeadingTrackNumber(QString line)
{
    // "07. ", "7) ", "12 - ", "03: " — common copy-paste numbering.
    static const QRegularExpression numbering(
        QStringLiteral("^\\s*\\d{1,4}\\s*[\\.\\)\\:-]\\s+"));
    return line.remove(numbering);
}

QString stripTrailingDuration(QString line)
{
    // Trailing "3:45", "(3:45)" or "[1:02:33]" from tracklist copy-paste.
    static const QRegularExpression duration(
        QStringLiteral("[\\s\\(\\[]+\\d{1,2}:\\d{2}(:\\d{2})?[\\)\\]]?\\s*$"));
    return line.remove(duration);
}

// Minimal RFC-ish csv line splitter (handles quoted fields with "" escapes).
QStringList splitCsvLine(const QString &line)
{
    QStringList fields;
    QString current;
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                    current.append(QLatin1Char('"'));
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                current.append(c);
            }
        } else if (c == QLatin1Char('"')) {
            inQuotes = true;
        } else if (c == QLatin1Char(',')) {
            fields.append(current);
            current.clear();
        } else {
            current.append(c);
        }
    }
    fields.append(current);
    return fields;
}

QVector<ImportEntry> parsePlainText(const QString &text)
{
    QVector<ImportEntry> entries;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        entries.append(parseLine(line));
    }
    return entries;
}

QVector<ImportEntry> parseM3u(const QString &text)
{
    QVector<ImportEntry> entries;
    ImportEntry pending;     // filled by #EXTINF, consumed by the next path line
    bool havePending = false;

    static const QRegularExpression extinf(
        QStringLiteral("^#EXTINF:\\s*(-?\\d+)[^,]*,(.*)$"));

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QLatin1Char('#'))) {
            const auto m = extinf.match(line);
            if (m.hasMatch()) {
                pending = parseLine(m.captured(2).trimmed());
                const qint64 secs = m.captured(1).toLongLong();
                pending.durationMs = secs > 0 ? secs * 1000 : 0;
                havePending = true;
            }
            continue;
        }
        // A non-comment line is a path/URI.
        ImportEntry entry = havePending ? pending : ImportEntry{};
        if (!havePending) {
            // Bare path with no EXTINF: guess from the basename.
            entry = parseLine(QFileInfo(line).completeBaseName());
        }
        entry.directPath = line;
        if (entry.rawLine.isEmpty()) {
            entry.rawLine = line;
        }
        entries.append(entry);
        havePending = false;
    }
    return entries;
}

QVector<ImportEntry> parseCsv(const QString &text)
{
    QVector<ImportEntry> entries;
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.isEmpty()) {
        return entries;
    }

    // Map header names → column indices (case-insensitive, csv export aliases).
    const QStringList header = splitCsvLine(lines.first().trimmed());
    int titleCol = -1, artistCol = -1, albumCol = -1, durationCol = -1, pathCol = -1;
    for (int i = 0; i < header.size(); ++i) {
        const QString name = header.at(i).trimmed().toLower();
        if (name == QStringLiteral("title") || name == QStringLiteral("track name")
            || name == QStringLiteral("song") || name == QStringLiteral("name")) {
            titleCol = i;
        } else if (name == QStringLiteral("artist") || name == QStringLiteral("artist name")
                   || name == QStringLiteral("artists")) {
            artistCol = i;
        } else if (name == QStringLiteral("album") || name == QStringLiteral("album name")) {
            albumCol = i;
        } else if (name == QStringLiteral("duration_ms") || name == QStringLiteral("duration")) {
            durationCol = i;
        } else if (name == QStringLiteral("path") || name == QStringLiteral("file")
                   || name == QStringLiteral("location")) {
            pathCol = i;
        }
    }
    if (titleCol < 0) {
        return entries;  // not a csv we understand; caller falls back to plain text
    }

    const auto fieldAt = [](const QStringList &fields, int idx) {
        return (idx >= 0 && idx < fields.size()) ? fields.at(idx).trimmed() : QString();
    };
    for (int i = 1; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QStringList fields = splitCsvLine(line);
        ImportEntry entry;
        entry.rawLine = line;
        entry.title = fieldAt(fields, titleCol);
        entry.artist = fieldAt(fields, artistCol);
        entry.album = fieldAt(fields, albumCol);
        entry.directPath = fieldAt(fields, pathCol);
        const QString duration = fieldAt(fields, durationCol);
        if (!duration.isEmpty()) {
            bool ok = false;
            const qint64 value = duration.toLongLong(&ok);
            if (ok) {
                // Heuristic: our export stores ms; small values are seconds.
                entry.durationMs = value > 30000 ? value : value * 1000;
            }
        }
        if (entry.title.isEmpty() && entry.directPath.isEmpty()) {
            continue;
        }
        entries.append(entry);
    }
    return entries;
}

// Coerce a JSON value to a duration in milliseconds (accepts a number or a numeric
// string; negative/invalid → 0).
qint64 jsonDurationMs(const QJsonValue &value)
{
    qint64 ms = 0;
    if (value.isDouble()) {
        ms = static_cast<qint64>(value.toDouble());
    } else if (value.isString()) {
        ms = value.toString().toLongLong();
    }
    return ms > 0 ? ms : 0;
}

// JSONL addedAt is a positive, integral Unix timestamp in seconds. Bound it to
// dates Qt can represent through year 9999 so malformed producer values cannot
// silently become a plausible historical date.
qint64 jsonAddedAt(const QJsonValue &value)
{
    constexpr qint64 kMaxUnixTimestamp = 253402300799;
    if (value.isDouble()) {
        const double seconds = value.toDouble();
        if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > kMaxUnixTimestamp
            || std::trunc(seconds) != seconds) {
            return 0;
        }
        return static_cast<qint64>(seconds);
    }
    if (value.isString()) {
        const QString text = value.toString().trimmed();
        static const QRegularExpression integer(QStringLiteral("^[0-9]+$"));
        if (!integer.match(text).hasMatch()) {
            return 0;
        }
        bool ok = false;
        const qint64 seconds = text.toLongLong(&ok);
        return ok && seconds > 0 && seconds <= kMaxUnixTimestamp ? seconds : 0;
    }
    return 0;
}

// JSONL (docs/playlist-import-jsonl.md): one JSON object per line. Blank lines and
// '#'-comment lines are ignored; a malformed line is skipped (never aborts). The
// first content line may be a {"playlist":{…}} header (reported via outHeader).
QVector<ImportEntry> parseJsonl(const QString &text, ImportHeader *outHeader)
{
    QVector<ImportEntry> entries;
    bool headerEligible = true;  // only the first content line may be the header
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
        if (!doc.isObject()) {
            headerEligible = false;  // a content line, just unusable
            continue;
        }
        const QJsonObject obj = doc.object();

        if (headerEligible && obj.size() == 1 && obj.value(QStringLiteral("playlist")).isObject()) {
            const QJsonObject pl = obj.value(QStringLiteral("playlist")).toObject();
            if (outHeader != nullptr) {
                outHeader->present = true;
                outHeader->name = pl.value(QStringLiteral("name")).toString().trimmed();
                outHeader->comment = pl.value(QStringLiteral("comment")).toString().trimmed();
            }
            headerEligible = false;
            continue;
        }
        headerEligible = false;

        ImportEntry entry;
        entry.rawLine = line;
        entry.title = obj.value(QStringLiteral("title")).toString().trimmed();
        entry.artist = obj.value(QStringLiteral("artist")).toString().trimmed();
        entry.album = obj.value(QStringLiteral("album")).toString().trimmed();
        entry.directPath = obj.value(QStringLiteral("directPath")).toString().trimmed();
        entry.externalId = obj.value(QStringLiteral("externalId")).toString().trimmed();
        entry.comment = obj.value(QStringLiteral("comment")).toString().trimmed();
        entry.durationMs = jsonDurationMs(obj.value(QStringLiteral("durationMs")));
        entry.addedAt = jsonAddedAt(obj.value(QStringLiteral("addedAt")));
        if (entry.title.isEmpty() && entry.directPath.isEmpty()) {
            continue;  // nothing to match on
        }
        entries.append(entry);
    }
    return entries;
}

Format detectFormat(const QString &text)
{
    const QString head = text.left(4096).trimmed();
    if (head.startsWith(QStringLiteral("#EXTM3U")) || head.contains(QStringLiteral("#EXTINF:"))) {
        return Format::M3U;
    }
    // First content line (skipping blanks and '#' comments) starting with '{' → JSONL.
    for (const QString &raw : head.split(QLatin1Char('\n'))) {
        const QString t = raw.trimmed();
        if (t.isEmpty() || t.startsWith(QLatin1Char('#'))) {
            continue;
        }
        if (t.startsWith(QLatin1Char('{'))) {
            return Format::Jsonl;
        }
        break;  // first real line isn't a JSON object
    }
    const QString firstLine = head.section(QLatin1Char('\n'), 0, 0).toLower();
    if (firstLine.contains(QLatin1Char(','))
        && (firstLine.contains(QStringLiteral("title")) || firstLine.contains(QStringLiteral("track name")))
        && firstLine.contains(QStringLiteral("artist"))) {
        return Format::Csv;
    }
    return Format::PlainText;
}

} // namespace

ImportEntry parseLine(const QString &line)
{
    ImportEntry entry;
    entry.rawLine = line.trimmed();

    QString work = stripTrailingDuration(stripLeadingTrackNumber(entry.rawLine));
    work = work.trimmed();

    for (const QString &sep : kArtistTitleSeparators) {
        const qsizetype idx = work.indexOf(sep);
        if (idx > 0) {
            entry.artist = work.left(idx).trimmed();
            entry.title = work.mid(idx + sep.size()).trimmed();
            return entry;
        }
    }
    entry.title = work;
    return entry;
}

QVector<ImportEntry> parse(const QString &text, Format format, ImportHeader *outHeader)
{
    if (outHeader != nullptr) {
        *outHeader = ImportHeader{};
    }
    if (format == Format::Auto) {
        format = detectFormat(text);
    }
    switch (format) {
    case Format::M3U:
        return parseM3u(text);
    case Format::Jsonl:
        return parseJsonl(text, outHeader);
    case Format::Csv: {
        QVector<ImportEntry> entries = parseCsv(text);
        // A "csv" without a usable header degrades to plain text.
        return entries.isEmpty() ? parsePlainText(text) : entries;
    }
    case Format::PlainText:
    case Format::Auto:
        break;
    }
    return parsePlainText(text);
}

QVector<ImportEntry> parseFile(const QString &filePath, QString *errorOut, ImportHeader *outHeader)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut != nullptr) {
            *errorOut = file.errorString();
        }
        return {};
    }
    QTextStream in(&file);
    const QString text = in.readAll();

    const QString ext = QFileInfo(filePath).suffix().toLower();
    Format format = Format::Auto;
    if (ext == QStringLiteral("m3u") || ext == QStringLiteral("m3u8")) {
        format = Format::M3U;
    } else if (ext == QStringLiteral("csv")) {
        format = Format::Csv;
    } else if (ext == QStringLiteral("jsonl") || ext == QStringLiteral("ndjson")) {
        format = Format::Jsonl;
    }
    return parse(text, format, outHeader);
}

QString normalizeForMatch(const QString &text)
{
    // NFKD then drop combining marks → strips diacritics; punctuation → space.
    const QString decomposed = text.normalized(QString::NormalizationForm_KD).toLower();
    QString out;
    out.reserve(decomposed.size());
    bool lastWasSpace = true;
    for (const QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing) {
            continue;
        }
        if (c.isLetterOrNumber()) {
            out.append(c);
            lastWasSpace = false;
        } else if (!lastWasSpace) {
            out.append(QLatin1Char(' '));
            lastWasSpace = true;
        }
    }
    return out.trimmed();
}

QString stripTitleNoise(const QString &title)
{
    QString work = title;

    // Remove parenthesized/bracketed segments that are packaging noise.
    static const QRegularExpression bracketed(QStringLiteral("[\\(\\[]([^\\)\\]]*)[\\)\\]]"));
    qsizetype offset = 0;
    while (true) {
        const auto m = bracketed.match(work, offset);
        if (!m.hasMatch()) {
            break;
        }
        if (isNoiseSegment(m.captured(1))) {
            work.remove(m.capturedStart(), m.capturedLength());
            offset = m.capturedStart();
        } else {
            offset = m.capturedEnd();
        }
    }

    // Trailing unbracketed "feat./ft. …" tail.
    static const QRegularExpression featTail(
        QStringLiteral("\\b(feat|ft|featuring)\\.?\\s.*$"),
        QRegularExpression::CaseInsensitiveOption);
    work.remove(featTail);

    return work.simplified();
}

} // namespace PlaylistImport
