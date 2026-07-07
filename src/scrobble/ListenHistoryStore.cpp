#include "scrobble/ListenHistoryStore.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

namespace {

constexpr int kSchemaVersion = 7;

void insertIfPresent(QJsonObject &object, const QString &key, const QString &value)
{
    if (!value.isEmpty()) {
        object.insert(key, value);
    }
}

void insertIfPositive(QJsonObject &object, const QString &key, qint64 value)
{
    if (value > 0) {
        object.insert(key, value);
    }
}

// Full track snapshot, so the history stays meaningful even if the file later
// leaves the library (the row columns only carry the scrobble-relevant core).
QJsonObject trackToJson(const Track &track)
{
    QJsonObject json;
    insertIfPresent(json, QStringLiteral("path"), track.path);
    insertIfPresent(json, QStringLiteral("filename"), track.filename);
    insertIfPresent(json, QStringLiteral("title"), track.title);
    insertIfPresent(json, QStringLiteral("artist"), track.artistName);
    insertIfPresent(json, QStringLiteral("albumArtist"), track.albumArtistName);
    insertIfPresent(json, QStringLiteral("album"), track.albumTitle);
    insertIfPresent(json, QStringLiteral("date"), track.date);
    insertIfPresent(json, QStringLiteral("originalDate"), track.originalDate);
    insertIfPositive(json, QStringLiteral("trackNumber"), track.trackNumber);
    insertIfPositive(json, QStringLiteral("trackTotal"), track.trackTotal);
    insertIfPositive(json, QStringLiteral("discNumber"), track.discNumber);
    insertIfPositive(json, QStringLiteral("discTotal"), track.discTotal);
    insertIfPositive(json, QStringLiteral("durationMs"), track.durationMs);
    insertIfPresent(json, QStringLiteral("mbArtistId"), track.musicBrainz.artistId);
    insertIfPresent(json, QStringLiteral("mbAlbumArtistId"), track.musicBrainz.albumArtistId);
    insertIfPresent(json, QStringLiteral("mbReleaseId"), track.musicBrainz.releaseId);
    insertIfPresent(json, QStringLiteral("mbReleaseGroupId"), track.musicBrainz.releaseGroupId);
    insertIfPresent(json, QStringLiteral("mbRecordingId"), track.musicBrainz.recordingId);
    insertIfPresent(json, QStringLiteral("mbTrackId"), track.musicBrainz.trackId);
    insertIfPresent(json, QStringLiteral("mbWorkId"), track.musicBrainz.workId);
    insertIfPresent(json, QStringLiteral("codec"), track.codec);
    insertIfPositive(json, QStringLiteral("sampleRateHz"), track.sampleRateHz);
    insertIfPositive(json, QStringLiteral("bitrateKbps"), track.bitrateKbps);
    insertIfPositive(json, QStringLiteral("channels"), track.channels);
    insertIfPositive(json, QStringLiteral("bitDepth"), track.bitDepth);
    return json;
}

Track trackFromJson(const QJsonObject &json)
{
    Track track;
    track.path = json.value(QStringLiteral("path")).toString();
    track.filename = json.value(QStringLiteral("filename")).toString();
    track.title = json.value(QStringLiteral("title")).toString();
    track.artistName = json.value(QStringLiteral("artist")).toString();
    track.albumArtistName = json.value(QStringLiteral("albumArtist")).toString();
    track.albumTitle = json.value(QStringLiteral("album")).toString();
    track.date = json.value(QStringLiteral("date")).toString();
    track.originalDate = json.value(QStringLiteral("originalDate")).toString();
    track.trackNumber = json.value(QStringLiteral("trackNumber")).toInt();
    track.trackTotal = json.value(QStringLiteral("trackTotal")).toInt();
    track.discNumber = json.value(QStringLiteral("discNumber")).toInt();
    track.discTotal = json.value(QStringLiteral("discTotal")).toInt();
    track.durationMs = static_cast<qint64>(json.value(QStringLiteral("durationMs")).toDouble());
    track.musicBrainz.artistId = json.value(QStringLiteral("mbArtistId")).toString();
    track.musicBrainz.albumArtistId = json.value(QStringLiteral("mbAlbumArtistId")).toString();
    track.musicBrainz.releaseId = json.value(QStringLiteral("mbReleaseId")).toString();
    track.musicBrainz.releaseGroupId = json.value(QStringLiteral("mbReleaseGroupId")).toString();
    track.musicBrainz.recordingId = json.value(QStringLiteral("mbRecordingId")).toString();
    track.musicBrainz.trackId = json.value(QStringLiteral("mbTrackId")).toString();
    track.musicBrainz.workId = json.value(QStringLiteral("mbWorkId")).toString();
    track.codec = json.value(QStringLiteral("codec")).toString();
    track.sampleRateHz = json.value(QStringLiteral("sampleRateHz")).toInt();
    track.bitrateKbps = json.value(QStringLiteral("bitrateKbps")).toInt();
    track.channels = json.value(QStringLiteral("channels")).toInt();
    track.bitDepth = json.value(QStringLiteral("bitDepth")).toInt();
    return track;
}

QString sentColumn(const QString &service)
{
    if (service == ListenHistoryStore::LastFm) {
        return QStringLiteral("sent_lastfm");
    }
    if (service == ListenHistoryStore::ListenBrainz) {
        return QStringLiteral("sent_listenbrainz");
    }
    return {};
}

QString owedColumn(const QString &service)
{
    if (service == ListenHistoryStore::LastFm) {
        return QStringLiteral("owed_lastfm");
    }
    if (service == ListenHistoryStore::ListenBrainz) {
        return QStringLiteral("owed_listenbrainz");
    }
    return {};
}

QString placeholders(qsizetype count)
{
    QStringList marks;
    marks.reserve(count);
    for (qsizetype i = 0; i < count; ++i) {
        marks << QStringLiteral("?");
    }
    return marks.join(QStringLiteral(", "));
}

QString componentsToJson(const QVector<ListenHistoryStore::RadioPickComponent> &components)
{
    QJsonArray array;
    for (const ListenHistoryStore::RadioPickComponent &component : components) {
        QJsonObject object;
        object.insert(QStringLiteral("name"), component.name);
        object.insert(QStringLiteral("value"), component.value);
        array.push_back(object);
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QVector<ListenHistoryStore::RadioPickComponent> componentsFromJson(const QByteArray &json)
{
    QVector<ListenHistoryStore::RadioPickComponent> components;
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isArray()) {
        return components;
    }
    const QJsonArray array = document.array();
    components.reserve(array.size());
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        const QString name = object.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || !object.value(QStringLiteral("value")).isDouble()) {
            continue;
        }
        components.push_back({name, object.value(QStringLiteral("value")).toDouble()});
    }
    return components;
}

