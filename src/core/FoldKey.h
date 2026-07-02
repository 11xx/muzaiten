#pragma once

#include <QChar>
#include <QString>

// Lightweight grouping key: whitespace-normalized + case-folded. Shared by the
// scrobbler backfill match index and the radio recommender so artist/album
// identity folds identically everywhere (deliberately distinct from the search
// engine's heavier CJK-aware Fold, which is tuned for recall, not equality).
namespace FoldKey {

// Case-fold + collapse whitespace for a single field.
inline QString fold(const QString &value)
{
    return value.simplified().toCaseFolded();
}

// Composite "albumartist\nalbum" key; the newline separator can never occur in a
// simplified field, so distinct pairs never collide.
inline QString albumKey(const QString &albumArtist, const QString &album)
{
    return fold(albumArtist) + QLatin1Char('\n') + fold(album);
}

inline QString songKey(const QString &mbRecordingId, const QString &artist, const QString &title)
{
    if (!mbRecordingId.isEmpty()) {
        return QStringLiteral("mbid:") + mbRecordingId;
    }
    return QStringLiteral("at:") + fold(artist) + QLatin1Char('\n') + fold(title);
}

inline QString albumGroupKey(const QString &releaseGroupMbid, const QString &albumArtist,
                             const QString &album)
{
    if (!releaseGroupMbid.isEmpty()) {
        return QStringLiteral("rg:") + releaseGroupMbid;
    }
    return albumKey(albumArtist, album);
}

} // namespace FoldKey
