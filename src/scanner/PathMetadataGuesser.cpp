#include "scanner/PathMetadataGuesser.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

// Heuristics adapted from Web Scrobbler / Metadata Filter (MIT). See the header.
namespace {

// First-separator split (Web Scrobbler `defaultSeparators` + `splitString`).
// Ordered so multi-char separators win over their single-char prefixes.
const QStringList &separators()
{
    static const QStringList kSeparators = {
        QStringLiteral(" – "),  // spaced en dash
        QStringLiteral(" — "),  // spaced em dash
        QStringLiteral(" - "),
        QStringLiteral(" ~ "),
        QStringLiteral(" // "),
        QStringLiteral(" | "),
        QStringLiteral(" · "),  // spaced middle dot
        QStringLiteral("–"),
        QStringLiteral("—"),
        QStringLiteral(" / "),
    };
    return kSeparators;
}

// {left, right} split on the first separator found, or {whole, ""} if none.
QPair<QString, QString> splitOnFirstSeparator(const QString &text)
{
    qsizetype bestIndex = -1;
    qsizetype bestLength = 0;
    for (const QString &sep : separators()) {
        const qsizetype index = text.indexOf(sep);
        if (index >= 0 && (bestIndex < 0 || index < bestIndex)) {
            bestIndex = index;
            bestLength = sep.size();
        }
    }
    if (bestIndex < 0) {
        return {text, QString()};
    }
    return {text.left(bestIndex), text.mid(bestIndex + bestLength)};
}

// Tidy a guessed field: underscores → spaces, drop empty brackets, collapse runs
// of whitespace, and trim leading/trailing junk symbols (Metadata Filter's
// TRIM_SYMBOLS rules).
QString clean(QString text)
{
    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    static const QRegularExpression emptyBrackets(QStringLiteral("\\(\\s*\\)|\\[\\s*\\]"));
    text.remove(emptyBrackets);
    static const QRegularExpression whitespace(QStringLiteral("\\s{2,}"));
    text.replace(whitespace, QStringLiteral(" "));
    static const QRegularExpression leadingJunk(QStringLiteral("^[\\s/,:;~\"\\-]+"));
    text.remove(leadingJunk);
    static const QRegularExpression trailingJunk(QStringLiteral("[\\s/,:;~\"\\-]+$"));
    text.remove(trailingJunk);
    return text.trimmed();
}

// Strip a leading track number ("01 - ", "01.", "1)", "07 ") and return it.
// Only a 1–2 digit run followed by a plain space is taken, so a title like
// "100 Years" is not mistaken for a track number; any digit run followed by an
// explicit separator (-._)]) is taken.
int takeLeadingTrackNumber(QString &text)
{
    static const QRegularExpression withSeparator(QStringLiteral("^\\s*(\\d{1,3})\\s*[-._)\\]]+\\s*"));
    static const QRegularExpression withSpace(QStringLiteral("^\\s*(\\d{1,2})\\s+(?=\\D)"));
    for (const QRegularExpression &re : {withSeparator, withSpace}) {
        const QRegularExpressionMatch match = re.match(text);
        if (match.hasMatch()) {
            const int number = match.captured(1).toInt();
            text = text.mid(match.capturedEnd(0));
            return number;
        }
    }
    return 0;
}

} // namespace

namespace PathMetadataGuesser {

GuessedMetadata guess(const QString &filePath, const QString &libraryRoot)
{
    GuessedMetadata guessed;

    // Directory components between the library root and the file decide how many
    // levels are Artist/Album. Fall back to the file's own parent dirs when the
    // path is not under the root (e.g. resolved through a link root).
    QString relative = libraryRoot.isEmpty()
        ? filePath
        : QDir(libraryRoot).relativeFilePath(filePath);
    if (relative.startsWith(QStringLiteral("../")) || relative == filePath) {
        relative = filePath;
    }
    QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return guessed;
    }
    const QString fileName = parts.takeLast();  // parts now holds only directories
    const QStringList &dirs = parts;

    if (dirs.size() >= 2) {
        guessed.artist = clean(dirs.at(dirs.size() - 2));
        guessed.album = clean(dirs.at(dirs.size() - 1));
    } else if (dirs.size() == 1) {
        // A single containing folder is most useful as the artist (the artist
        // panel groups first); the album fills in from the real tags later.
        guessed.artist = clean(dirs.at(0));
    }

    QString base = QFileInfo(fileName).completeBaseName();
    guessed.trackNumber = takeLeadingTrackNumber(base);

    if (guessed.artist.isEmpty()) {
        // Flat layout: try to read "Artist - Title" out of the filename itself.
        const QPair<QString, QString> split = splitOnFirstSeparator(base);
        if (!split.second.isEmpty()) {
            guessed.artist = clean(split.first);
            guessed.title = clean(split.second);
        } else {
            guessed.title = clean(base);
        }
    } else {
        // Directory already named the artist; keep the whole filename as the title
        // rather than risk mis-splitting a title that contains a dash.
        guessed.title = clean(base);
    }

    guessed.albumArtist = guessed.artist;
    return guessed;
}

} // namespace PathMetadataGuesser
