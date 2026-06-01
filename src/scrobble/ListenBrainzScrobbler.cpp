#include "scrobble/ListenBrainzScrobbler.h"

#include "Version.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <algorithm>

Q_LOGGING_CATEGORY(listenBrainzLog, "muzaiten.listenbrainz")

namespace {
constexpr qint64 maxRequiredListenMs = 4 * 60 * 1000;
constexpr int maxListensPerImport = 99;
constexpr int maxConsecutiveSubmissionFailures = 3;
constexpr auto apiUrl = "https://api.listenbrainz.org/1/submit-listens";
// Minimum seconds between "playing_now" resubmissions on play/resume, to
// prevent play-pause spam from flooding ListenBrainz with update requests.
constexpr qint64 kPlayingNowResubmitMinSecs = 30;
// Minimum seconds between eager "playing_now" updates for new track starts.
// Rapid skips are coalesced so only the latest selected track is submitted.
constexpr qint64 kTrackStartPlayingNowMinSecs = 10;

QString trackTitle(const Track &track)
{
    return track.title.trimmed().isEmpty() ? track.filename : track.title.trimmed();
}

QString artistName(const Track &track)
{
    return track.artistName.trimmed().isEmpty() ? track.albumArtistName.trimmed() : track.artistName.trimmed();
}

void addStringIfPresent(QJsonObject &object, const QString &key, const QString &value)
{
    if (!value.trimmed().isEmpty()) {
        object.insert(key, value.trimmed());
    }
}

void addArrayIfPresent(QJsonObject &object, const QString &key, const QString &value)
{
    if (!value.trimmed().isEmpty()) {
        object.insert(key, QJsonArray{value.trimmed()});
    }
}

} // namespace

ListenBrainzScrobbler::ListenBrainzScrobbler(QObject *parent)
    : QObject(parent)
{
    m_network = new QNetworkAccessManager(this);
    m_progressTimer = new QTimer(this);
    m_retryTimer = new QTimer(this);
    m_trackStartPlayingNowTimer = new QTimer(this);
    m_progressTimer->setInterval(1000);
    m_retryTimer->setInterval(60000);
    m_trackStartPlayingNowTimer->setSingleShot(true);
    connect(m_progressTimer, &QTimer::timeout, this, &ListenBrainzScrobbler::checkListenProgress);
    connect(m_retryTimer, &QTimer::timeout, this, &ListenBrainzScrobbler::retryPending);
    connect(m_trackStartPlayingNowTimer, &QTimer::timeout, this, &ListenBrainzScrobbler::submitPendingTrackStartPlayingNow);
}

void ListenBrainzScrobbler::configure(bool enabled, const QString &token, const QString &cachePath)
{
    m_enabled = enabled;
    m_token = token.trimmed();
    m_consecutiveFailures = 0;
    if (m_cachePath != cachePath) {
        m_cachePath = cachePath;
        loadPending();
    }

    if (!m_enabled || m_token.isEmpty()) {
        m_retryTimer->stop();
        return;
    }

    m_retryTimer->start();
    retryPending();
}

void ListenBrainzScrobbler::trackStarted(const Track &track)
{
    m_currentTrack = track;
    m_hasCurrentTrack = true;
    m_playing = true;
    m_listenSubmitted = false;
    m_listenTimestampSecs = QDateTime::currentSecsSinceEpoch();
    m_requiredMs = requiredListenMs(track);
    m_accumulatedMs = 0;
    m_segmentTimer.restart();
    m_progressTimer->start();

    submitPlayingNowForTrackStart(track);
}

void ListenBrainzScrobbler::resumeTrack(const Track &track, qint64 elapsedMs, bool playing)
{
    m_currentTrack = track;
    m_hasCurrentTrack = true;
    m_playing = playing;
    m_requiredMs = requiredListenMs(track);
    m_accumulatedMs = std::max<qint64>(0, elapsedMs);
    // Anchor the listen timestamp to when the track originally began so that
    // the completed listen reports the real start time.
    m_listenTimestampSecs = QDateTime::currentSecsSinceEpoch() - m_accumulatedMs / 1000;
    // If we already passed the listen threshold assume it was submitted by the
    // prior session — mark submitted to avoid a duplicate.
    m_listenSubmitted = (m_accumulatedMs >= m_requiredMs);
    m_segmentTimer.restart();

    if (m_playing && !m_listenSubmitted) {
        m_progressTimer->start();
        submitPlayingNow(track);
    } else {
        m_progressTimer->stop();
    }
}

void ListenBrainzScrobbler::playbackStateChanged(bool playing)
{
    if (!m_hasCurrentTrack || m_playing == playing) {
        return;
    }

    if (!playing && m_segmentTimer.isValid()) {
        m_accumulatedMs += m_segmentTimer.elapsed();
    } else if (playing) {
        m_segmentTimer.restart();
    }

    m_playing = playing;
    if (m_playing && !m_listenSubmitted) {
        m_progressTimer->start();
        // Re-send "playing now" on resume, but rate-limit to avoid flooding
        // ListenBrainz when the user rapidly toggles play/pause.
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (now - m_lastPlayingNowSecs >= kPlayingNowResubmitMinSecs) {
            submitPlayingNow(m_currentTrack);
        }
    }
}

