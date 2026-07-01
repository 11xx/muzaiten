#include "scrobble/ScrobbleBackfill.h"

#include "scrobble/BackfillParse.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

Q_LOGGING_CATEGORY(scrobbleBackfillLog, "muzaiten.backfill")

namespace {

constexpr int kListensPerPage = 100;
constexpr int kTopTracksPerPage = 1000;
// Floor between any two requests, regardless of what the service reports.
constexpr int kMinRequestGapMs = 1100;
constexpr int kMaxPageRetries = 3;

const QString kLbOldestTsKey = QStringLiteral("backfill.listenbrainz.oldest_ts");
const QString kLbCompletedKey = QStringLiteral("backfill.listenbrainz.completed_at");
const QString kLfSyncedKey = QStringLiteral("backfill.lastfm.synced_at");

} // namespace

ScrobbleBackfill::ScrobbleBackfill(QObject *parent)
    : QObject(parent)
{
    m_network = new QNetworkAccessManager(this);
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        if (m_aborting) {
            return;
        }
        if (m_job == Job::ListenBrainz) {
            requestListenBrainzPage();
        } else if (m_job == Job::LastFm) {
            requestLastFmPage();
        }
    });
}

ScrobbleBackfill::~ScrobbleBackfill() = default;

QString ScrobbleBackfill::foldedArtistTitleKey(const QString &artist, const QString &title)
{
    return artist.simplified().toCaseFolded() + QLatin1Char('\n') + title.simplified().toCaseFolded();
}

QString ScrobbleBackfill::matchTrackPath(const LibraryIndex &index, const QString &mbRecordingId,
                                         const QString &artist, const QString &title)
{
    if (!mbRecordingId.isEmpty()) {
        const auto it = index.byRecordingMbid.constFind(mbRecordingId);
        if (it != index.byRecordingMbid.constEnd()) {
            return it.value();
        }
    }
    const auto it = index.byArtistTitle.constFind(foldedArtistTitleKey(artist, title));
    return it != index.byArtistTitle.constEnd() ? it.value() : QString();
}

bool ScrobbleBackfill::beginJob(Job job, const QString &historyPath)
{
    if (m_job != Job::None) {
        return false;
    }
    m_job = job;
    m_aborting = false;
    m_processed = 0;
    m_inserted = 0;
    m_pageRetries = 0;
    m_history = historyPath.isEmpty() ? nullptr : std::make_unique<ListenHistoryStore>(historyPath);
    return true;
}

void ScrobbleBackfill::endJob()
{
    m_timer->stop();
    m_job = Job::None;
    m_aborting = false;
    m_reply = nullptr;
    m_history.reset();
    m_index = LibraryIndex();
    m_token.clear();
    m_apiKey.clear();
    m_username.clear();
}

QString ScrobbleBackfill::sourceName() const
{
    switch (m_job) {
    case Job::ListenBrainz:
        return ListenHistoryStore::ListenBrainz;
    case Job::LastFm:
        return ListenHistoryStore::LastFm;
    case Job::None:
        break;
    }
    return {};
}

void ScrobbleBackfill::finishFailed(const QString &message)
{
    const QString source = sourceName();
    endJob();
    emit failed(source, message);
}

int ScrobbleBackfill::retryDelayMs(int attempt)
{
    switch (attempt) {
    case 0:
        return 10 * 1000;
    case 1:
        return 60 * 1000;
    default:
        break;
    }
    return 5 * 60 * 1000;
}

int ScrobbleBackfill::rateLimitDelayMs(QNetworkReply *reply) const
{
    int delay = kMinRequestGapMs;
    if (reply != nullptr) {
        bool haveRemaining = false;
        const int remaining = reply->rawHeader("X-RateLimit-Remaining").toInt(&haveRemaining);
        bool haveReset = false;
        const int resetIn = reply->rawHeader("X-RateLimit-Reset-In").toInt(&haveReset);
        // Out of budget for this window: wait the full reset interval.
        if (haveRemaining && remaining <= 0 && haveReset && resetIn > 0) {
            delay = std::max(delay, (resetIn + 1) * 1000);
        }
    }
    return delay;
}

void ScrobbleBackfill::abort()
{
    if (m_job == Job::None) {
        return;
    }
    m_aborting = true;
    m_timer->stop();
    if (m_reply != nullptr) {
        m_reply->abort();   // triggers the finished handler, which sees m_aborting
        return;
    }
    finishFailed(QStringLiteral("aborted"));
}

// --- ListenBrainz --------------------------------------------------------

