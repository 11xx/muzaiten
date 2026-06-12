#include "scrobble/LastFmScrobbler.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <algorithm>

Q_LOGGING_CATEGORY(lastFmLog, "muzaiten.lastfm")

namespace {

constexpr int maxScrobblesPerBatch = 50;
constexpr int maxConsecutiveSubmissionFailures = 3;
constexpr int authPollIntervalMs = 3000;
constexpr int maxAuthPollAttempts = 60;
// Minimum seconds between "now playing" resubmissions on play/resume, to
// prevent play-pause spam from flooding Last.fm with update requests.
constexpr qint64 kNowPlayingResubmitMinSecs = 30;
// Minimum seconds between eager "now playing" updates for new track starts.
// Rapid skips are coalesced so only the latest selected track is submitted.
constexpr qint64 kTrackStartNowPlayingMinSecs = 10;
constexpr int errorTokenNotAuthorized = 14;
constexpr int errorTokenExpired = 15;
constexpr auto apiRootUrl = "https://ws.audioscrobbler.com/2.0/";
constexpr auto authRootUrl = "https://www.last.fm/api/auth/";

void addCommonTrackParams(LastFmApi::Params &params, const LastFmApi::Scrobble &scrobble)
{
    LastFmApi::addParam(params, QStringLiteral("artist"), scrobble.artist);
    LastFmApi::addParam(params, QStringLiteral("track"), scrobble.track);
    LastFmApi::addParamIfPresent(params, QStringLiteral("album"), scrobble.album);
    if (!scrobble.albumArtist.trimmed().isEmpty()) {
        LastFmApi::addParam(params, QStringLiteral("albumArtist"), scrobble.albumArtist.trimmed());
    }
    if (scrobble.trackNumber > 0) {
        LastFmApi::addParam(params, QStringLiteral("trackNumber"), QString::number(scrobble.trackNumber));
    }
    LastFmApi::addParamIfPresent(params, QStringLiteral("mbid"), scrobble.mbid);
    if (scrobble.durationSecs > 0) {
        LastFmApi::addParam(params, QStringLiteral("duration"), QString::number(scrobble.durationSecs));
    }
}

QString responseErrorText(const LastFmApi::Response &response, const QString &transportMessage)
{
    if (response.parsed && !response.ok) {
        return QStringLiteral("Last.fm error %1: %2").arg(response.errorCode).arg(response.errorMessage);
    }
    return transportMessage.isEmpty() ? QStringLiteral("Last.fm request failed") : transportMessage;
}

} // namespace

LastFmScrobbler::LastFmScrobbler(QObject *parent)
    : QObject(parent)
{
    m_network = new QNetworkAccessManager(this);
    m_retryTimer = new QTimer(this);
    m_authPollTimer = new QTimer(this);
    m_trackStartNowPlayingTimer = new QTimer(this);
    m_retryTimer->setInterval(60000);
    m_authPollTimer->setInterval(authPollIntervalMs);
    m_trackStartNowPlayingTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, &LastFmScrobbler::uploadBacklog);
    connect(m_authPollTimer, &QTimer::timeout, this, &LastFmScrobbler::pollAuthSession);
    connect(m_trackStartNowPlayingTimer, &QTimer::timeout, this, &LastFmScrobbler::submitPendingTrackStartNowPlaying);
}

LastFmScrobbler::~LastFmScrobbler() = default;