int deleteByPaths(QSqlDatabase database, const QString &table, const QString &column, const QStringList &paths)
{
    if (paths.isEmpty()) {
        return 0;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral("DELETE FROM %1 WHERE %2 IN (%3)")
                      .arg(table, column, placeholders(paths.size())));
    for (const QString &path : paths) {
        query.addBindValue(path);
    }
    if (!query.exec()) {
        return 0;
    }
    return query.numRowsAffected();
}

} // namespace

const QString ListenHistoryStore::LastFm = QStringLiteral("lastfm");
const QString ListenHistoryStore::ListenBrainz = QStringLiteral("listenbrainz");

ListenHistoryStore::ListenHistoryStore(const QString &path)
    : m_connectionName(QStringLiteral("muzaiten-history-%1").arg(reinterpret_cast<quintptr>(this)))
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        return;
    }

    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"));

    // The schema defined below is the only supported revision. No migration of
    // databases written by earlier revisions is performed or intended: a
    // pre-existing table is left untouched by CREATE TABLE IF NOT EXISTS, and the
    // schemaVersion row is overwritten unconditionally. Earlier schema revisions
    // are therefore silently skipped or overridden; backward compatibility is not
    // a goal of this store.
    QSqlQuery create(m_db);
    create.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS listens ("
        " id INTEGER PRIMARY KEY,"
        " listened_at INTEGER NOT NULL,"  // epoch secs, the second the track started playing
        " title TEXT NOT NULL,"
        " artist TEXT NOT NULL,"
        " album TEXT,"
        " path TEXT,"
        " duration_ms INTEGER,"
        " track_json TEXT NOT NULL,"
        " owed_lastfm INTEGER NOT NULL DEFAULT 0,"
        " sent_lastfm INTEGER NOT NULL DEFAULT 0,"
        " owed_listenbrainz INTEGER NOT NULL DEFAULT 0,"
        " sent_listenbrainz INTEGER NOT NULL DEFAULT 0,"
        " UNIQUE(listened_at, artist, title))"));
    create.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_listens_unsent_lastfm ON listens(listened_at) WHERE owed_lastfm = 1 AND sent_lastfm = 0"));
    create.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_listens_unsent_listenbrainz ON listens(listened_at) WHERE owed_listenbrainz = 1 AND sent_listenbrainz = 0"));
    create.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS play_events ("
        " id INTEGER PRIMARY KEY,"
        " started_at INTEGER NOT NULL,"        // epoch secs the track started
        " ended_at INTEGER,"                   // epoch secs the event finalized
        " played_ms INTEGER NOT NULL,"         // accumulated wall-clock playback time
        " duration_ms INTEGER,"
        " completion REAL,"                    // played_ms/duration_ms capped at 1.0; NULL when duration unknown
        " outcome TEXT NOT NULL,"              // finished | skipped | stopped | session_end
        " user_initiated INTEGER NOT NULL DEFAULT 0,"  // track start was an explicit user pick
        " source TEXT NOT NULL,"               // queue_manual | queue_auto | library_shuffle | resume
        " shuffle_mode TEXT,"
        " track_path TEXT NOT NULL,"
        " mb_recording_id TEXT,"
        " previous_track_path TEXT,"
        " session_id TEXT NOT NULL,"
        " track_json TEXT NOT NULL)"));
    create.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_play_events_track ON play_events(track_path, started_at)"));
    create.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_play_events_session ON play_events(session_id)"));
    create.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS rating_events ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " occurred_at INTEGER NOT NULL,"
        " track_path TEXT NOT NULL,"
        " mb_recording_id TEXT,"
        " old_user_rating INTEGER,"
        " old_effective_rating INTEGER,"
        " new_rating INTEGER,"
        " source_surface TEXT NOT NULL,"
        " playing_track_path TEXT,"
        " playing_source TEXT,"
        " radio_active INTEGER NOT NULL DEFAULT 0,"
        " track_json TEXT)"));
    create.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS queue_removals ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " occurred_at INTEGER NOT NULL,"
        " track_path TEXT NOT NULL,"
        " mb_recording_id TEXT,"
        " was_radio_pick INTEGER NOT NULL,"
        " was_unheard INTEGER NOT NULL,"
        " radio_active INTEGER NOT NULL)"));
    create.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS radio_picks ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " occurred_at INTEGER NOT NULL,"
        " track_path TEXT NOT NULL,"
        " mb_recording_id TEXT,"
        " session_kind TEXT NOT NULL,"
        " exploration INTEGER NOT NULL,"
        " weights_json TEXT NOT NULL,"
        " components_json TEXT NOT NULL,"
        " score REAL NOT NULL)"));
    create.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)"));

    // Scrobbler backfill (Stage 0b). Historical listens pulled from a service
    // (ListenBrainz full export, Last.fm import) land here, NOT in `listens`.
    // Being a separate table is load-bearing: these rows have no `owed_*`/
    // `sent_*` flags, so the scrobble-backlog drain can never pick them up and
    // re-scrobble history muzaiten only mirrored back from the service.
    create.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS imported_listens ("
        " id INTEGER PRIMARY KEY,"
        " source TEXT NOT NULL,"              // 'listenbrainz' | 'lastfm'
        " listened_at INTEGER NOT NULL,"      // epoch secs
        " title TEXT NOT NULL,"
        " artist TEXT NOT NULL,"
        " album TEXT,"
        " mb_recording_id TEXT,"
        " matched_track_path TEXT,"           // resolved library track; NULL when unmatched
        " UNIQUE(source, listened_at, artist, title))"));
    create.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_imported_listens_track ON imported_listens(matched_track_path, listened_at)"));
    // Per-service, per-track playcount snapshots (MusicBee-style count sync).
    // artist/title are the service-side identity, kept verbatim for re-matching.
    create.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS playcount_baselines ("
        " source TEXT NOT NULL,"
        " artist TEXT NOT NULL,"              // service-side identity, verbatim
        " title TEXT NOT NULL,"
        " mb_recording_id TEXT,"
        " matched_track_path TEXT,"
        " count INTEGER NOT NULL,"
        " synced_at INTEGER NOT NULL,"
        " PRIMARY KEY(source, artist, title))"));

    QSqlQuery version(m_db);
    version.prepare(QStringLiteral(
        "INSERT INTO meta(key, value) VALUES('schemaVersion', ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    version.addBindValue(QString::number(kSchemaVersion));
    version.exec();
}

ListenHistoryStore::~ListenHistoryStore()
{
    if (m_db.isValid()) {
        m_db.close();
    }
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool ListenHistoryStore::isOpen() const
{
    return m_db.isOpen();
}

void ListenHistoryStore::releaseCacheMemory()
{
    if (!m_db.isOpen()) {
        return;
    }

    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA shrink_memory"));
}

qint64 ListenHistoryStore::recordListen(const Track &track, qint64 listenedAtSecs,
                                        bool oweLastFm, bool oweListenBrainz)
{
    const QString title = track.title.trimmed().isEmpty() ? track.filename : track.title.trimmed();
    const QString artist = track.artistName.trimmed().isEmpty() ? track.albumArtistName.trimmed() : track.artistName.trimmed();
    if (!m_db.isOpen() || listenedAtSecs <= 0 || title.isEmpty()) {
        return -1;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO listens(listened_at, title, artist, album, path, duration_ms, track_json, "
        "owed_lastfm, sent_lastfm, owed_listenbrainz, sent_listenbrainz) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(listenedAtSecs);
    query.addBindValue(title);
    // A null QString binds as SQL NULL, which the NOT NULL constraint rejects
    // (and INSERT OR IGNORE then swallows); force a real empty string.
    query.addBindValue(artist.isNull() ? QStringLiteral("") : artist);
    query.addBindValue(track.albumTitle.trimmed());
    query.addBindValue(track.path);
    query.addBindValue(track.durationMs);
    query.addBindValue(QString::fromUtf8(QJsonDocument(trackToJson(track)).toJson(QJsonDocument::Compact)));
    query.addBindValue(oweLastFm ? 1 : 0);
    query.addBindValue(0);
    query.addBindValue(oweListenBrainz ? 1 : 0);
    query.addBindValue(0);
    if (!query.exec() || query.numRowsAffected() <= 0) {
        return -1;
    }
    return query.lastInsertId().toLongLong();
}

QList<ListenHistoryStore::Listen> ListenHistoryStore::unsent(const QString &service, int limit) const
{
    QList<Listen> listens;
    const QString column = sentColumn(service);
    const QString owed = owedColumn(service);
    if (!m_db.isOpen() || column.isEmpty() || owed.isEmpty() || limit <= 0) {
        return listens;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT id, listened_at, track_json FROM listens "
                                 "WHERE %1 = 1 AND %2 = 0 ORDER BY listened_at ASC LIMIT ?")
                      .arg(owed, column));
    query.addBindValue(limit);
    if (!query.exec()) {
        return listens;
    }
    while (query.next()) {
        Listen listen;
        listen.id = query.value(0).toLongLong();
        listen.listenedAtSecs = query.value(1).toLongLong();
        listen.track = trackFromJson(QJsonDocument::fromJson(query.value(2).toString().toUtf8()).object());
        listens.push_back(listen);
    }
    return listens;
}

int ListenHistoryStore::unsentCount(const QString &service) const
{
    return pendingCount(service);
}

int ListenHistoryStore::pendingCount(const QString &service) const
{
    const QString column = sentColumn(service);
    const QString owed = owedColumn(service);
    if (!m_db.isOpen() || column.isEmpty() || owed.isEmpty()) {
        return 0;
    }
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM listens WHERE %1 = 1 AND %2 = 0").arg(owed, column)) || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

int ListenHistoryStore::totalCount() const
{
    if (!m_db.isOpen()) {
        return 0;
    }
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM listens")) || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

void ListenHistoryStore::markSent(const QString &service, const QList<qint64> &ids)
{
    const QString column = sentColumn(service);
    if (!m_db.isOpen() || column.isEmpty() || ids.isEmpty()) {
        return;
    }
    QStringList idText;
    for (qint64 id : ids) {
        idText << QString::number(id);
    }
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("UPDATE listens SET %1 = 1 WHERE id IN (%2)").arg(column, idText.join(QLatin1Char(','))));
}

int ListenHistoryStore::clearPending(const QString &service)
{
    const QString column = sentColumn(service);
    const QString owed = owedColumn(service);
    if (!m_db.isOpen() || column.isEmpty() || owed.isEmpty()) {
        return 0;
    }
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("UPDATE listens SET %1 = 0 WHERE %1 = 1 AND %2 = 0").arg(owed, column))) {
        return 0;
    }
    return query.numRowsAffected();
}

int ListenHistoryStore::markOwed(const QString &service, const QList<qint64> &ids)
{
    const QString column = sentColumn(service);
    const QString owed = owedColumn(service);
    if (!m_db.isOpen() || column.isEmpty() || owed.isEmpty() || ids.isEmpty()) {
        return 0;
    }
    QStringList idText;
    for (qint64 id : ids) {
        idText << QString::number(id);
    }
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("UPDATE listens SET %1 = 1 WHERE %1 = 0 AND %2 = 0 AND id IN (%3)")
                        .arg(owed, column, idText.join(QLatin1Char(','))))) {
        return 0;
    }
    return query.numRowsAffected();
}

