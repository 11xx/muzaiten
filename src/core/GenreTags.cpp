#include "core/GenreTags.h"

#include <QRegularExpression>
#include <QSet>

namespace GenreTags {

QString folded(const QString &genre)
{
    return genre.simplified().toCaseFolded();
}

QString canonical(const QString &folded, const QHash<QString, QString> &aliases)
{
    return aliases.value(folded, folded);
}

QStringList fromMetadata(const MetadataBlob::FullMetadata &metadata)
{
    QStringList genres;

    const auto it = metadata.tags.constFind(QStringLiteral("GENRE"));
    if (it == metadata.tags.constEnd()) {
        return genres;
    }

    // Some taggers pack multiple genres into a single tag value separated by
    // ';', ',', '/' or NUL, on top of GENRE itself being a repeatable tag.
    static const QRegularExpression separator(QStringLiteral("[;,/\\x00]"));

    QSet<QString> seenFolded;
    for (const QString &value : *it) {
        const QStringList parts = value.split(separator);
        for (const QString &part : parts) {
            const QString trimmed = part.simplified();
            if (trimmed.isEmpty()) {
                continue;
            }
            const QString key = folded(trimmed);
            if (seenFolded.contains(key)) {
                continue;
            }
            seenFolded.insert(key);
            genres.append(trimmed);
        }
    }
    return genres;
}

bool isNonGenre(const QString &folded)
{
    // Tagger placeholders, not genres: seen in the wild standing in for an
    // absent GENRE tag rather than the tag being left empty. Keeping this list
    // here (instead of, say, an empty-string check) is deliberate — it is the
    // one place the "junk genre" vocabulary is allowed to grow.
    static const QSet<QString> nonGenres = {
        QStringLiteral("other"),
        QStringLiteral("unknown"),
        QStringLiteral("misc"),
        QStringLiteral("none"),
        QStringLiteral("undefined"),
        QStringLiteral("no genre"),
        QStringLiteral("unclassifiable"),
        QStringLiteral("various"),
        QStringLiteral("genre"),
    };
    return nonGenres.contains(folded);
}

QStringList informative(const QStringList &foldedGenres)
{
    QStringList result;
    result.reserve(foldedGenres.size());
    for (const QString &genre : foldedGenres) {
        if (!isNonGenre(genre)) {
            result.push_back(genre);
        }
    }
    return result;
}

} // namespace GenreTags