void LastFmScrobbler::configure(bool enabled, bool uploadAllowed, const QString &apiKey, const QString &sharedSecret, const QString &sessionKey, const QString &historyPath)
{
    m_enabled = enabled;
    m_uploadAllowed = uploadAllowed;
    m_apiKey = apiKey.trimmed();
    m_sharedSecret = sharedSecret.trimmed();
    m_sessionKey = sessionKey.trimmed();
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

void LastFmScrobbler::trackStarted(const Track &track)
{
    m_currentTrack = track;
    m_hasCurrentTrack = true;
    m_playing = true;
    submitNowPlayingForTrackStart(track);
}

void LastFmScrobbler::resumeTrack(const Track &track, qint64 elapsedMs, bool playing)
{
    Q_UNUSED(elapsedMs);
    m_currentTrack = track;
    m_hasCurrentTrack = true;
    m_playing = playing;
    if (m_playing) {
        submitNowPlaying(track);
    }
}

void LastFmScrobbler::playbackStateChanged(bool playing)
{
    if (!m_hasCurrentTrack || m_playing == playing) {
        return;
    }

    m_playing = playing;
    if (m_playing) {
        // Re-send "now playing" on resume, but rate-limit to avoid flooding
        // Last.fm when the user rapidly toggles play/pause.
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (now - m_lastNowPlayingSecs >= kNowPlayingResubmitMinSecs) {
            submitNowPlaying(m_currentTrack);
        }
    }
}

void LastFmScrobbler::startAuthentication(const QString &apiKey, const QString &sharedSecret)
{
    const QString trimmedApiKey = apiKey.trimmed();
    const QString trimmedSecret = sharedSecret.trimmed();
    if (trimmedApiKey.isEmpty() || trimmedSecret.isEmpty()) {
        emit authenticationFailed(QStringLiteral("Last.fm API key and shared secret are required."));
        return;
    }

    m_authPollTimer->stop();
    m_authPollAttempts = 0;
    m_authRequestInFlight = false;
    m_pendingAuthToken.clear();
    m_pendingAuthApiKey = trimmedApiKey;
    m_pendingAuthSecret = trimmedSecret;

    LastFmApi::Params params;
    LastFmApi::addParam(params, QStringLiteral("method"), QStringLiteral("auth.getToken"));
    LastFmApi::addParam(params, QStringLiteral("api_key"), m_pendingAuthApiKey);
    postParams(params, RequestKind::AuthToken);
}

void LastFmScrobbler::cancelAuthentication()
{
    m_authPollTimer->stop();
    m_authPollAttempts = 0;
    m_authRequestInFlight = false;
    m_pendingAuthToken.clear();
}

void LastFmScrobbler::pollAuthSession()
{
    if (m_authRequestInFlight || m_pendingAuthToken.isEmpty()) {
        return;
    }
    if (m_authPollAttempts >= maxAuthPollAttempts) {
        cancelAuthentication();
        emit authenticationFailed(QStringLiteral("Timed out waiting for Last.fm authorization. Try logging in again."));
        return;
    }

    ++m_authPollAttempts;
    m_authRequestInFlight = true;
    LastFmApi::Params params;
    LastFmApi::addParam(params, QStringLiteral("method"), QStringLiteral("auth.getSession"));
    LastFmApi::addParam(params, QStringLiteral("api_key"), m_pendingAuthApiKey);
    LastFmApi::addParam(params, QStringLiteral("token"), m_pendingAuthToken);
    postParams(params, RequestKind::AuthSession);
}

void LastFmScrobbler::uploadBacklog()
{
    if (!canUpload() || m_scrobbleSubmissionInFlight || m_history == nullptr || !m_history->isOpen()) {
        return;
    }

    QList<LastFmApi::Scrobble> batch;
    QList<qint64> submittedIds;
    while (batch.isEmpty()) {
        const QList<ListenHistoryStore::Listen> rows = m_history->unsent(ListenHistoryStore::LastFm, maxScrobblesPerBatch);
        if (rows.isEmpty()) {
            return;
        }
        // Rows the service can never accept (no artist/title, or tracks under
        // the 30s scrobble minimum) are marked sent up front; left unsent at
        // the head of the queue they would block every valid listen forever.
        QList<qint64> skippedIds;
        for (const ListenHistoryStore::Listen &row : rows) {
            if (!LastFmApi::hasMinimumMetadata(row.track) || LastFmApi::isKnownTooShortToScrobble(row.track)) {
                skippedIds.push_back(row.id);
                continue;
            }
            batch.push_back(LastFmApi::scrobbleFromTrack(row.track, row.listenedAtSecs));
            submittedIds.push_back(row.id);
        }
        m_history->markSent(ListenHistoryStore::LastFm, skippedIds);
        if (!skippedIds.isEmpty()) {
            emit backlogProcessed(0, static_cast<int>(skippedIds.size()), m_history->pendingCount(ListenHistoryStore::LastFm));
        }
        if (skippedIds.isEmpty() && batch.isEmpty()) {
            return;
        }
    }

    LastFmApi::Params params;
    LastFmApi::addParam(params, QStringLiteral("method"), QStringLiteral("track.scrobble"));
    LastFmApi::addParam(params, QStringLiteral("api_key"), m_apiKey);
    LastFmApi::addParam(params, QStringLiteral("sk"), m_sessionKey);
    for (int index = 0; index < batch.size(); ++index) {
        LastFmApi::addScrobbleParams(params, batch.at(index), index);
    }
    m_scrobbleSubmissionInFlight = true;
    postParams(params, RequestKind::Scrobble, submittedIds);
}

void LastFmScrobbler::submitNowPlaying(const Track &track)
{
    if (!canUpload()) {
        return;
    }
    if (!LastFmApi::hasMinimumMetadata(track)) {
        qCWarning(lastFmLog) << "cannot submit Last.fm now-playing without tagged title and artist" << track.path;
        return;
    }
    if (LastFmApi::isKnownTooShortToScrobble(track)) {
        return;
    }

    m_lastNowPlayingSecs = QDateTime::currentSecsSinceEpoch();
    LastFmApi::Params params;
    LastFmApi::addParam(params, QStringLiteral("method"), QStringLiteral("track.updateNowPlaying"));
    LastFmApi::addParam(params, QStringLiteral("api_key"), m_apiKey);
    LastFmApi::addParam(params, QStringLiteral("sk"), m_sessionKey);
    addCommonTrackParams(params, LastFmApi::scrobbleFromTrack(track, 0));
    postParams(params, RequestKind::NowPlaying);
}

void LastFmScrobbler::submitNowPlayingForTrackStart(const Track &track)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (m_lastNowPlayingSecs == 0 || now - m_lastNowPlayingSecs >= kTrackStartNowPlayingMinSecs) {
        m_hasPendingTrackStartNowPlaying = false;
        m_trackStartNowPlayingTimer->stop();
        submitNowPlaying(track);
        return;
    }

    m_pendingTrackStartNowPlaying = track;
    m_hasPendingTrackStartNowPlaying = true;
    const qint64 delayMs = std::max<qint64>(1, (kTrackStartNowPlayingMinSecs - (now - m_lastNowPlayingSecs)) * 1000);
    m_trackStartNowPlayingTimer->start(static_cast<int>(delayMs));
}

