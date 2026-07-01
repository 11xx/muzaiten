#include "scrobble/BackfillParse.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace BackfillParse {

namespace {

// Last.fm serializes every number as a JSON string ("123"); ListenBrainz uses
// real JSON numbers. Accept both shapes for any numeric field.
qint64 toLongLong(const QJsonValue &value)
{
    if (value.isDouble()) {
        return static_cast<qint64>(value.toDouble());
    }
    if (value.isString()) {
        return value.toString().toLongLong();
    }
    return 0;
}

int toInt(const QJsonValue &value)
{
    return static_cast<int>(toLongLong(value));
}

} // namespace

ListenBrainzPage parseListenBrainzPage(const QByteArray &json)
{
    ListenBrainzPage page;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return page;
    }
    const QJsonValue payloadValue = doc.object().value(QStringLiteral("payload"));
    if (!payloadValue.isObject()) {
        return page;
    }
    // A well-formed payload (even an empty one) counts as a successful parse:
    // the worker relies on ok==true + empty listens to detect end-of-history.
    page.ok = true;

    const QJsonArray listens = payloadValue.toObject().value(QStringLiteral("listens")).toArray();
    for (const QJsonValue &entry : listens) {
        const QJsonObject listen = entry.toObject();
        const qint64 listenedAt = toLongLong(listen.value(QStringLiteral("listened_at")));
        const QJsonObject metadata = listen.value(QStringLiteral("track_metadata")).toObject();

        ListenBrainzListen row;
        row.listenedAtSecs = listenedAt;
        row.artist = metadata.value(QStringLiteral("artist_name")).toString().trimmed();
        row.title = metadata.value(QStringLiteral("track_name")).toString().trimmed();
        row.album = metadata.value(QStringLiteral("release_name")).toString().trimmed();
        const QJsonObject mapping = metadata.value(QStringLiteral("mbid_mapping")).toObject();
        row.mbRecordingId = mapping.value(QStringLiteral("recording_mbid")).toString().trimmed();

        if (row.artist.isEmpty() || row.title.isEmpty() || row.listenedAtSecs <= 0) {
            continue;
        }
        if (page.oldestListenedAt == 0 || row.listenedAtSecs < page.oldestListenedAt) {
            page.oldestListenedAt = row.listenedAtSecs;
        }
        page.listens.push_back(row);
    }
    return page;
}

TokenValidation parseTokenValidation(const QByteArray &json)
{
    TokenValidation result;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return result;
    }
    const QJsonObject object = doc.object();
    result.valid = object.value(QStringLiteral("valid")).toBool();
    result.username = object.value(QStringLiteral("user_name")).toString().trimmed();
    return result;
}

LastFmTopTracksPage parseLastFmTopTracks(const QByteArray &json)
{
    LastFmTopTracksPage page;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return page;
    }
    const QJsonObject root = doc.object();

    // Error envelope: {"error": <code>, "message": "..."}.
    if (root.contains(QStringLiteral("error"))) {
        page.errorCode = toInt(root.value(QStringLiteral("error")));
        page.errorMessage = root.value(QStringLiteral("message")).toString();
        return page;
    }

    const QJsonValue topTracksValue = root.value(QStringLiteral("toptracks"));
    if (!topTracksValue.isObject()) {
        return page;
    }
    const QJsonObject topTracks = topTracksValue.toObject();
    page.ok = true;

    const QJsonObject attr = topTracks.value(QStringLiteral("@attr")).toObject();
    page.page = toInt(attr.value(QStringLiteral("page")));
    page.totalPages = toInt(attr.value(QStringLiteral("totalPages")));

    // A single-track page delivers `track` as an object, not an array.
    const QJsonValue trackValue = topTracks.value(QStringLiteral("track"));
    QJsonArray tracks;
    if (trackValue.isArray()) {
        tracks = trackValue.toArray();
    } else if (trackValue.isObject()) {
        tracks.append(trackValue);
    }

    for (const QJsonValue &entry : tracks) {
        const QJsonObject track = entry.toObject();
        LastFmTrack row;
        row.title = track.value(QStringLiteral("name")).toString().trimmed();
        row.count = toLongLong(track.value(QStringLiteral("playcount")));
        row.mbRecordingId = track.value(QStringLiteral("mbid")).toString().trimmed();
        row.artist = track.value(QStringLiteral("artist")).toObject()
                         .value(QStringLiteral("name")).toString().trimmed();
        if (row.artist.isEmpty() || row.title.isEmpty()) {
            continue;
        }
        page.tracks.push_back(row);
    }
    return page;
}

} // namespace BackfillParse
