#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// Pure JSON response parsers for the Stage 0b scrobbler backfill. Free
// functions only — no network, no Qt-network includes. Everything that turns a
// service HTTP body into structured data lives here so it can be unit-tested
// without a socket. The ScrobbleBackfill worker owns the transport and feeds
// raw QByteArray bodies through these.
namespace BackfillParse {

// One historical listen from a ListenBrainz listens page. Mirrors
// ListenHistoryStore::ImportedListen minus the source/match fields, which the
// worker fills in.
struct ListenBrainzListen {
    qint64 listenedAtSecs = 0;
    QString title;
    QString artist;
    QString album;
    QString mbRecordingId;
};

struct ListenBrainzPage {
    bool ok = false;                 // JSON parsed and the payload shape was present
    QList<ListenBrainzListen> listens;
    // Oldest listened_at in this page: the next `max_ts` cursor to page further
    // back. 0 when the page carried no usable listens.
    qint64 oldestListenedAt = 0;
};

struct TokenValidation {
    bool valid = false;
    QString username;
};

// One track from a Last.fm user.getTopTracks page.
struct LastFmTrack {
    QString artist;
    QString title;
    qint64 count = 0;
    QString mbRecordingId;
};

struct LastFmTopTracksPage {
    bool ok = false;                 // JSON parsed and toptracks shape was present
    QList<LastFmTrack> tracks;
    int page = 0;
    int totalPages = 0;
    // Last.fm error envelope {"error": <code>, "message": "..."}; 0 = no error.
    int errorCode = 0;
    QString errorMessage;
};

// Parse a ListenBrainz `GET /1/user/{name}/listens` page. Rows with empty
// artist or title, or listened_at <= 0, are dropped.
ListenBrainzPage parseListenBrainzPage(const QByteArray &json);

// Parse a ListenBrainz `GET /1/validate-token` response.
TokenValidation parseTokenValidation(const QByteArray &json);

// Parse a Last.fm `user.getTopTracks` page (format=json). Handles numbers
// arriving as JSON strings and a single-track page delivering `track` as an
// object rather than an array.
LastFmTopTracksPage parseLastFmTopTracks(const QByteArray &json);

} // namespace BackfillParse