void LastFmScrobbler::submitPendingTrackStartNowPlaying()
{
    if (!m_hasPendingTrackStartNowPlaying) {
        return;
    }

    const Track track = m_pendingTrackStartNowPlaying;
    m_hasPendingTrackStartNowPlaying = false;
    if (!m_playing || !m_hasCurrentTrack || track.path != m_currentTrack.path) {
        return;
    }

    submitNowPlaying(track);
}

void LastFmScrobbler::postParams(LastFmApi::Params params, RequestKind kind, const QList<qint64> &submittedIds)
{
    const QString secret = (kind == RequestKind::AuthToken || kind == RequestKind::AuthSession) ? m_pendingAuthSecret : m_sharedSecret;
    LastFmApi::addParam(params, QStringLiteral("api_sig"), LastFmApi::signature(params, secret));

    QNetworkRequest request(QUrl(QString::fromLatin1(apiRootUrl)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept", "application/xml");

    QNetworkReply *reply = m_network->post(request, LastFmApi::formBody(params));
    connect(reply, &QNetworkReply::finished, this, [this, reply, kind, submittedIds]() {
        handleRequestFinished(reply, kind, submittedIds);
    });
}

void LastFmScrobbler::handleRequestFinished(QNetworkReply *reply, RequestKind kind, const QList<qint64> &submittedIds)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->isOpen() ? reply->readAll() : QByteArray();
    const LastFmApi::Response response = LastFmApi::parseXml(body);
    const QString errorString = reply->errorString();
    const QString statusText = status > 0 ? QString::number(status) : QStringLiteral("network");
    const QString transportMessage = QStringLiteral("Last.fm request failed (%1): %2").arg(statusText, errorString);
    const bool networkError = reply->error() != QNetworkReply::NoError && !response.parsed;

    if (kind == RequestKind::Scrobble) {
        m_scrobbleSubmissionInFlight = false;
    }

    reply->deleteLater();

    switch (kind) {
    case RequestKind::AuthToken:
        handleAuthTokenResponse(response);
        break;
    case RequestKind::AuthSession:
        handleAuthSessionResponse(response);
        break;
    case RequestKind::NowPlaying:
        handleNowPlayingResponse(response, transportMessage);
        break;
    case RequestKind::Scrobble:
        handleScrobbleResponse(response, networkError, transportMessage, submittedIds);
        break;
    }
}

void LastFmScrobbler::handleAuthTokenResponse(const LastFmApi::Response &response)
{
    if (!response.parsed || !response.ok || response.token.isEmpty()) {
        emit authenticationFailed(responseErrorText(response, QStringLiteral("Last.fm could not create an authentication token.")));
        return;
    }

    m_pendingAuthToken = response.token;
    QUrl url(QString::fromLatin1(authRootUrl));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("api_key"), m_pendingAuthApiKey);
    query.addQueryItem(QStringLiteral("token"), m_pendingAuthToken);
    url.setQuery(query);
    emit authenticationUrlReady(url);

    // Poll auth.getSession until the user authorizes in the browser, so the
    // session is picked up automatically with no further interaction.
    m_authPollAttempts = 0;
    m_authPollTimer->start();
}

