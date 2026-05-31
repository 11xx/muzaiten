#include "scrobble/LastFmScrobbler.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <algorithm>

Q_LOGGING_CATEGORY(lastFmLog, "muzaiten.lastfm")

namespace {

constexpr qint64 maxRequiredListenMs = 4 * 60 * 1000;
constexpr int maxScrobblesPerBatch = 50;
constexpr int maxConsecutiveSubmissionFailures = 3;
constexpr int authPollIntervalMs = 3000;
constexpr int maxAuthPollAttempts = 60;
// Minimum seconds between "now playing" resubmissions on play/resume, to
// prevent play-pause spam from flooding Last.fm with update requests.
constexpr qint64 kNowPlayingResubmitMinSecs = 30;
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

QJsonObject scrobbleToJson(const LastFmApi::Scrobble &scrobble)
{
    QJsonObject object;
    object.insert(QStringLiteral("artist"), scrobble.artist);
    object.insert(QStringLiteral("track"), scrobble.track);
    object.insert(QStringLiteral("timestamp"), scrobble.timestamp);
    if (!scrobble.album.isEmpty()) {
        object.insert(QStringLiteral("album"), scrobble.album);
    }
    if (!scrobble.albumArtist.isEmpty()) {
        object.insert(QStringLiteral("albumArtist"), scrobble.albumArtist);
    }
    if (scrobble.trackNumber > 0) {
        object.insert(QStringLiteral("trackNumber"), scrobble.trackNumber);
    }
    if (!scrobble.mbid.isEmpty()) {
        object.insert(QStringLiteral("mbid"), scrobble.mbid);
    }
    if (scrobble.durationSecs > 0) {
        object.insert(QStringLiteral("duration"), scrobble.durationSecs);
    }
    return object;
}

LastFmApi::Scrobble scrobbleFromJson(const QJsonObject &object)
{
    LastFmApi::Scrobble scrobble;
    scrobble.artist = object.value(QStringLiteral("artist")).toString().trimmed();
    scrobble.track = object.value(QStringLiteral("track")).toString().trimmed();
    scrobble.timestamp = static_cast<qint64>(object.value(QStringLiteral("timestamp")).toDouble());
    scrobble.album = object.value(QStringLiteral("album")).toString().trimmed();
    scrobble.albumArtist = object.value(QStringLiteral("albumArtist")).toString().trimmed();
    scrobble.trackNumber = object.value(QStringLiteral("trackNumber")).toInt();
    scrobble.mbid = object.value(QStringLiteral("mbid")).toString().trimmed();
    scrobble.durationSecs = object.value(QStringLiteral("duration")).toInt();
    return scrobble;
}

bool isValidCachedScrobble(const LastFmApi::Scrobble &scrobble)
{
    return !scrobble.artist.isEmpty() && !scrobble.track.isEmpty() && scrobble.timestamp > 0;
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
    m_progressTimer = new QTimer(this);
    m_retryTimer = new QTimer(this);
    m_authPollTimer = new QTimer(this);
    m_progressTimer->setInterval(1000);
    m_retryTimer->setInterval(60000);
    m_authPollTimer->setInterval(authPollIntervalMs);
    connect(m_progressTimer, &QTimer::timeout, this, &LastFmScrobbler::checkListenProgress);
    connect(m_retryTimer, &QTimer::timeout, this, &LastFmScrobbler::retryPending);
    connect(m_authPollTimer, &QTimer::timeout, this, &LastFmScrobbler::pollAuthSession);
}

void LastFmScrobbler::configure(bool enabled, const QString &apiKey, const QString &sharedSecret, const QString &sessionKey, const QString &cachePath)
{
    m_enabled = enabled;
    m_apiKey = apiKey.trimmed();
    m_sharedSecret = sharedSecret.trimmed();
    m_sessionKey = sessionKey.trimmed();
    m_consecutiveFailures = 0;
    if (m_cachePath != cachePath) {
        m_cachePath = cachePath;
        loadPending();
    }

    if (!m_enabled || !hasCredentials()) {
        m_retryTimer->stop();
        return;
    }

    m_retryTimer->start();
    retryPending();
}

void LastFmScrobbler::trackStarted(const Track &track)
{
    m_currentTrack = track;
    m_hasCurrentTrack = true;
    m_playing = true;
    m_scrobbleSubmitted = false;
    m_scrobbleTimestampSecs = QDateTime::currentSecsSinceEpoch();
    m_requiredMs = requiredListenMs(track);
    m_accumulatedMs = 0;
    m_segmentTimer.restart();
    m_progressTimer->start();

    submitNowPlaying(track);
}

void LastFmScrobbler::resumeTrack(const Track &track, qint64 elapsedMs, bool playing)
{
    m_currentTrack = track;
    m_hasCurrentTrack = true;
    m_playing = playing;
    m_requiredMs = requiredListenMs(track);
    m_accumulatedMs = std::max<qint64>(0, elapsedMs);
    // Anchor the scrobble timestamp to when the track originally began so that
    // the completed-scrobble reports the real start time.
    m_scrobbleTimestampSecs = QDateTime::currentSecsSinceEpoch() - m_accumulatedMs / 1000;
    // If we already passed the listen threshold assume it was scrobbled by the
    // prior session — mark submitted to avoid a duplicate.
    m_scrobbleSubmitted = (m_accumulatedMs >= m_requiredMs);
    m_segmentTimer.restart();

    if (m_playing && !m_scrobbleSubmitted) {
        m_progressTimer->start();
        submitNowPlaying(track);
    } else {
        m_progressTimer->stop();
    }
}

void LastFmScrobbler::playbackStateChanged(bool playing)
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
    if (m_playing && !m_scrobbleSubmitted) {
        m_progressTimer->start();
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

void LastFmScrobbler::checkListenProgress()
{
    if (!m_enabled || !m_playing || !m_hasCurrentTrack || m_scrobbleSubmitted) {
        return;
    }

    if (playedMs() >= m_requiredMs) {
        submitCompletedScrobble();
    }
}

void LastFmScrobbler::retryPending()
{
    if (!m_enabled || !hasCredentials() || m_pendingScrobbles.isEmpty() || m_scrobbleSubmissionInFlight) {
        return;
    }

    submitScrobbleBatch(m_pendingScrobbles.mid(0, maxScrobblesPerBatch));
}

void LastFmScrobbler::submitNowPlaying(const Track &track)
{
    if (!m_enabled || !hasCredentials()) {
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
    addCommonTrackParams(params, LastFmApi::scrobbleFromTrack(track, m_scrobbleTimestampSecs));
    postParams(params, RequestKind::NowPlaying);
}

void LastFmScrobbler::submitCompletedScrobble()
{
    if (!m_hasCurrentTrack || m_scrobbleSubmitted) {
        return;
    }

    m_scrobbleSubmitted = true;
    if (!m_enabled || !hasCredentials()) {
        return;
    }
    if (!LastFmApi::hasMinimumMetadata(m_currentTrack)) {
        qCWarning(lastFmLog) << "cannot scrobble Last.fm track without tagged title and artist" << m_currentTrack.path;
        return;
    }
    if (LastFmApi::isKnownTooShortToScrobble(m_currentTrack)) {
        return;
    }

    cachePendingScrobble(LastFmApi::scrobbleFromTrack(m_currentTrack, m_scrobbleTimestampSecs));
    retryPending();
}

void LastFmScrobbler::submitScrobbleBatch(const QList<LastFmApi::Scrobble> &submitted)
{
    if (submitted.isEmpty()) {
        return;
    }

    LastFmApi::Params params;
    LastFmApi::addParam(params, QStringLiteral("method"), QStringLiteral("track.scrobble"));
    LastFmApi::addParam(params, QStringLiteral("api_key"), m_apiKey);
    LastFmApi::addParam(params, QStringLiteral("sk"), m_sessionKey);
    for (int index = 0; index < submitted.size(); ++index) {
        LastFmApi::addScrobbleParams(params, submitted.at(index), index);
    }
    m_scrobbleSubmissionInFlight = true;
    postParams(params, RequestKind::Scrobble, static_cast<int>(submitted.size()));
}

void LastFmScrobbler::postParams(LastFmApi::Params params, RequestKind kind, int submittedScrobbleCount)
{
    const QString secret = (kind == RequestKind::AuthToken || kind == RequestKind::AuthSession) ? m_pendingAuthSecret : m_sharedSecret;
    LastFmApi::addParam(params, QStringLiteral("api_sig"), LastFmApi::signature(params, secret));

    QNetworkRequest request(QUrl(QString::fromLatin1(apiRootUrl)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept", "application/xml");

    QNetworkReply *reply = m_network->post(request, LastFmApi::formBody(params));
    connect(reply, &QNetworkReply::finished, this, [this, reply, kind, submittedScrobbleCount]() {
        handleRequestFinished(reply, kind, submittedScrobbleCount);
    });
}

void LastFmScrobbler::handleRequestFinished(QNetworkReply *reply, RequestKind kind, int submittedScrobbleCount)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
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
        handleScrobbleResponse(response, networkError, transportMessage, submittedScrobbleCount);
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
                                             int submittedScrobbleCount)
{
    if (response.parsed && response.ok) {
        m_consecutiveFailures = 0;
        removeSubmittedPrefix(submittedScrobbleCount);
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
        removeSubmittedPrefix(submittedScrobbleCount);
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

qint64 LastFmScrobbler::requiredListenMs(const Track &track) const
{
    if (track.durationMs <= 0) {
        return maxRequiredListenMs;
    }
    return std::min(track.durationMs / 2, maxRequiredListenMs);
}

qint64 LastFmScrobbler::playedMs() const
{
    return m_accumulatedMs + (m_playing && m_segmentTimer.isValid() ? m_segmentTimer.elapsed() : 0);
}

void LastFmScrobbler::loadPending()
{
    m_pendingScrobbles.clear();
    QFile file(m_cachePath);
    if (m_cachePath.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        return;
    }

    const QJsonArray scrobbles = QJsonDocument::fromJson(file.readAll()).array();
    for (const QJsonValue &value : scrobbles) {
        if (!value.isObject()) {
            continue;
        }
        const LastFmApi::Scrobble scrobble = scrobbleFromJson(value.toObject());
        if (isValidCachedScrobble(scrobble)) {
            m_pendingScrobbles.push_back(scrobble);
        }
    }
}

void LastFmScrobbler::savePending() const
{
    if (m_cachePath.isEmpty()) {
        return;
    }

    QFileInfo info(m_cachePath);
    QDir().mkpath(info.absolutePath());
    QFile file(m_cachePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(lastFmLog) << "failed to write pending Last.fm cache" << m_cachePath;
        return;
    }

    QJsonArray scrobbles;
    for (const LastFmApi::Scrobble &scrobble : m_pendingScrobbles) {
        scrobbles.append(scrobbleToJson(scrobble));
    }
    file.write(QJsonDocument(scrobbles).toJson(QJsonDocument::Compact));
}

void LastFmScrobbler::cachePendingScrobble(const LastFmApi::Scrobble &scrobble)
{
    if (!isValidCachedScrobble(scrobble)) {
        return;
    }
    m_pendingScrobbles.push_back(scrobble);
    savePending();
}

void LastFmScrobbler::removeSubmittedPrefix(int count)
{
    const int removeCount = std::min(count, static_cast<int>(m_pendingScrobbles.size()));
    if (removeCount <= 0) {
        return;
    }
    m_pendingScrobbles.erase(m_pendingScrobbles.begin(), m_pendingScrobbles.begin() + removeCount);
    savePending();
}

void LastFmScrobbler::disableScrobbling(const QString &message)
{
    m_enabled = false;
    m_retryTimer->stop();
    emit disabledAfterFailures(message);
}

bool LastFmScrobbler::hasCredentials() const
{
    return !m_apiKey.isEmpty() && !m_sharedSecret.isEmpty() && !m_sessionKey.isEmpty();
}
