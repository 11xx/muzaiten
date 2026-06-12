#include "scrobble/ListenHistoryStore.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QVariant>

namespace {

constexpr int kSchemaVersion = 2;

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
    create.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)"));

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