void LastFmScrobbler::handleAuthSessionResponse(const LastFmApi::Response &response)
{
    m_authRequestInFlight = false;

    if (response.parsed && response.ok && !response.sessionKey.isEmpty()) {
        const QString sessionName = response.sessionName;
        const QString sessionKey = response.sessionKey;
        cancelAuthentication();
        emit authenticationSucceeded(sessionName, sessionKey);
        return;
    }

    // The token is not authorized yet; keep polling until the user finishes
    // in the browser or the attempt budget runs out.
    if (response.parsed && !response.ok && response.errorCode == errorTokenNotAuthorized) {
        return;
    }

    // An expired token or any other hard failure ends the attempt.
    if (response.parsed && !response.ok && response.errorCode == errorTokenExpired) {
        cancelAuthentication();
        emit authenticationFailed(QStringLiteral("Last.fm authorization expired before it completed. Try logging in again."));
        return;
    }
    if (response.parsed && !response.ok) {
        cancelAuthentication();
        emit authenticationFailed(responseErrorText(response, QStringLiteral("Last.fm authentication did not complete.")));
        return;
    }

    // Unparsed/network errors are transient; let the poll timer try again.
}

void LastFmScrobbler::handleNowPlayingResponse(const LastFmApi::Response &response, const QString &transportMessage)
{
    if (response.parsed && response.ok) {
        return;
    }

    const LastFmApi::FailureAction action = LastFmApi::scrobbleFailureAction(response, !response.parsed);
    if (action == LastFmApi::FailureAction::Reauthenticate) {
        disableScrobbling(QStringLiteral("Last.fm session was rejected. Re-authenticate Last.fm scrobbling."));
        return;
    }
    if (action == LastFmApi::FailureAction::Disable) {
        disableScrobbling(responseErrorText(response, transportMessage));
        return;
    }

    const QString message = responseErrorText(response, transportMessage);
    qCWarning(lastFmLog) << message;
    emit submissionFailed(message);
}

void LastFmScrobbler::handleScrobbleResponse(const LastFmApi::Response &response,
                                             bool networkError,
                                             const QString &transportMessage,
                                             const QList<qint64> &submittedIds)
{
    if (response.parsed && response.ok) {
        m_consecutiveFailures = 0;
        if (m_history != nullptr) {
            m_history->markSent(ListenHistoryStore::LastFm, submittedIds);
            emit backlogProcessed(static_cast<int>(submittedIds.size()), 0, m_history->pendingCount(ListenHistoryStore::LastFm));
        }
        // Keep draining until the backlog is empty.
        uploadBacklog();
        return;
    }

    const LastFmApi::FailureAction action = LastFmApi::scrobbleFailureAction(response, networkError);
    const QString message = responseErrorText(response, transportMessage);
    qCWarning(lastFmLog) << message;

    switch (action) {
    case LastFmApi::FailureAction::RetryLater:
        ++m_consecutiveFailures;
        emit submissionFailed(message);
        if (m_consecutiveFailures >= maxConsecutiveSubmissionFailures) {
            disableScrobbling(QStringLiteral("Last.fm submissions failed %1 times. Scrobbling has been disabled.")
                                  .arg(maxConsecutiveSubmissionFailures));
        }
        return;
    case LastFmApi::FailureAction::DropSubmitted:
        m_consecutiveFailures = 0;
        if (m_history != nullptr) {
            m_history->markSent(ListenHistoryStore::LastFm, submittedIds);
        }
        emit submissionFailed(message);
        return;
    case LastFmApi::FailureAction::Reauthenticate:
        disableScrobbling(QStringLiteral("Last.fm session was rejected. Re-authenticate Last.fm scrobbling."));
        return;
    case LastFmApi::FailureAction::Disable:
        disableScrobbling(message);
        return;
    case LastFmApi::FailureAction::Ignore:
        return;
    }
}

void LastFmScrobbler::disableScrobbling(const QString &message)
{
    m_enabled = false;
    m_retryTimer->stop();
    emit disabledAfterFailures(message);
}

bool LastFmScrobbler::canUpload() const
{
    return m_enabled && m_uploadAllowed && hasCredentials();
}

bool LastFmScrobbler::hasCredentials() const
{
    return !m_apiKey.isEmpty() && !m_sharedSecret.isEmpty() && !m_sessionKey.isEmpty();
}
