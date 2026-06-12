#include "scrobble/ListenBrainzScrobbler.h"

#include "Version.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <algorithm>

Q_LOGGING_CATEGORY(listenBrainzLog, "muzaiten.listenbrainz")

namespace {
constexpr int maxListensPerImport = 99;
constexpr int maxConsecutiveSubmissionFailures = 3;
constexpr auto apiUrl = "https://api.listenbrainz.org/1/submit-listens";
constexpr auto validateTokenUrl = "https://api.listenbrainz.org/1/validate-token";
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
    m_retryTimer = new QTimer(this);
    m_trackStartPlayingNowTimer = new QTimer(this);
    m_retryTimer->setInterval(60000);
    m_trackStartPlayingNowTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &ListenBrainzScrobbler::uploadBacklog);
    connect(m_trackStartPlayingNowTimer, &QTimer::timeout, this, &ListenBrainzScrobbler::submitPendingTrackStartPlayingNow);
}

ListenBrainzScrobbler::~ListenBrainzScrobbler() = default;

void ListenBrainzScrobbler::configure(bool enabled, bool uploadAllowed, const QString &token, const QString &historyPath)
{
    m_enabled = enabled;
    m_uploadAllowed = uploadAllowed;
    m_token = token.trimmed();
    m_consecutiveFailures = 0;
    if (m_historyPath != historyPath) {
        m_historyPath = historyPath;
        m_history = historyPath.isEmpty() ? nullptr : std::make_unique<ListenHistoryStore>(historyPath);
    }
    if (!canUpload()) {
        m_retryTimer->stop();
        return;
    }

    m_retryTimer->start();
    uploadBacklog();
}

void ListenBrainzScrobbler::trackStarted(const Track &track)
{
    m_currentTrack = track;
    m_hasCurrentTrack = true;
    m_playing = true;
    submitPlayingNowForTrackStart(track);
}

void ListenBrainzScrobbler::resumeTrack(const Track &track, qint64 elapsedMs, bool playing)
{
    Q_UNUSED(elapsedMs);
    m_currentTrack = track;
    m_hasCurrentTrack = true;
    m_playing = playing;
    if (m_playing) {
        submitPlayingNow(track);
    }
}

void ListenBrainzScrobbler::playbackStateChanged(bool playing)
{
    if (!m_hasCurrentTrack || m_playing == playing) {
        return;
    }

    m_playing = playing;
    if (m_playing) {
        // Re-send "playing now" on resume, but rate-limit to avoid flooding
        // ListenBrainz when the user rapidly toggles play/pause.
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (now - m_lastPlayingNowSecs >= kPlayingNowResubmitMinSecs) {
            submitPlayingNow(m_currentTrack);
        }
    }
}

void ListenBrainzScrobbler::uploadBacklog()
{
    if (!canUpload() || m_listenSubmissionInFlight || m_history == nullptr || !m_history->isOpen()) {
        return;
    }

    QJsonArray listens;
    QList<qint64> submittedIds;
    while (listens.isEmpty()) {
        const QList<ListenHistoryStore::Listen> rows = m_history->unsent(ListenHistoryStore::ListenBrainz, maxListensPerImport);
        if (rows.isEmpty()) {
            return;
        }
        // Rows the service can never accept (no artist/title) are marked sent
        // up front; left unsent at the head of the queue they would block
        // every valid listen behind them forever.
        QList<qint64> skippedIds;
        for (const ListenHistoryStore::Listen &row : rows) {
            if (!hasMinimumMetadata(row.track, /*warn=*/false)) {
                skippedIds.push_back(row.id);
                continue;
            }
            listens.append(listenObject(row.track, row.listenedAtSecs));
            submittedIds.push_back(row.id);
        }
        m_history->markSent(ListenHistoryStore::ListenBrainz, skippedIds);
        if (!skippedIds.isEmpty()) {
            emit backlogProcessed(0, static_cast<int>(skippedIds.size()), m_history->pendingCount(ListenHistoryStore::ListenBrainz));
        }
        if (skippedIds.isEmpty() && listens.isEmpty()) {
            return;
        }
    }

    QJsonObject body;
    body.insert(QStringLiteral("listen_type"), QStringLiteral("import"));
    body.insert(QStringLiteral("payload"), listens);
    submitPayload(body, SubmissionKind::Listen, submittedIds);
}