void ScrobbleBackfill::startListenBrainzImport(const QString &token, const QString &historyPath,
                                               const LibraryIndex &index)
{
    if (m_job != Job::None) {
        emit failed(ListenHistoryStore::ListenBrainz, QStringLiteral("backfill already running"));
        return;
    }
    if (token.trimmed().isEmpty()) {
        emit failed(ListenHistoryStore::ListenBrainz, QStringLiteral("missing ListenBrainz token"));
        return;
    }
    beginJob(Job::ListenBrainz, historyPath);
    if (m_history == nullptr || !m_history->isOpen()) {
        finishFailed(QStringLiteral("history store unavailable"));
        return;
    }
    m_token = token.trimmed();
    m_index = index;
    // Resume from a prior interrupted run if a cursor persists; otherwise start
    // from now. A previously completed import may stop early once it pages into
    // already-imported history.
    m_lbCursor = m_history->metaValue(kLbOldestTsKey).toLongLong();
    m_lbEarlyStopAllowed = !m_history->metaValue(kLbCompletedKey).isEmpty();
    validateListenBrainzToken();
}

void ScrobbleBackfill::validateListenBrainzToken()
{
    QNetworkRequest request(QUrl(QStringLiteral("https://api.listenbrainz.org/1/validate-token")));
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization", QStringLiteral("Token %1").arg(m_token).toUtf8());

    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        const QByteArray body = reply->isOpen() ? reply->readAll() : QByteArray();
        const bool networkOk = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (m_aborting) {
            finishFailed(QStringLiteral("aborted"));
            return;
        }
        if (!networkOk) {
            finishFailed(QStringLiteral("token validation failed: network error"));
            return;
        }
        const BackfillParse::TokenValidation validation = BackfillParse::parseTokenValidation(body);
        if (!validation.valid || validation.username.isEmpty()) {
            finishFailed(QStringLiteral("ListenBrainz token was rejected"));
            return;
        }
        m_username = validation.username;
        requestListenBrainzPage();
    });
}

void ScrobbleBackfill::requestListenBrainzPage()
{
    QUrl url(QStringLiteral("https://api.listenbrainz.org/1/user/%1/listens").arg(m_username));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("count"), QString::number(kListensPerPage));
    if (m_lbCursor > 0) {
        query.addQueryItem(QStringLiteral("max_ts"), QString::number(m_lbCursor));
    }
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization", QStringLiteral("Token %1").arg(m_token).toUtf8());

    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::finished, this, &ScrobbleBackfill::handleListenBrainzPage);
}

void ScrobbleBackfill::handleListenBrainzPage()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    const QByteArray body = reply->isOpen() ? reply->readAll() : QByteArray();
    const bool networkOk = reply->error() == QNetworkReply::NoError;
    const int nextDelay = rateLimitDelayMs(reply);
    reply->deleteLater();

    if (m_aborting) {
        finishFailed(QStringLiteral("aborted"));
        return;
    }

    const BackfillParse::ListenBrainzPage page = networkOk ? BackfillParse::parseListenBrainzPage(body)
                                                           : BackfillParse::ListenBrainzPage();
    if (!networkOk || !page.ok) {
        // Persist the cursor so the next run resumes; retry with backoff first.
        if (m_pageRetries < kMaxPageRetries) {
            const int delay = retryDelayMs(m_pageRetries++);
            qCWarning(scrobbleBackfillLog) << "ListenBrainz page failed; retry in" << delay << "ms";
            m_timer->start(delay);
            return;
        }
        m_history->setMetaValue(kLbOldestTsKey, QString::number(m_lbCursor));
        finishFailed(QStringLiteral("ListenBrainz import failed after %1 retries").arg(kMaxPageRetries));
        return;
    }
    m_pageRetries = 0;

    // Empty page: reached the end of history. Mark complete and clear the cursor.
    if (page.listens.isEmpty()) {
        m_history->setMetaValue(kLbCompletedKey, QString::number(QDateTime::currentSecsSinceEpoch()));
        m_history->setMetaValue(kLbOldestTsKey, QString());
        const QString source = sourceName();
        const int processed = m_processed;
        const int inserted = m_inserted;
        endJob();
        emit finished(source, processed, inserted,
                      QStringLiteral("imported %1 new listens").arg(inserted));
        return;
    }

    QList<ListenHistoryStore::ImportedListen> rows;
    rows.reserve(page.listens.size());
    for (const BackfillParse::ListenBrainzListen &listen : page.listens) {
        ListenHistoryStore::ImportedListen row;
        row.source = ListenHistoryStore::ListenBrainz;
        row.listenedAtSecs = listen.listenedAtSecs;
        row.title = listen.title;
        row.artist = listen.artist;
        row.album = listen.album;
        row.mbRecordingId = listen.mbRecordingId;
        row.matchedTrackPath = matchTrackPath(m_index, listen.mbRecordingId, listen.artist, listen.title);
        rows.push_back(row);
    }
    const int insertedThisPage = m_history->recordImportedListens(rows);
    m_processed += static_cast<int>(rows.size());
    m_inserted += insertedThisPage;

    // Page further back: max_ts is exclusive, so the oldest listened_at becomes
    // the next cursor (the row we already stored is excluded next time).
    m_lbCursor = page.oldestListenedAt;
    m_history->setMetaValue(kLbOldestTsKey, QString::number(m_lbCursor));
    emit progress(ListenHistoryStore::ListenBrainz, m_processed, m_inserted);

    // Re-run optimization: once a completed import pages into a full page it has
    // already seen (nothing new inserted), everything older is known too — stop.
    if (m_lbEarlyStopAllowed && insertedThisPage == 0
        && static_cast<int>(rows.size()) >= kListensPerPage) {
        m_history->setMetaValue(kLbCompletedKey, QString::number(QDateTime::currentSecsSinceEpoch()));
        m_history->setMetaValue(kLbOldestTsKey, QString());
        const QString source = sourceName();
        const int processed = m_processed;
        const int inserted = m_inserted;
        endJob();
        emit finished(source, processed, inserted,
                      QStringLiteral("imported %1 new listens (reached known history)").arg(inserted));
        return;
    }

    m_timer->start(nextDelay);
}

