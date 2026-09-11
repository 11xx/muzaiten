#include "db/Database.h"
#include "db/SqlUtil.h"

#include <QFileInfo>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace {

struct TrackedTable {
    QString name;
    QStringList columns;
};

const QVector<TrackedTable> &trackedTables()
{
    // Keep these inputs aligned with the local/MPD search projections and visibility predicates.
    static const QVector<TrackedTable> tables{
        {QStringLiteral("tracks"), QStringLiteral("path filename title artist_name album_artist_name album_title date duration_ms sample_rate_hz bitrate_kbps channels codec rating_0_100 track_number disc_number file_mtime file_size bit_depth title_sort artist_sort album_artist_sort album_sort missing metadata_scanned").split(QLatin1Char(' '))},
        {QStringLiteral("user_track_ratings"), {QStringLiteral("track_path"), QStringLiteral("rating_0_100")}},
        {QStringLiteral("pending_track_rating_writes"), {QStringLiteral("track_path"), QStringLiteral("rating_0_100"), QStringLiteral("status")}},
        {QStringLiteral("scan_roots"), {QStringLiteral("path"), QStringLiteral("library_enabled")}},
        {QStringLiteral("mpd_tracks"), QStringLiteral("uri title artist_name album_artist_name album_title date duration_ms track_number disc_number").split(QLatin1Char(' '))},
    };
    return tables;
}

QString triggerName(const QString &table, const QString &operation)
{
    return QStringLiteral("search_revision_%1_%2").arg(table, operation.toLower());
}

} // namespace

bool Database::ensureSearchRevision()
{
    QSqlQuery query(m_db);
    bool identityValid = false;
    if (query.exec(QStringLiteral("SELECT database_id, revision FROM search_content_state WHERE singleton=1")) && query.next()) {
        bool ok = false;
        const qint64 revision = query.value(1).toLongLong(&ok);
        identityValid = !QUuid(query.value(0).toString()).isNull() && ok && revision >= 0;
    }
    query.finish();
    QSet<QString> installed;
    if (!query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='trigger' AND name LIKE 'search_revision_%'"))) {
        m_lastError = query.lastError().text();
        return false;
    }
    while (query.next()) installed.insert(query.value(0).toString());
    query.finish();
    const QStringList operations{QStringLiteral("INSERT"), QStringLiteral("UPDATE"), QStringLiteral("DELETE")};
    bool complete = identityValid;
    for (const auto &table : trackedTables()) {
        for (const auto &operation : operations) complete = complete && installed.contains(triggerName(table.name, operation));
    }
    if (complete) return true;

    SqlUtil::Savepoint migration(m_db, &m_lastError);
    if (!migration.active()) return false;
    const auto exec = [&](const QString &sql) {
        if (query.exec(sql)) return true;
        m_lastError = query.lastError().text();
        return false;
    };
    if (!exec(QStringLiteral("CREATE TABLE IF NOT EXISTS search_content_state (singleton INTEGER PRIMARY KEY CHECK(singleton=1), database_id TEXT NOT NULL, revision INTEGER NOT NULL CHECK(revision>=0))"))) return false;
    if (!exec(QStringLiteral("DELETE FROM search_content_state"))) return false;
    query.prepare(QStringLiteral("INSERT INTO search_content_state VALUES(1, ?, 0)"));
    query.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    for (const auto &table : trackedTables()) {
        QStringList changed;
        for (const auto &column : table.columns) changed << QStringLiteral("OLD.%1 IS NOT NEW.%1").arg(column);
        for (const auto &operation : operations) {
            const QString name = triggerName(table.name, operation);
            if (!exec(QStringLiteral("DROP TRIGGER IF EXISTS %1").arg(name))) return false;
            const QString when = operation == QStringLiteral("UPDATE")
                ? QStringLiteral(" WHEN ") + changed.join(QStringLiteral(" OR ")) : QString();
            if (!exec(QStringLiteral("CREATE TRIGGER %1 AFTER %2 ON %3%4 BEGIN UPDATE search_content_state SET revision=revision+1 WHERE singleton=1; END")
                          .arg(name, operation, table.name, when))) return false;
        }
    }
    return migration.commit();
}

Database::SearchContentState Database::searchContentState() const
{
    SearchContentState state;
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT database_id, revision FROM search_content_state WHERE singleton=1")) || !query.next()) return state;
    bool ok = false;
    state.revision = query.value(1).toLongLong(&ok);
    if (!ok || state.revision < 0) return {};
    state.databaseId = query.value(0).toString();
    const QFileInfo file(m_db.databaseName());
    state.databasePath = file.canonicalFilePath();
    if (state.databasePath.isEmpty()) state.databasePath = file.absoluteFilePath();
    return state;
}
