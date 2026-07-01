#include "core/GenreTags.h"

#include <QRegularExpression>
#include <QSet>

namespace GenreTags {

QString folded(const QString &genre)
{
    return genre.simplified().toCaseFolded();
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

} // namespace GenreTags