QList<ListenHistoryStore::HistoryRow> ListenHistoryStore::historyRows(int limit, int offset) const
{
    QList<HistoryRow> rows;
    if (!m_db.isOpen() || limit <= 0 || offset < 0) {
        return rows;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT id, listened_at, track_json, owed_lastfm, sent_lastfm, owed_listenbrainz, sent_listenbrainz "
        "FROM listens ORDER BY listened_at DESC, id DESC LIMIT ? OFFSET ?"));
    query.addBindValue(limit);
    query.addBindValue(offset);
    if (!query.exec()) {
        return rows;
    }
    while (query.next()) {
        HistoryRow row;
        row.id = query.value(0).toLongLong();
        row.listenedAtSecs = query.value(1).toLongLong();
        row.track = trackFromJson(QJsonDocument::fromJson(query.value(2).toString().toUtf8()).object());
        row.owedLastFm = query.value(3).toInt() != 0;
        row.sentLastFm = query.value(4).toInt() != 0;
        row.owedListenBrainz = query.value(5).toInt() != 0;
        row.sentListenBrainz = query.value(6).toInt() != 0;
        rows.push_back(row);
    }
    return rows;
}

qint64 ListenHistoryStore::recordPlayEvent(const PlayEvent &event)
{
    if (!m_db.isOpen() || event.outcome.isEmpty() || event.sessionId.isEmpty() || event.track.path.isEmpty()) {
        return -1;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO play_events(started_at, ended_at, played_ms, duration_ms, completion, outcome, "
        "user_initiated, source, shuffle_mode, track_path, mb_recording_id, previous_track_path, "
        "session_id, track_json) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(event.startedAtSecs);
    // ended_at is nullable; a non-positive value means the event was never
    // finalized against the clock, so leave it NULL rather than storing 0.
    query.addBindValue(event.endedAtSecs > 0 ? QVariant(event.endedAtSecs) : QVariant(QMetaType(QMetaType::LongLong)));
    query.addBindValue(event.playedMs);
    query.addBindValue(event.durationMs > 0 ? QVariant(event.durationMs) : QVariant(QMetaType(QMetaType::LongLong)));
    // completion < 0 encodes "duration unknown" and is stored as SQL NULL.
    query.addBindValue(event.completion >= 0.0 ? QVariant(event.completion) : QVariant(QMetaType(QMetaType::Double)));
    query.addBindValue(event.outcome);
    query.addBindValue(event.userInitiated ? 1 : 0);
    query.addBindValue(event.source);
    query.addBindValue(event.shuffleMode.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(event.shuffleMode));
    query.addBindValue(event.track.path);
    query.addBindValue(event.track.musicBrainz.recordingId);
    query.addBindValue(event.previousTrackPath);
    query.addBindValue(event.sessionId);
    query.addBindValue(QString::fromUtf8(QJsonDocument(trackToJson(event.track)).toJson(QJsonDocument::Compact)));
    if (!query.exec() || query.numRowsAffected() <= 0) {
        return -1;
    }
    return query.lastInsertId().toLongLong();
}

QList<ListenHistoryStore::PlayEvent> ListenHistoryStore::recentPlayEvents(int limit, int offset) const
{
    QList<PlayEvent> events;
    if (!m_db.isOpen() || limit <= 0 || offset < 0) {
        return events;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT id, started_at, ended_at, played_ms, duration_ms, completion, outcome, user_initiated, "
        "source, shuffle_mode, previous_track_path, session_id, track_json "
        "FROM play_events ORDER BY started_at DESC, id DESC LIMIT ? OFFSET ?"));
    query.addBindValue(limit);
    query.addBindValue(offset);
    if (!query.exec()) {
        return events;
    }
    while (query.next()) {
        PlayEvent event;
        event.id = query.value(0).toLongLong();
        event.startedAtSecs = query.value(1).toLongLong();
        event.endedAtSecs = query.value(2).toLongLong();
        event.playedMs = query.value(3).toLongLong();
        event.durationMs = query.value(4).toLongLong();
        // A NULL completion column round-trips back to the <0 "unknown" sentinel.
        event.completion = query.value(5).isNull() ? -1.0 : query.value(5).toDouble();
        event.outcome = query.value(6).toString();
        event.userInitiated = query.value(7).toInt() != 0;
        event.source = query.value(8).toString();
        event.shuffleMode = query.value(9).toString();
        event.previousTrackPath = query.value(10).toString();
        event.sessionId = query.value(11).toString();
        event.track = trackFromJson(QJsonDocument::fromJson(query.value(12).toString().toUtf8()).object());
        events.push_back(event);
    }
    return events;
}

int ListenHistoryStore::playEventCount() const
{
    if (!m_db.isOpen()) {
        return 0;
    }
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM play_events")) || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

bool ListenHistoryStore::recordRatingEvent(const RatingEvent &event)
{
    if (!m_db.isOpen() || event.occurredAtSecs <= 0 || event.track.path.isEmpty() || event.sourceSurface.isEmpty()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO rating_events(occurred_at, track_path, mb_recording_id, old_user_rating, "
        "old_effective_rating, new_rating, source_surface, playing_track_path, playing_source, "
        "radio_active, track_json) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(event.occurredAtSecs);
    query.addBindValue(event.track.path);
    query.addBindValue(event.track.musicBrainz.recordingId);
    query.addBindValue(event.hasOldUserRating ? QVariant(event.oldUserRating0To100) : QVariant(QMetaType(QMetaType::Int)));
    query.addBindValue(event.oldEffectiveRating0To100 >= 0
                           ? QVariant(event.oldEffectiveRating0To100)
                           : QVariant(QMetaType(QMetaType::Int)));
    query.addBindValue(event.newRating0To100 >= 0
                           ? QVariant(event.newRating0To100)
                           : QVariant(QMetaType(QMetaType::Int)));
    query.addBindValue(event.sourceSurface);
    query.addBindValue(event.playingTrackPath.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.playingTrackPath));
    query.addBindValue(event.playingSource.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.playingSource));
    query.addBindValue(event.radioActive ? 1 : 0);
    query.addBindValue(QString::fromUtf8(QJsonDocument(trackToJson(event.track)).toJson(QJsonDocument::Compact)));
    return query.exec() && query.numRowsAffected() > 0;
}

QVector<ListenHistoryStore::RatingEvent> ListenHistoryStore::ratingEvents(int limit) const
{
    QVector<RatingEvent> events;
    if (!m_db.isOpen() || limit == 0) {
        return events;
    }

    QString sql = QStringLiteral(
        "SELECT id, occurred_at, track_path, mb_recording_id, old_user_rating, old_effective_rating, new_rating, "
        "source_surface, playing_track_path, playing_source, radio_active, track_json "
        "FROM rating_events ORDER BY occurred_at ASC, id ASC");
    if (limit > 0) {
        sql += QStringLiteral(" LIMIT ?");
    }
    QSqlQuery query(m_db);
    query.prepare(sql);
    if (limit > 0) {
        query.addBindValue(limit);
    }
    if (!query.exec()) {
        return events;
    }
    while (query.next()) {
        RatingEvent event;
        event.id = query.value(0).toLongLong();
        event.occurredAtSecs = query.value(1).toLongLong();
        event.hasOldUserRating = !query.value(4).isNull();
        event.oldUserRating0To100 = event.hasOldUserRating ? query.value(4).toInt() : -1;
        event.oldEffectiveRating0To100 = query.value(5).isNull() ? -1 : query.value(5).toInt();
        event.newRating0To100 = query.value(6).isNull() ? -1 : query.value(6).toInt();
        event.sourceSurface = query.value(7).toString();
        event.playingTrackPath = query.value(8).toString();
        event.playingSource = query.value(9).toString();
        event.radioActive = query.value(10).toInt() != 0;
        event.track = trackFromJson(QJsonDocument::fromJson(query.value(11).toString().toUtf8()).object());
        if (event.track.path.isEmpty()) {
            event.track.path = query.value(2).toString();
        }
        if (event.track.musicBrainz.recordingId.isEmpty()) {
            event.track.musicBrainz.recordingId = query.value(3).toString();
        }
        events.push_back(event);
    }
    return events;
}

bool ListenHistoryStore::recordQueueRemoval(const QueueRemovalEvent &event)
{
    if (!m_db.isOpen() || event.occurredAtSecs <= 0 || event.track.path.isEmpty()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO queue_removals(occurred_at, track_path, mb_recording_id, "
        "was_radio_pick, was_unheard, radio_active) "
        "VALUES(?, ?, ?, ?, ?, ?)"));
    query.addBindValue(event.occurredAtSecs);
    query.addBindValue(event.track.path);
    query.addBindValue(event.track.musicBrainz.recordingId.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.track.musicBrainz.recordingId));
    query.addBindValue(event.wasRadioPick ? 1 : 0);
    query.addBindValue(event.wasUnheard ? 1 : 0);
    query.addBindValue(event.radioActive ? 1 : 0);
    return query.exec() && query.numRowsAffected() > 0;
}

QVector<ListenHistoryStore::QueueRemovalEvent> ListenHistoryStore::queueRemovalEvents(int limit) const
{
    QVector<QueueRemovalEvent> events;
    if (!m_db.isOpen() || limit == 0) {
        return events;
    }

    QString sql = QStringLiteral(
        "SELECT id, occurred_at, track_path, mb_recording_id, was_radio_pick, was_unheard, radio_active "
        "FROM queue_removals ORDER BY occurred_at ASC, id ASC");
    if (limit > 0) {
        sql += QStringLiteral(" LIMIT ?");
    }
    QSqlQuery query(m_db);
    query.prepare(sql);
    if (limit > 0) {
        query.addBindValue(limit);
    }
    if (!query.exec()) {
        return events;
    }
    while (query.next()) {
        QueueRemovalEvent event;
        event.id = query.value(0).toLongLong();
        event.occurredAtSecs = query.value(1).toLongLong();
        event.track.path = query.value(2).toString();
        event.track.musicBrainz.recordingId = query.value(3).toString();
        event.wasRadioPick = query.value(4).toInt() != 0;
        event.wasUnheard = query.value(5).toInt() != 0;
        event.radioActive = query.value(6).toInt() != 0;
        events.push_back(event);
    }
    return events;
}

bool ListenHistoryStore::recordRadioPick(const RadioPickEvent &event)
{
    if (!m_db.isOpen() || event.occurredAtSecs <= 0 || event.track.path.isEmpty()
        || event.sessionKind.isEmpty() || event.weightsJson.trimmed().isEmpty()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO radio_picks(occurred_at, track_path, mb_recording_id, session_kind, "
        "exploration, weights_json, components_json, score) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(event.occurredAtSecs);
    query.addBindValue(event.track.path);
    query.addBindValue(event.track.musicBrainz.recordingId.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(event.track.musicBrainz.recordingId));
    query.addBindValue(event.sessionKind);
    query.addBindValue(event.exploration0To100);
    query.addBindValue(QString::fromUtf8(event.weightsJson));
    query.addBindValue(componentsToJson(event.components));
    query.addBindValue(event.score);
    return query.exec() && query.numRowsAffected() > 0;
}

QVector<ListenHistoryStore::RadioPickEvent> ListenHistoryStore::radioPickEvents(int limit) const
{
    QVector<RadioPickEvent> events;
    if (!m_db.isOpen() || limit == 0) {
        return events;
    }

    QString sql = QStringLiteral(
        "SELECT id, occurred_at, track_path, mb_recording_id, session_kind, exploration, "
        "weights_json, components_json, score "
        "FROM radio_picks ORDER BY occurred_at ASC, id ASC");
    if (limit > 0) {
        sql += QStringLiteral(" LIMIT ?");
    }
    QSqlQuery query(m_db);
    query.prepare(sql);
    if (limit > 0) {
        query.addBindValue(limit);
    }
    if (!query.exec()) {
        return events;
    }
    while (query.next()) {
        RadioPickEvent event;
        event.id = query.value(0).toLongLong();
        event.occurredAtSecs = query.value(1).toLongLong();
        event.track.path = query.value(2).toString();
        event.track.musicBrainz.recordingId = query.value(3).toString();
        event.sessionKind = query.value(4).toString();
        event.exploration0To100 = query.value(5).toInt();
        event.weightsJson = query.value(6).toString().toUtf8();
        event.components = componentsFromJson(query.value(7).toString().toUtf8());
        event.score = query.value(8).toDouble();
        events.push_back(event);
    }
    return events;
}

int ListenHistoryStore::forgetTrackBehavior(const QStringList &paths, bool includeImportedListens)
{
    if (!m_db.isOpen()) {
        return 0;
    }

    QStringList uniquePaths;
    for (const QString &path : paths) {
        if (!path.isEmpty() && !uniquePaths.contains(path)) {
            uniquePaths.push_back(path);
        }
    }
    if (uniquePaths.isEmpty()) {
        return 0;
    }

    if (!m_db.transaction()) {
        return 0;
    }
    int removed = deleteByPaths(m_db, QStringLiteral("play_events"), QStringLiteral("track_path"), uniquePaths);
    if (includeImportedListens) {
        removed += deleteByPaths(m_db, QStringLiteral("imported_listens"), QStringLiteral("matched_track_path"), uniquePaths);
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return 0;
    }
    return removed;
}

int ListenHistoryStore::recordImportedListens(const QList<ImportedListen> &rows)
{
    if (!m_db.isOpen() || rows.isEmpty()) {
        return 0;
    }

    // Cross-dedup lookup: does an identical (listened_at, artist, title) row
    // already exist in `listens`? Those are the user's own scrobbles muzaiten
    // submitted, so importing the service's echo of them would double-count.
    QSqlQuery existing(m_db);
    existing.prepare(QStringLiteral(
        "SELECT 1 FROM listens WHERE listened_at = ? AND artist = ? AND title = ? LIMIT 1"));

    QSqlQuery insert(m_db);
    insert.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO imported_listens"
        "(source, listened_at, title, artist, album, mb_recording_id, matched_track_path) "
        "VALUES(?, ?, ?, ?, ?, ?, ?)"));

    int inserted = 0;
    m_db.transaction();
    for (const ImportedListen &row : rows) {
        const QString title = row.title.trimmed();
        const QString artist = row.artist.trimmed();
        if (row.source.isEmpty() || row.listenedAtSecs <= 0 || title.isEmpty() || artist.isEmpty()) {
            continue;
        }

        existing.addBindValue(row.listenedAtSecs);
        existing.addBindValue(artist);
        existing.addBindValue(title);
        const bool ownScrobble = existing.exec() && existing.next();
        existing.finish();
        if (ownScrobble) {
            continue;
        }

        insert.addBindValue(row.source);
        insert.addBindValue(row.listenedAtSecs);
        insert.addBindValue(title);
        insert.addBindValue(artist);
        insert.addBindValue(row.album.trimmed().isEmpty() ? QVariant(QMetaType(QMetaType::QString))
                                                          : QVariant(row.album.trimmed()));
        insert.addBindValue(row.mbRecordingId.trimmed().isEmpty()
                                ? QVariant(QMetaType(QMetaType::QString))
                                : QVariant(row.mbRecordingId.trimmed()));
        insert.addBindValue(row.matchedTrackPath.isEmpty()
                                ? QVariant(QMetaType(QMetaType::QString))
                                : QVariant(row.matchedTrackPath));
        if (insert.exec() && insert.numRowsAffected() > 0) {
            ++inserted;
        }
    }
    m_db.commit();
    return inserted;
}

bool ListenHistoryStore::upsertPlaycountBaseline(const PlaycountBaseline &row)
{
    const QString artist = row.artist.trimmed();
    const QString title = row.title.trimmed();
    if (!m_db.isOpen() || row.source.isEmpty() || artist.isEmpty() || title.isEmpty()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO playcount_baselines"
        "(source, artist, title, mb_recording_id, matched_track_path, count, synced_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(source, artist, title) DO UPDATE SET "
        "count = excluded.count, synced_at = excluded.synced_at, "
        "mb_recording_id = excluded.mb_recording_id, matched_track_path = excluded.matched_track_path"));
    query.addBindValue(row.source);
    query.addBindValue(artist);
    query.addBindValue(title);
    query.addBindValue(row.mbRecordingId.trimmed().isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(row.mbRecordingId.trimmed()));
    query.addBindValue(row.matchedTrackPath.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(row.matchedTrackPath));
    query.addBindValue(row.count);
    query.addBindValue(row.syncedAtSecs);
    return query.exec();
}

