#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtTypes>

#include "core/Rating.h"

// Rich, self-contained playlist model backed by playlists.sqlite (separate from
// the scanned library). Items reference library tracks by path AND carry a
// metadata snapshot plus the search query that produced the match, so a playlist
// survives rescans, can be exported, and can represent unresolved imports.

enum class PlaylistItemStatus {
    Matched,     // resolved to a single library track
    Missing,     // had a track once, but the path is no longer in the library
    Pending,     // imported text with no confident match yet
    MultiMatch,  // import produced several candidates; awaiting a pick
    Approximate, // auto-picked a best guess below the confidence bar; awaiting a glance
};

struct PlaylistItem {
    qint64 id = 0;
    qint64 playlistId = 0;
    int ordinal = 0;            // canonical 0-based position within the playlist
    QString trackPath;          // library resolve key; empty for pending/missing
    QString titleSnapshot;
    QString artistSnapshot;
    QString albumSnapshot;
    qint64 durationMs = 0;
    qint64 addedAt = 0;         // epoch seconds, first added
    qint64 modifiedAt = 0;      // epoch seconds, last edited
    QString comment;            // free-form note, e.g. "where this came from"
    QString sourceText;         // original import string ("Artist - Title — Album"); immutable, never overwritten by matching/replacement
    QString externalId;         // import source id (e.g. "youtube:ID"); link-back + dedup
    QString query;              // remembered search query used to add/edit it
    PlaylistItemStatus status = PlaylistItemStatus::Matched;
    QStringList candidatePaths; // MultiMatch: the import's close candidates,
                                // shown by the edit modal for a quick pick
    int effectiveRating0To100 = Rating::unset; // transient live/library rating for UI display
};

struct Playlist {
    qint64 id = 0;
    QString name;
    QString comment;
    qint64 createdAt = 0;       // epoch seconds
    qint64 updatedAt = 0;       // epoch seconds
    int itemCount = 0;          // populated by list queries; not stored
};

Q_DECLARE_METATYPE(PlaylistItem)
Q_DECLARE_METATYPE(Playlist)
Q_DECLARE_METATYPE(QVector<PlaylistItem>)