void ListenBrainzScrobbler::checkListenProgress()
{
    if (!m_enabled || !m_playing || !m_hasCurrentTrack || m_listenSubmitted) {
        return;
    }

    if (playedMs() >= m_requiredMs) {
        submitCompletedListen();
    }
}

void ListenBrainzScrobbler::retryPending()
{
    if (!m_enabled || m_token.isEmpty() || m_pendingListens.isEmpty() || m_listenSubmissionInFlight) {
        return;
    }

    const QList<QJsonObject> submitted = m_pendingListens.mid(0, maxListensPerImport);
    QJsonArray listens;
    for (const QJsonObject &listen : submitted) {
        listens.append(listen);
    }

    QJsonObject body;
    body.insert(QStringLiteral("listen_type"), QStringLiteral("import"));
    body.insert(QStringLiteral("payload"), listens);
    submitPayload(body, SubmissionKind::Listen);
}

void ListenBrainzScrobbler::submitPlayingNow(const Track &track)
{
    if (!m_enabled || m_token.isEmpty() || !hasMinimumMetadata(track)) {
        return;
    }

    m_lastPlayingNowSecs = QDateTime::currentSecsSinceEpoch();
    QJsonArray payload;
    QJsonObject nowPlaying;
    nowPlaying.insert(QStringLiteral("track_metadata"), metadataObject(track));
    payload.append(nowPlaying);

    QJsonObject body;
    body.insert(QStringLiteral("listen_type"), QStringLiteral("playing_now"));
    body.insert(QStringLiteral("payload"), payload);
    submitPayload(body, SubmissionKind::PlayingNow);
}

void ListenBrainzScrobbler::submitPlayingNowForTrackStart(const Track &track)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (m_lastPlayingNowSecs == 0 || now - m_lastPlayingNowSecs >= kTrackStartPlayingNowMinSecs) {
        m_hasPendingTrackStartPlayingNow = false;
        m_trackStartPlayingNowTimer->stop();
        submitPlayingNow(track);
        return;
    }

    m_pendingTrackStartPlayingNow = track;
    m_hasPendingTrackStartPlayingNow = true;
    const qint64 delayMs = std::max<qint64>(1, (kTrackStartPlayingNowMinSecs - (now - m_lastPlayingNowSecs)) * 1000);
    m_trackStartPlayingNowTimer->start(static_cast<int>(delayMs));
}

void ListenBrainzScrobbler::submitPendingTrackStartPlayingNow()
{
    if (!m_hasPendingTrackStartPlayingNow) {
        return;
    }

    const Track track = m_pendingTrackStartPlayingNow;
    m_hasPendingTrackStartPlayingNow = false;
    if (!m_playing || m_listenSubmitted || !m_hasCurrentTrack || track.path != m_currentTrack.path) {
        return;
    }

    submitPlayingNow(track);
}

void ListenBrainzScrobbler::submitCompletedListen()
{
    if (!m_hasCurrentTrack || m_listenSubmitted) {
        return;
    }

    m_listenSubmitted = true;
    const QJsonObject listen = listenObject(m_currentTrack, m_listenTimestampSecs);
    if (!m_enabled || m_token.isEmpty() || !hasMinimumMetadata(m_currentTrack)) {
        return;
    }

    cachePendingListen(listen);
    retryPending();
}

void ListenBrainzScrobbler::submitPayload(const QJsonObject &payload, SubmissionKind kind)
{
    QNetworkRequest request(QUrl(QString::fromLatin1(apiUrl)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization", QStringLiteral("Token %1").arg(m_token).toUtf8());

    QList<QJsonObject> submittedListens;
    if (kind == SubmissionKind::Listen) {
        m_listenSubmissionInFlight = true;
        const QJsonArray listens = payload.value(QStringLiteral("payload")).toArray();
        for (const QJsonValue &listen : listens) {
            submittedListens.push_back(listen.toObject());
        }
    }

    QNetworkReply *reply = m_network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, kind, submittedListens]() {
        handleSubmissionFinished(reply, kind, submittedListens);
    });
}

QJsonObject ListenBrainzScrobbler::listenObject(const Track &track, qint64 listenedAt) const
{
    QJsonObject listen;
    listen.insert(QStringLiteral("listened_at"), listenedAt);
    listen.insert(QStringLiteral("track_metadata"), metadataObject(track));
    return listen;
}

QJsonObject ListenBrainzScrobbler::metadataObject(const Track &track) const
{
    QJsonObject metadata;
    metadata.insert(QStringLiteral("artist_name"), artistName(track));
    metadata.insert(QStringLiteral("track_name"), trackTitle(track));
    addStringIfPresent(metadata, QStringLiteral("release_name"), track.albumTitle);
    metadata.insert(QStringLiteral("additional_info"), additionalInfoObject(track));
    return metadata;
}

