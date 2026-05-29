#pragma once

#include "core/Track.h"

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QVector>

namespace LastFmApi {

using Params = QList<QPair<QString, QString>>;

struct Scrobble {
    QString artist;
    QString track;
    qint64 timestamp = 0;
    QString album;
    QString albumArtist;
    int trackNumber = 0;
    QString mbid;
    int durationSecs = 0;
};

struct Response {
    bool parsed = false;
    bool ok = false;
    int errorCode = 0;
    QString errorMessage;
    int accepted = -1;
    int ignored = -1;
    QVector<int> ignoredCodes;
    QString token;
    QString sessionName;
    QString sessionKey;
};

enum class FailureAction {
    RetryLater,
    DropSubmitted,
    Disable,
    Reauthenticate,
    Ignore,
};

void addParam(Params &params, const QString &key, const QString &value);
void addParamIfPresent(Params &params, const QString &key, const QString &value);
QString signature(const Params &params, const QString &secret);
QByteArray formBody(const Params &params);
Response parseXml(const QByteArray &body);
FailureAction scrobbleFailureAction(const Response &response, bool networkError);
QString trackTitle(const Track &track);
QString artistName(const Track &track);
bool hasMinimumMetadata(const Track &track);
bool isKnownTooShortToScrobble(const Track &track);
Scrobble scrobbleFromTrack(const Track &track, qint64 timestamp);
void addScrobbleParams(Params &params, const Scrobble &scrobble, int index);

} // namespace LastFmApi
