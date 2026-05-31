#include "scrobble/LastFmApi.h"

#include <QCryptographicHash>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>

namespace {

constexpr qint64 minimumDurationMs = 30 * 1000;

bool shouldSign(const QString &key)
{
    return key != QStringLiteral("api_sig") && key != QStringLiteral("format") && key != QStringLiteral("callback");
}

int intAttribute(const QXmlStreamAttributes &attributes, const QStringView name, int fallback = -1)
{
    bool ok = false;
    const int value = attributes.value(name).toInt(&ok);
    return ok ? value : fallback;
}

QString lowerName(const QXmlStreamReader &reader)
{
    return reader.name().toString().toLower();
}

} // namespace

namespace LastFmApi {

void addParam(Params &params, const QString &key, const QString &value)
{
    params.push_back({key, value});
}

void addParamIfPresent(Params &params, const QString &key, const QString &value)
{
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty()) {
        addParam(params, key, trimmed);
    }
}

QString signature(const Params &params, const QString &secret)
{
    Params signedParams;
    for (const auto &param : params) {
        if (shouldSign(param.first)) {
            signedParams.push_back(param);
        }
    }

    std::sort(signedParams.begin(), signedParams.end(), [](const auto &left, const auto &right) {
        return left.first.toUtf8() < right.first.toUtf8();
    });

    QByteArray input;
    for (const auto &param : signedParams) {
        input += param.first.toUtf8();
        input += param.second.toUtf8();
    }
    input += secret.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(input, QCryptographicHash::Md5).toHex());
}

QByteArray formBody(const Params &params)
{
    // Build application/x-www-form-urlencoded using QUrl::toPercentEncoding so
    // that '+' is encoded as '%2B'. QUrlQuery::query(FullyEncoded) leaves '+' as
    // a literal '+', which servers decode as a space — diverging from the raw
    // UTF-8 bytes that signature() signed, causing error 13 on tracks/artists
    // whose names contain '+' (e.g. "C++", "Justice + Simian").
    QByteArray body;
    bool first = true;
    for (const auto &param : params) {
        if (!first) {
            body += '&';
        }
        first = false;
        body += QUrl::toPercentEncoding(param.first);
        body += '=';
        body += QUrl::toPercentEncoding(param.second);
    }
    return body;
}

Response parseXml(const QByteArray &body)
{
    Response response;
    QXmlStreamReader reader(body);
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement()) {
            continue;
        }

        const QString name = lowerName(reader);
        if (name == QStringLiteral("lfm")) {
            response.parsed = true;
            response.ok = reader.attributes().value(QStringLiteral("status")) == QStringLiteral("ok");
        } else if (name == QStringLiteral("error")) {
            response.errorCode = intAttribute(reader.attributes(), QStringLiteral("code"), 0);
            response.errorMessage = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
        } else if (name == QStringLiteral("token")) {
            response.token = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
        } else if (name == QStringLiteral("name")) {
            response.sessionName = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
        } else if (name == QStringLiteral("key")) {
            response.sessionKey = reader.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
        } else if (name == QStringLiteral("scrobbles")) {
            response.accepted = intAttribute(reader.attributes(), QStringLiteral("accepted"));
            response.ignored = intAttribute(reader.attributes(), QStringLiteral("ignored"));
        } else if (name == QStringLiteral("ignoredmessage")) {
            response.ignoredCodes.push_back(intAttribute(reader.attributes(), QStringLiteral("code"), 0));
        }
    }

    if (reader.hasError()) {
        response.parsed = false;
        response.ok = false;
    }
    return response;
}

FailureAction scrobbleFailureAction(const Response &response, bool networkError)
{
    if (networkError || !response.parsed) {
        return FailureAction::RetryLater;
    }
    if (response.ok) {
        return FailureAction::Ignore;
    }

    switch (response.errorCode) {
    case 8:
    case 11:
    case 16:
    case 29:
        return FailureAction::RetryLater;
    case 9:
        return FailureAction::Reauthenticate;
    case 4:
    case 10:
    case 13:
    case 26:
        return FailureAction::Disable;
    default:
        return FailureAction::DropSubmitted;
    }
}

QString trackTitle(const Track &track)
{
    return track.title.trimmed();
}

QString artistName(const Track &track)
{
    const QString artist = track.artistName.trimmed();
    return artist.isEmpty() ? track.albumArtistName.trimmed() : artist;
}

bool hasMinimumMetadata(const Track &track)
{
    return !trackTitle(track).isEmpty() && !artistName(track).isEmpty();
}

bool isKnownTooShortToScrobble(const Track &track)
{
    return track.durationMs > 0 && track.durationMs <= minimumDurationMs;
}

Scrobble scrobbleFromTrack(const Track &track, qint64 timestamp)
{
    Scrobble scrobble;
    scrobble.artist = artistName(track);
    scrobble.track = trackTitle(track);
    scrobble.timestamp = timestamp;
    scrobble.album = track.albumTitle.trimmed();
    scrobble.albumArtist = track.albumArtistName.trimmed();
    scrobble.trackNumber = track.trackNumber;
    scrobble.mbid = track.musicBrainz.recordingId.trimmed().isEmpty() ? track.musicBrainz.trackId.trimmed() : track.musicBrainz.recordingId.trimmed();
    if (track.durationMs > 0) {
        scrobble.durationSecs = static_cast<int>(track.durationMs / 1000);
    }
    return scrobble;
}

void addScrobbleParams(Params &params, const Scrobble &scrobble, int index)
{
    const QString suffix = QStringLiteral("[%1]").arg(index);
    addParam(params, QStringLiteral("artist%1").arg(suffix), scrobble.artist);
    addParam(params, QStringLiteral("track%1").arg(suffix), scrobble.track);
    addParam(params, QStringLiteral("timestamp%1").arg(suffix), QString::number(scrobble.timestamp));
    addParamIfPresent(params, QStringLiteral("album%1").arg(suffix), scrobble.album);
    if (!scrobble.albumArtist.trimmed().isEmpty()) {
        addParam(params, QStringLiteral("albumArtist%1").arg(suffix), scrobble.albumArtist.trimmed());
    }
    if (scrobble.trackNumber > 0) {
        addParam(params, QStringLiteral("trackNumber%1").arg(suffix), QString::number(scrobble.trackNumber));
    }
    addParamIfPresent(params, QStringLiteral("mbid%1").arg(suffix), scrobble.mbid);
    if (scrobble.durationSecs > 0) {
        addParam(params, QStringLiteral("duration%1").arg(suffix), QString::number(scrobble.durationSecs));
    }
}

} // namespace LastFmApi
