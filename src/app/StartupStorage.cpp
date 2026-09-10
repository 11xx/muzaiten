#include "app/StartupStorage.h"

#include "app/AppPaths.h"
#include "db/PlaylistDatabase.h"
#include "db/Schema.h"
#include "db/SettingsStore.h"
#include "db/SqlUtil.h"
#include "features/FeatureStore.h"
#include "scrobble/ListenHistoryStore.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>

namespace StartupStorage {
namespace {

struct Store {
    QString name;
    QString path;
    QString versionTable;
    QString versionKey;
    int version;
    bool required;
    bool writable = true;
};

QString inspectStore(const Store &store)
{
    const QFileInfo file(store.path);
    QFileInfo parent(file.absolutePath());
    while (!parent.exists() && parent.absoluteFilePath() != parent.absolutePath()) {
        parent.setFile(parent.absolutePath());
    }
    if (!parent.isDir() || !parent.isExecutable() || (store.writable && !parent.isWritable())) {
        return QStringLiteral("The storage directory is unavailable or not writable");
    }
    if (!file.exists()) return {};
    if (!file.isFile() || !file.isReadable() || (store.writable && !file.isWritable())) {
        return QStringLiteral("The database is not a readable, writable regular file");
    }
    const QString connection = QStringLiteral("startup-inspect-") + QUuid::createUuid().toString(QUuid::Id128);
    QString error;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(store.path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=1000"));
        if (!db.open()) {
            error = db.lastError().text();
        } else {
            QSqlQuery query(db);
            if (!query.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table'"))) {
                error = query.lastError().text();
            } else {
                QSet<QString> tables;
                while (query.next()) tables.insert(query.value(0).toString());
                query.finish();
                if (!store.versionTable.isEmpty()) SqlUtil::validateSchemaVersion(db, store.versionTable, store.versionKey, store.version, &error);
                const QMap<QString, QPair<QString, QString>> probes{
                    {QStringLiteral("library"), {QStringLiteral("tracks"), QStringLiteral("path, title, artist_name")}},
                    {QStringLiteral("playlists"), {QStringLiteral("playlists"), QStringLiteral("id, name")}},
                    {QStringLiteral("state"), {QStringLiteral("settings"), QStringLiteral("key, value")}},
                    {QStringLiteral("history"), {QStringLiteral("listens"), QStringLiteral("listened_at, title, artist, track_json")}},
                    {QStringLiteral("artwork"), {QStringLiteral("artwork_blobs"), QStringLiteral("cache_key, data")}},
                    {QStringLiteral("features"), {QStringLiteral("files"), QStringLiteral("path, status")}},
                };
                const auto probe = probes.value(store.name);
                if (error.isEmpty() && !tables.isEmpty()) {
                    if (!tables.contains(probe.first)) error = QStringLiteral("Unrecognized database schema");
                    else if (!query.exec(QStringLiteral("SELECT %1 FROM %2 LIMIT 0").arg(probe.second, probe.first))) error = query.lastError().text();
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return error;
}

} // namespace

bool Report::ok() const
{
    return std::ranges::none_of(issues, [](const Issue &issue) { return issue.required; });
}

QJsonObject Report::json() const
{
    QJsonArray rows;
    for (const auto &issue : issues) {
        rows.append(QJsonObject{{QStringLiteral("store"), issue.store}, {QStringLiteral("path"), issue.path},
            {QStringLiteral("error"), issue.error}, {QStringLiteral("required"), issue.required}});
    }
    return {{QStringLiteral("schema"), QStringLiteral("muzaiten-storage-health/1")},
        {QStringLiteral("phase"), phase},
        {QStringLiteral("ok"), ok()}, {QStringLiteral("code"), !ok() ? QStringLiteral("storage_unavailable")
            : issues.isEmpty() ? QStringLiteral("ready") : QStringLiteral("degraded")},
        {QStringLiteral("issues"), rows}};
}

QString Report::summary() const
{
    QStringList lines;
    for (const auto &issue : issues) {
        lines.append(QStringLiteral("%1 (%2): %3\n%4")
            .arg(issue.store, issue.required ? QStringLiteral("required") : QStringLiteral("optional"), issue.error, issue.path));
    }
    return lines.join(QStringLiteral("\n\n"));
}

Failure::Failure(Report value)
    : std::runtime_error(value.summary().toStdString()), report(std::move(value))
{
}

[[noreturn]] void fail(const QString &store, const QString &path, const QString &error)
{
    throw Failure(Report{{Issue{store, QFileInfo(path).absoluteFilePath(), error, true}}, QStringLiteral("initialize")});
}

Report inspect()
{
    const QDir data(AppPaths::dataDir()), state(AppPaths::stateDir()), cache(AppPaths::cacheDir());
    const QVector<Store> stores{
        {QStringLiteral("library"), data.filePath(QStringLiteral("library.sqlite")), QStringLiteral("schema_migrations"), {}, Schema::currentVersion, true},
        {QStringLiteral("playlists"), data.filePath(QStringLiteral("playlists.sqlite")), QStringLiteral("schema_migrations"), {}, PlaylistDatabase::currentSchemaVersion, true},
        {QStringLiteral("state"), state.filePath(QStringLiteral("state.sqlite")), QStringLiteral("meta"), QStringLiteral("schemaVersion"), SettingsStore::currentSchemaVersion, true},
        {QStringLiteral("history"), data.filePath(QStringLiteral("history.sqlite")), QStringLiteral("meta"), QStringLiteral("schemaVersion"), ListenHistoryStore::currentSchemaVersion, true},
        {QStringLiteral("artwork"), cache.filePath(QStringLiteral("artwork.sqlite")), {}, {}, 0, false},
        {QStringLiteral("features"), data.filePath(QStringLiteral("features.sqlite")), QStringLiteral("meta"), QStringLiteral("schema_version"), FeatureStore::maximumSchemaVersion, false, false},
    };
    Report report;
    for (const auto &store : stores) {
        const QString error = inspectStore(store);
        if (!error.isEmpty()) report.issues.append({store.name, store.path, error, store.required});
    }
    return report;
}

} // namespace StartupStorage