void ListenBrainzScrobbler::validateToken(const QString &token)
{
    QNetworkRequest request(QUrl(QString::fromLatin1(validateTokenUrl)));
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization", QStringLiteral("Token %1").arg(token.trimmed()).toUtf8());

    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QJsonObject body = QJsonDocument::fromJson(reply->isOpen() ? reply->readAll() : QByteArray()).object();
        const bool valid = reply->error() == QNetworkReply::NoError && body.value(QStringLiteral("valid")).toBool();
        emit tokenValidated(valid, body.value(QStringLiteral("user_name")).toString());
        reply->deleteLater();
    });
}

void ListenBrainzScrobbler::submitPlayingNow(const Track &track)
{
    if (!canUpload() || !hasMinimumMetadata(track)) {
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
    if (!m_playing || !m_hasCurrentTrack || track.path != m_currentTrack.path) {
        return;
    }

    submitPlayingNow(track);
}

void ListenBrainzScrobbler::submitPayload(const QJsonObject &payload, SubmissionKind kind, const QList<qint64> &submittedIds)
{
    QNetworkRequest request(QUrl(QString::fromLatin1(apiUrl)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization", QStringLiteral("Token %1").arg(m_token).toUtf8());

    if (kind == SubmissionKind::Listen) {
        m_listenSubmissionInFlight = true;
    }

    QNetworkReply *reply = m_network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, kind, submittedIds]() {
        handleSubmissionFinished(reply, kind, submittedIds);
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

bool ListenBrainzScrobbler::hasMinimumMetadata(const Track &track, bool warn) const
{
    const bool ok = !trackTitle(track).isEmpty() && !artistName(track).isEmpty();
    if (!ok && warn) {
        qCWarning(listenBrainzLog) << "cannot submit track without title and artist" << track.path;
    }
    return ok;
}

bool ListenBrainzScrobbler::canUpload() const
{
    return m_enabled && m_uploadAllowed && !m_token.isEmpty();
}

void ListenBrainzScrobbler::handleSubmissionFinished(QNetworkReply *reply, SubmissionKind kind, QList<qint64> submittedIds)
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
        if (status == 429) {
            // Rate limited: back off for the window the server reports
            // (X-RateLimit-Reset-In, seconds) and don't count it as a failure.
            bool parsedResetIn = false;
            const int resetInSecs = reply->rawHeader("X-RateLimit-Reset-In").toInt(&parsedResetIn);
            m_retryTimer->start(parsedResetIn && resetInSecs > 0 ? (resetInSecs + 1) * 1000 : 60000);
            return;
        }
        emit submissionFailed(message);
        // Only real listen submissions count toward permanent disablement.
        // 'playing now' updates are best-effort (a transient 429/500 on a
        // throttled ping must never disable scrobbling) — this mirrors
        // LastFmScrobbler, where only scrobble failures touch the counter.
        if (kind == SubmissionKind::Listen) {
            ++m_consecutiveFailures;
            if (m_consecutiveFailures >= maxConsecutiveSubmissionFailures) {
                disableScrobbling(QStringLiteral("ListenBrainz submissions failed %1 times. Scrobbling has been disabled.")
                                      .arg(maxConsecutiveSubmissionFailures));
            }
        }
        return;
    }

    m_consecutiveFailures = 0;
    if (m_enabled && m_retryTimer->interval() != 60000) {
        m_retryTimer->start(60000);
    }
    if (kind == SubmissionKind::Listen && !submittedIds.isEmpty() && m_history != nullptr) {
        m_history->markSent(ListenHistoryStore::ListenBrainz, submittedIds);
        emit backlogProcessed(static_cast<int>(submittedIds.size()), 0, m_history->pendingCount(ListenHistoryStore::ListenBrainz));
        // Keep draining until the backlog is empty.
        uploadBacklog();
    }

    reply->deleteLater();
}

void ListenBrainzScrobbler::disableScrobbling(const QString &message)
{
    m_enabled = false;
    m_retryTimer->stop();
    emit disabledAfterFailures(message);
}