QList<ListenHistoryStore::PlaycountBaseline> ListenHistoryStore::playcountBaselines(const QString &source) const
{
    QList<PlaycountBaseline> rows;
    if (!m_db.isOpen()) {
        return rows;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT source, artist, title, mb_recording_id, matched_track_path, count, synced_at "
        "FROM playcount_baselines WHERE source = ? ORDER BY count DESC"));
    query.addBindValue(source);
    if (!query.exec()) {
        return rows;
    }
    while (query.next()) {
        PlaycountBaseline row;
        row.source = query.value(0).toString();
        row.artist = query.value(1).toString();
        row.title = query.value(2).toString();
        row.mbRecordingId = query.value(3).toString();
        row.matchedTrackPath = query.value(4).toString();
        row.count = query.value(5).toLongLong();
        row.syncedAtSecs = query.value(6).toLongLong();
        rows.push_back(row);
    }
    return rows;
}

int ListenHistoryStore::importedListenCount(const QString &source) const
{
    if (!m_db.isOpen()) {
        return 0;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM imported_listens WHERE source = ?"));
    query.addBindValue(source);
    if (!query.exec() || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

QHash<QString, ListenHistoryStore::TrackAffinityRow> ListenHistoryStore::trackAffinities() const
{
    QHash<QString, TrackAffinityRow> affinities;
    if (!m_db.isOpen()) {
        return affinities;
    }

    // play_events: spins, split by outcome, plus the most recent start.
    // A "skip" only counts as a dislike signal when it happened before the
    // scrobble threshold (half the duration, capped at 4 minutes — the same
    // rule ListenTracker uses). A skip past that point already produced a
    // listens row: the user heard the song and moved on, which is a listen,
    // not a rejection ("skipped near the end" must not read as dislike).
    QSqlQuery events(m_db);
    if (events.exec(QStringLiteral(
            "SELECT track_path, COUNT(*), "
            "SUM(outcome = 'finished'), "
            "SUM(outcome = 'skipped' AND played_ms < "
            " CASE WHEN duration_ms > 0 THEN MIN(duration_ms / 2, 240000) ELSE 240000 END), "
            "MAX(started_at) "
            "FROM play_events WHERE track_path <> '' GROUP BY track_path"))) {
        while (events.next()) {
            TrackAffinityRow &row = affinities[events.value(0).toString()];
            row.playEvents = events.value(1).toInt();
            row.finished = events.value(2).toInt();
            row.skipped = events.value(3).toInt();
            row.lastPlayedAtSecs = events.value(4).toLongLong();
        }
    }

    // Local listens by resolved path.
    QSqlQuery listens(m_db);
    if (listens.exec(QStringLiteral(
            "SELECT path, COUNT(*) FROM listens WHERE path IS NOT NULL AND path <> '' GROUP BY path"))) {
        while (listens.next()) {
            affinities[listens.value(0).toString()].listenCount += listens.value(1).toInt();
        }
    }

    // Imported listens by matched library path (unmatched rows carry NULL).
    QSqlQuery imported(m_db);
    if (imported.exec(QStringLiteral(
            "SELECT matched_track_path, COUNT(*) FROM imported_listens "
            "WHERE matched_track_path IS NOT NULL AND matched_track_path <> '' GROUP BY matched_track_path"))) {
        while (imported.next()) {
            affinities[imported.value(0).toString()].listenCount += imported.value(1).toInt();
        }
    }

    // Playcount baselines: max across services (never summed — sources overlap).
    QSqlQuery baselines(m_db);
    if (baselines.exec(QStringLiteral(
            "SELECT matched_track_path, MAX(count) FROM playcount_baselines "
            "WHERE matched_track_path IS NOT NULL AND matched_track_path <> '' GROUP BY matched_track_path"))) {
        while (baselines.next()) {
            affinities[baselines.value(0).toString()].baselineMax = baselines.value(1).toInt();
        }
    }

    return affinities;
}

QString ListenHistoryStore::metaValue(const QString &key) const
{
    if (!m_db.isOpen()) {
        return {};
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    query.addBindValue(key);
    if (!query.exec() || !query.next()) {
        return {};
    }
    return query.value(0).toString();
}

void ListenHistoryStore::setMetaValue(const QString &key, const QString &value)
{
    if (!m_db.isOpen()) {
        return;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO meta(key, value) VALUES(?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.addBindValue(key);
    // A null QString() (used to clear a key, e.g. the backfill cursor/canceled
    // flag) binds as SQL NULL, which the column's NOT NULL constraint would
    // silently reject (exec() failing without anyone checking it here).
    // Normalize to a non-null empty string so "clear" round-trips through
    // metaValue() as "" like the rest of this API documents.
    query.addBindValue(value.isNull() ? QString(QLatin1String("")) : value);
    query.exec();
}