// --- Last.fm -------------------------------------------------------------

void ScrobbleBackfill::startLastFmCountSync(const QString &apiKey, const QString &username,
                                            const QString &historyPath, const LibraryIndex &index)
{
    if (m_job != Job::None) {
        emit failed(ListenHistoryStore::LastFm, QStringLiteral("backfill already running"));
        return;
    }
    if (apiKey.trimmed().isEmpty() || username.trimmed().isEmpty()) {
        emit failed(ListenHistoryStore::LastFm, QStringLiteral("missing Last.fm credentials"));
        return;
    }
    beginJob(Job::LastFm, historyPath);
    if (m_history == nullptr || !m_history->isOpen()) {
        finishFailed(QStringLiteral("history store unavailable"));
        return;
    }
    m_apiKey = apiKey.trimmed();
    m_username = username.trimmed();
    m_index = index;
    m_lfPage = 1;
    m_lfTotalPages = 0;
    requestLastFmPage();
}

void ScrobbleBackfill::requestLastFmPage()
{
    QUrl url(QStringLiteral("https://ws.audioscrobbler.com/2.0/"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("method"), QStringLiteral("user.gettoptracks"));
    query.addQueryItem(QStringLiteral("user"), m_username);
    query.addQueryItem(QStringLiteral("api_key"), m_apiKey);
    query.addQueryItem(QStringLiteral("period"), QStringLiteral("overall"));
    query.addQueryItem(QStringLiteral("limit"), QString::number(kTopTracksPerPage));
    query.addQueryItem(QStringLiteral("page"), QString::number(m_lfPage));
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept", "application/json");

    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::finished, this, &ScrobbleBackfill::handleLastFmPage);
}

void ScrobbleBackfill::handleLastFmPage()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    const QByteArray body = reply->isOpen() ? reply->readAll() : QByteArray();
    const bool networkOk = reply->error() == QNetworkReply::NoError;
    reply->deleteLater();

    if (m_aborting) {
        finishFailed(QStringLiteral("aborted"));
        return;
    }

    const BackfillParse::LastFmTopTracksPage page = networkOk
        ? BackfillParse::parseLastFmTopTracks(body)
        : BackfillParse::LastFmTopTracksPage();
    if (!networkOk || !page.ok) {
        if (page.errorCode != 0) {
            // A service-level error (bad key, no such user) is terminal.
            finishFailed(QStringLiteral("Last.fm error %1: %2").arg(page.errorCode).arg(page.errorMessage));
            return;
        }
        if (m_pageRetries < kMaxPageRetries) {
            const int delay = retryDelayMs(m_pageRetries++);
            qCWarning(scrobbleBackfillLog) << "Last.fm page failed; retry in" << delay << "ms";
            m_timer->start(delay);
            return;
        }
        finishFailed(QStringLiteral("Last.fm sync failed after %1 retries").arg(kMaxPageRetries));
        return;
    }
    m_pageRetries = 0;
    m_lfTotalPages = std::max(m_lfTotalPages, page.totalPages);

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const BackfillParse::LastFmTrack &track : page.tracks) {
        ListenHistoryStore::PlaycountBaseline baseline;
        baseline.source = ListenHistoryStore::LastFm;
        baseline.artist = track.artist;
        baseline.title = track.title;
        baseline.mbRecordingId = track.mbRecordingId;
        baseline.matchedTrackPath = matchTrackPath(m_index, track.mbRecordingId, track.artist, track.title);
        baseline.count = track.count;
        baseline.syncedAtSecs = now;
        if (m_history->upsertPlaycountBaseline(baseline)) {
            ++m_inserted;
        }
        ++m_processed;
    }
    emit progress(ListenHistoryStore::LastFm, m_processed, m_inserted);

    // Done when the last page was reached (or the page carried no tracks).
    if (page.tracks.isEmpty() || (m_lfTotalPages > 0 && m_lfPage >= m_lfTotalPages)) {
        m_history->setMetaValue(kLfSyncedKey, QString::number(now));
        const QString source = sourceName();
        const int processed = m_processed;
        const int inserted = m_inserted;
        endJob();
        emit finished(source, processed, inserted,
                      QStringLiteral("synced %1 playcount baselines").arg(inserted));
        return;
    }

    ++m_lfPage;
    m_timer->start(kMinRequestGapMs);
}