QJsonObject ListenBrainzScrobbler::additionalInfoObject(const Track &track) const
{
    QJsonObject additionalInfo;
    if (!track.artistName.trimmed().isEmpty()) {
        additionalInfo.insert(QStringLiteral("artist_names"), QJsonArray{track.artistName.trimmed()});
    }
    addArrayIfPresent(additionalInfo, QStringLiteral("artist_mbids"), track.musicBrainz.artistId);
    addStringIfPresent(additionalInfo, QStringLiteral("release_mbid"), track.musicBrainz.releaseId);
    addStringIfPresent(additionalInfo, QStringLiteral("recording_mbid"), track.musicBrainz.recordingId);
    addStringIfPresent(additionalInfo, QStringLiteral("track_mbid"), track.musicBrainz.trackId);
    addArrayIfPresent(additionalInfo, QStringLiteral("work_mbids"), track.musicBrainz.workId);
    if (track.trackNumber > 0) {
        additionalInfo.insert(QStringLiteral("tracknumber"), QString::number(track.trackNumber));
    }
    if (track.durationMs > 0) {
        additionalInfo.insert(QStringLiteral("duration_ms"), track.durationMs);
    }
    additionalInfo.insert(QStringLiteral("media_player"), QStringLiteral("muzaiten"));
    additionalInfo.insert(QStringLiteral("submission_client"), QStringLiteral("muzaiten"));
    additionalInfo.insert(QStringLiteral("submission_client_version"), QStringLiteral(MUZAITEN_VERSION));
    return additionalInfo;
}

bool ListenBrainzScrobbler::hasMinimumMetadata(const Track &track) const
{
    const bool ok = !trackTitle(track).isEmpty() && !artistName(track).isEmpty();
    if (!ok) {
        qCWarning(listenBrainzLog) << "cannot submit track without title and artist" << track.path;
    }
    return ok;
}

qint64 ListenBrainzScrobbler::requiredListenMs(const Track &track) const
{
    if (track.durationMs <= 0) {
        return maxRequiredListenMs;
    }
    return std::min(track.durationMs / 2, maxRequiredListenMs);
}

qint64 ListenBrainzScrobbler::playedMs() const
{
    return m_accumulatedMs + (m_playing && m_segmentTimer.isValid() ? m_segmentTimer.elapsed() : 0);
}

void ListenBrainzScrobbler::loadPending()
{
    m_pendingListens.clear();
    QFile file(m_cachePath);
    if (m_cachePath.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QJsonArray listens = QJsonDocument::fromJson(file.readAll()).array();
    for (const QJsonValue &listen : listens) {
        if (listen.isObject()) {
            m_pendingListens.push_back(listen.toObject());
        }
    }
}

void ListenBrainzScrobbler::savePending() const
{
    if (m_cachePath.isEmpty()) {
        return;
    }

    QFileInfo info(m_cachePath);
    QDir().mkpath(info.absolutePath());
    QFile file(m_cachePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(listenBrainzLog) << "failed to write pending ListenBrainz cache" << m_cachePath;
        return;
    }

    QJsonArray listens;
    for (const QJsonObject &listen : m_pendingListens) {
        listens.append(listen);
    }
    file.write(QJsonDocument(listens).toJson(QJsonDocument::Compact));
}

void ListenBrainzScrobbler::cachePendingListen(const QJsonObject &listen)
{
    m_pendingListens.push_back(listen);
    savePending();
}

void ListenBrainzScrobbler::handleSubmissionFinished(QNetworkReply *reply, SubmissionKind kind, QList<QJsonObject> submittedListens)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString errorString = reply->errorString();
    if (kind == SubmissionKind::Listen) {
        m_listenSubmissionInFlight = false;
    }
    const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
    if (!ok) {
        const QString statusText = status > 0 ? QString::number(status) : QStringLiteral("network");
        const QString message = QStringLiteral("ListenBrainz submission failed (%1): %2").arg(statusText, errorString);
        qCWarning(listenBrainzLog) << message;
        reply->deleteLater();
        if (status == 401 || status == 403) {
            disableScrobbling(QStringLiteral("ListenBrainz token was rejected. Scrobbling has been disabled."));
            return;
        }
        ++m_consecutiveFailures;
        emit submissionFailed(message);
        if (m_consecutiveFailures >= maxConsecutiveSubmissionFailures) {
            disableScrobbling(QStringLiteral("ListenBrainz submissions failed %1 times. Scrobbling has been disabled.")
                                  .arg(maxConsecutiveSubmissionFailures));
        }
        return;
    }

    m_consecutiveFailures = 0;
    if (kind == SubmissionKind::Listen && !submittedListens.isEmpty()) {
        m_pendingListens.erase(m_pendingListens.begin(), m_pendingListens.begin() + std::min(submittedListens.size(), m_pendingListens.size()));
        savePending();
    }

    reply->deleteLater();
}

void ListenBrainzScrobbler::disableScrobbling(const QString &message)
{
    m_enabled = false;
    m_retryTimer->stop();
    emit disabledAfterFailures(message);
}
