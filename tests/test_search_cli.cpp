#include "cli/SearchCli.h"
#include "core/Rating.h"
#include "core/Track.h"
#include "db/Database.h"
#include "scrobble/ListenHistoryStore.h"
#include "search/SearchIndex.h"
#include "search/SearchQuery.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>
#include <QVector>

namespace {

Track makeTrack(const QString &dir, const QString &filename, const QString &title, int rating)
{
    Track track;
    track.path = dir + QStringLiteral("/Artist/Album/") + filename;
    track.parentDir = dir + QStringLiteral("/Artist/Album");
    track.filename = filename;
    track.title = title;
    track.artistName = QStringLiteral("BEGIN");
    track.albumArtistName = QStringLiteral("BEGIN");
    track.albumTitle = QStringLiteral("Best");
    track.trackNumber = filename.left(2).toInt();
    track.rating0To100 = rating;
    track.ratingSource = Rating::Source::VorbisRating;
    track.fileSize = 10;
    track.fileMtime = 20;
    return track;
}

struct SemanticFeatureRow {
    QString path;
    qint64 groupId = -1;
    QByteArray vectorBlob;
};

bool execFeatureSql(QSqlQuery &query, const QString &sql, QString *error)
{
    if (query.exec(sql)) {
        return true;
    }
    if (error != nullptr) {
        *error = query.lastError().text() + QStringLiteral(": ") + sql;
    }
    return false;
}

bool createSemanticFeatureFixture(const QString &path, const QVector<SemanticFeatureRow> &rows, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile::remove(path);
    const QString connectionName =
        QStringLiteral("semantic-feature-fixture-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool ok = true;

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(path);
        if (!db.open()) {
            if (error != nullptr) {
                *error = db.lastError().text();
            }
            ok = false;
        }

        if (ok) {
            QSqlQuery query(db);
            ok = ok && execFeatureSql(query, QStringLiteral("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT)"), error);
            ok = ok && execFeatureSql(query, QStringLiteral(
                                          "CREATE TABLE files("
                                          " path TEXT PRIMARY KEY,"
                                          " mtime INTEGER NOT NULL,"
                                          " size INTEGER NOT NULL,"
                                          " duration_ms INTEGER,"
                                          " decode_hash TEXT,"
                                          " chromaprint_fp BLOB,"
                                          " content_group_id INTEGER,"
                                          " analyzed_at INTEGER NOT NULL,"
                                          " status TEXT NOT NULL DEFAULT 'ok')"),
                                      error);
            ok = ok && execFeatureSql(query, QStringLiteral(
                                          "CREATE TABLE content_groups(id INTEGER PRIMARY KEY AUTOINCREMENT)"),
                                      error);
            ok = ok && execFeatureSql(query, QStringLiteral(
                                          "CREATE TABLE features("
                                          " content_group_id INTEGER PRIMARY KEY,"
                                          " bliss_vector BLOB NOT NULL,"
                                          " tempo_bpm REAL,"
                                          " loudness REAL,"
                                          " energy REAL,"
                                          " brightness REAL,"
                                          " extractor TEXT NOT NULL,"
                                          " version TEXT NOT NULL)"),
                                      error);
            ok = ok && execFeatureSql(query, QStringLiteral(
                                          "CREATE TABLE embeddings("
                                          " content_group_id INTEGER PRIMARY KEY,"
                                          " model TEXT NOT NULL,"
                                          " version TEXT NOT NULL,"
                                          " dim INTEGER NOT NULL,"
                                          " vector BLOB NOT NULL)"),
                                      error);
            ok = ok && execFeatureSql(query, QStringLiteral(
                                          "CREATE TABLE track_neighbors("
                                          " content_group_id INTEGER NOT NULL,"
                                          " neighbor_group_id INTEGER NOT NULL,"
                                          " rank INTEGER NOT NULL,"
                                          " cosine REAL NOT NULL,"
                                          " PRIMARY KEY(content_group_id, rank))"),
                                      error);
        }

        if (ok) {
            QSqlQuery meta(db);
            meta.prepare(QStringLiteral("INSERT INTO meta(key, value) VALUES('schema_version', '2')"));
            if (!meta.exec()) {
                if (error != nullptr) {
                    *error = meta.lastError().text();
                }
                ok = false;
            }
        }

        QSet<qint64> groups;
        if (ok) {
            for (const SemanticFeatureRow &row : rows) {
                if (groups.contains(row.groupId)) {
                    continue;
                }
                QSqlQuery group(db);
                group.prepare(QStringLiteral("INSERT INTO content_groups(id) VALUES(?)"));
                group.addBindValue(row.groupId);
                if (!group.exec()) {
                    if (error != nullptr) {
                        *error = group.lastError().text();
                    }
                    ok = false;
                    break;
                }
                groups.insert(row.groupId);
            }
        }

        if (ok) {
            for (const SemanticFeatureRow &row : rows) {
                QSqlQuery file(db);
                file.prepare(QStringLiteral(
                    "INSERT INTO files(path, mtime, size, duration_ms, decode_hash, chromaprint_fp, "
                    "content_group_id, analyzed_at, status) VALUES(?, 1, 1, 1000, ?, ?, ?, 1000, 'ok')"));
                file.addBindValue(row.path);
                file.addBindValue(QStringLiteral("hash-%1").arg(row.groupId));
                file.addBindValue(QByteArray::fromHex("01020304"));
                file.addBindValue(row.groupId);
                if (!file.exec()) {
                    if (error != nullptr) {
                        *error = file.lastError().text();
                    }
                    ok = false;
                    break;
                }

                QSqlQuery embedding(db);
                embedding.prepare(QStringLiteral(
                    "INSERT INTO embeddings(content_group_id, model, version, dim, vector) "
                    "VALUES(?, 'fixture-model', 'fixture-version', ?, ?)"));
                embedding.addBindValue(row.groupId);
                embedding.addBindValue(row.vectorBlob.size() / static_cast<int>(sizeof(float)));
                embedding.addBindValue(row.vectorBlob);
                if (!embedding.exec()) {
                    if (error != nullptr) {
                        *error = embedding.lastError().text();
                    }
                    ok = false;
                    break;
                }
            }
        }

        db.close();
    }

    QSqlDatabase::removeDatabase(connectionName);
    return ok;
}

QString muzaitenCtlPath()
{
    QDir buildDir(QCoreApplication::applicationDirPath());
    buildDir.cdUp();
    return buildDir.filePath(QStringLiteral("muzaitenctl"));
}

Track radioLearnTrack(const QString &path)
{
    Track track;
    track.path = path;
    track.title = QFileInfo(path).completeBaseName();
    track.artistName = QStringLiteral("Radio Fixture");
    track.albumArtistName = track.artistName;
    track.albumTitle = QStringLiteral("Learn");
    track.durationMs = 240000;
    return track;
}

QVector<ListenHistoryStore::RadioPickComponent> pickComponents(const QString &name, double value)
{
    return {{name, value}};
}

void recordRadioPick(ListenHistoryStore &store,
                     const Track &track,
                     qint64 occurredAtSecs,
                     const QVector<ListenHistoryStore::RadioPickComponent> &components)
{
    ListenHistoryStore::RadioPickEvent pick;
    pick.occurredAtSecs = occurredAtSecs;
    pick.track = track;
    pick.sessionKind = QStringLiteral("fixture");
    pick.exploration0To100 = 30;
    pick.weightsJson = QByteArrayLiteral("{}");
    pick.components = components;
    for (const ListenHistoryStore::RadioPickComponent &component : components) {
        pick.score += component.value;
    }
    QVERIFY(store.recordRadioPick(pick));
}

void recordPlayEvent(ListenHistoryStore &store,
                     const Track &track,
                     qint64 startedAtSecs,
                     const QString &source,
                     const QString &outcome,
                     qint64 playedMs,
                     qint64 durationMs)
{
    ListenHistoryStore::PlayEvent event;
    event.startedAtSecs = startedAtSecs;
    event.endedAtSecs = startedAtSecs + 60;
    event.playedMs = playedMs;
    event.durationMs = durationMs;
    event.outcome = outcome;
    event.source = source;
    event.shuffleMode = QStringLiteral("radio");
    event.sessionId = QStringLiteral("radio-learn-fixture");
    event.track = track;
    QVERIFY(store.recordPlayEvent(event) > 0);
}

void createRadioLearnFixture(const QString &dataDir)
{
    QDir().mkpath(dataDir);
    const QString libraryPath = QDir(dataDir).filePath(QStringLiteral("library.sqlite"));
    const QString historyPath = QDir(dataDir).filePath(QStringLiteral("history.sqlite"));
    QFile::remove(historyPath);
    QFile::remove(historyPath + QStringLiteral("-wal"));
    QFile::remove(historyPath + QStringLiteral("-shm"));

    Database db(QStringLiteral("radio-learn-library-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(libraryPath), qPrintable(db.lastError()));

    ListenHistoryStore store(historyPath);
    QVERIFY(store.isOpen());

    const Track early = radioLearnTrack(dataDir + QStringLiteral("/early.flac"));
    const Track finished = radioLearnTrack(dataDir + QStringLiteral("/finished.flac"));
    const Track late = radioLearnTrack(dataDir + QStringLiteral("/late.flac"));
    const Track unmatched = radioLearnTrack(dataDir + QStringLiteral("/unmatched.flac"));

    recordRadioPick(store, early, 1000, pickComponents(QStringLiteral("genre"), 3.0));
    recordPlayEvent(store, early, 900, QStringLiteral("radio"), QStringLiteral("finished"), 240000, 240000);
    recordPlayEvent(store, early, 1005, QStringLiteral("queue_auto"), QStringLiteral("skipped"), 1000, 240000);
    recordPlayEvent(store, early, 1010, QStringLiteral("radio"), QStringLiteral("skipped"), 30000, 240000);

    recordRadioPick(store, finished, 2000, pickComponents(QStringLiteral("rating"), 1.5));
    recordPlayEvent(store, finished, 2010, QStringLiteral("radio"), QStringLiteral("finished"), 240000, 240000);

    recordRadioPick(store, late, 3000, pickComponents(QStringLiteral("rating"), 1.5));
    recordPlayEvent(store, late, 3010, QStringLiteral("radio"), QStringLiteral("skipped"), 200000, 240000);

    recordRadioPick(store, unmatched, 4000, pickComponents(QStringLiteral("genre"), 3.0));
    recordPlayEvent(store, unmatched, 4000 + 12 * 60 * 60 + 1,
                    QStringLiteral("radio"), QStringLiteral("skipped"), 1000, 240000);
}

} // namespace

// End-to-end exercise of the standalone CLI search index path: build-from-DB on
// a cache miss, reuse on a hit, detect staleness after a change, and rebuild on
// --refresh.
class SearchCliTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_temp;

private slots:
    void initTestCase()
    {
        QVERIFY(m_temp.isValid());
        // Isolate AppPaths to the temp dir so we never touch real data/cache.
        qputenv("MUZAITEN_DATA_DIR", (m_temp.path() + QStringLiteral("/data")).toUtf8());
        qputenv("MUZAITEN_CACHE_DIR", (m_temp.path() + QStringLiteral("/cache")).toUtf8());
    }

    void buildsReusesAndRefreshes()
    {
        const QString dbPath = SearchCli::libraryDbPath();
        {
            Database db(QStringLiteral("cli-setup-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            QVERIFY2(db.open(dbPath), qPrintable(db.lastError()));
            Track t = makeTrack(m_temp.path(), QStringLiteral("01.flac"), QString::fromUtf8("三線の花"), 80);
            t.titleSort = QStringLiteral("Sanshin no Hana");
            QVERIFY2(db.upsertTrack(t), qPrintable(db.lastError()));
            QVERIFY2(db.upsertTrack(makeTrack(m_temp.path(), QStringLiteral("02.flac"), QStringLiteral("Hello"), 60)),
                     qPrintable(db.lastError()));
        }

        QVERIFY(!QFile::exists(SearchCli::cachePath())); // nothing cached yet

        // Cold: builds from the DB, writes the cache, romaji query matches kanji.
        Search::SearchIndex idx;
        const SearchCli::LoadResult cold = SearchCli::loadIndex(idx, /*forceRefresh=*/false);
        QVERIFY2(cold.ok, qPrintable(cold.error));
        QVERIFY(cold.rebuilt);
        QVERIFY(!cold.usedCache);
        QCOMPARE(idx.size(), 2);
        QVERIFY(QFile::exists(SearchCli::cachePath()));
        int total = 0;
        QCOMPARE(idx.match(Search::SearchQuery::parse(QStringLiteral("sanshin")), false, {}, &total).size(), 1);

        // Warm: loads from the cache, not stale.
        Search::SearchIndex idx2;
        const SearchCli::LoadResult warm = SearchCli::loadIndex(idx2, false);
        QVERIFY(warm.ok);
        QVERIFY(warm.usedCache);
        QVERIFY(!warm.wasStale);
        QVERIFY(!warm.rebuilt);
        QCOMPARE(idx2.size(), 2);

        // Add a track → cache is stale but still used (shows the old 2).
        {
            Database db(QStringLiteral("cli-add-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            QVERIFY2(db.open(dbPath), qPrintable(db.lastError()));
            QVERIFY2(db.upsertTrack(makeTrack(m_temp.path(), QStringLiteral("03.flac"), QStringLiteral("Third"), 40)),
                     qPrintable(db.lastError()));
        }
        Search::SearchIndex idx3;
        const SearchCli::LoadResult stale = SearchCli::loadIndex(idx3, false);
        QVERIFY(stale.usedCache);
        QVERIFY(stale.wasStale);
        QCOMPARE(idx3.size(), 2);

        // --refresh rebuilds from the DB and clears staleness.
        Search::SearchIndex idx4;
        const SearchCli::LoadResult refreshed = SearchCli::loadIndex(idx4, /*forceRefresh=*/true);
        QVERIFY(refreshed.rebuilt);
        QVERIFY(!refreshed.wasStale);
        QCOMPARE(idx4.size(), 3);

        // After the refresh write, a plain load is fresh again.
        Search::SearchIndex idx5;
        const SearchCli::LoadResult afterRefresh = SearchCli::loadIndex(idx5, false);
        QVERIFY(afterRefresh.usedCache);
        QVERIFY(!afterRefresh.wasStale);
        QCOMPARE(idx5.size(), 3);
    }

    void streamRecordsYieldsEverything()
    {
        // The prior test left 3 tracks + a fresh cache. Streaming must yield them
        // all (from the cache), and again when forced to rebuild from the DB.
        int fromCache = 0;
        bool sawSanshin = false;
        const SearchCli::LoadResult cached = SearchCli::streamRecords(
            [&](const Search::SearchRecord &r) {
                ++fromCache;
                if (r.normTitle.contains(QStringLiteral("sanshin"))) {
                    sawSanshin = true;
                }
            },
            /*forceRefresh=*/false);
        QVERIFY(cached.ok);
        QVERIFY(cached.usedCache);
        QCOMPARE(fromCache, 3);
        QVERIFY(sawSanshin); // folded norm survived the streaming deserialize

        int fromDb = 0;
        const SearchCli::LoadResult rebuilt = SearchCli::streamRecords(
            [&](const Search::SearchRecord &) { ++fromDb; }, /*forceRefresh=*/true);
        QVERIFY(rebuilt.ok);
        QVERIFY(rebuilt.rebuilt);
        QCOMPARE(fromDb, 3);
    }

    void clearCacheRemovesFile()
    {
        // The previous test left a cache in place.
        QVERIFY(QFile::exists(SearchCli::cachePath()));
        QString path;
        QVERIFY(SearchCli::clearCache(&path));
        QCOMPARE(path, SearchCli::cachePath());
        QVERIFY(!QFile::exists(SearchCli::cachePath()));
        QVERIFY(!SearchCli::clearCache(nullptr)); // already gone
    }

    void semanticSearchRanksInjectedQueryVector()
    {
        const QString dbPath = SearchCli::libraryDbPath();
        Track warm = makeTrack(m_temp.path(), QStringLiteral("10.flac"), QStringLiteral("Warm Piano"), 70);
        warm.artistName = QStringLiteral("Alpha");
        warm.albumArtistName = QStringLiteral("Alpha");
        warm.albumTitle = QStringLiteral("Late Room");
        warm.codec = QStringLiteral("flac");
        warm.bitDepth = 24;
        warm.sampleRateHz = 96000;

        Track bright = makeTrack(m_temp.path(), QStringLiteral("11.flac"), QStringLiteral("Bright Synth"), 70);
        bright.artistName = QStringLiteral("Beta");
        bright.albumArtistName = QStringLiteral("Beta");
        bright.albumTitle = QStringLiteral("Morning");
        bright.codec = QStringLiteral("mp3");
        bright.bitrateKbps = 320;

        {
            Database db(QStringLiteral("semantic-cli-setup-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            QVERIFY2(db.open(dbPath), qPrintable(db.lastError()));
            QVERIFY2(db.upsertTrack(warm), qPrintable(db.lastError()));
            QVERIFY2(db.upsertTrack(bright), qPrintable(db.lastError()));
        }

        QString error;
        const QString featurePath = QDir(qEnvironmentVariable("MUZAITEN_DATA_DIR")).filePath(QStringLiteral("features.sqlite"));
        QVERIFY2(createSemanticFeatureFixture(featurePath,
                                              {
                                                  {warm.path, 101, QByteArray::fromHex("0000803f00000000")},
                                                  {bright.path, 102, QByteArray::fromHex("000000000000803f")},
                                              },
                                              &error),
                 qPrintable(error));

        const QString ctlPath = muzaitenCtlPath();
        QVERIFY2(QFileInfo::exists(ctlPath), qPrintable(ctlPath));

        QProcess ctl;
        ctl.setProgram(ctlPath);
        ctl.setArguments({
            QStringLiteral("--json"),
            QStringLiteral("semantic-search"),
            QStringLiteral("warm piano"),
            QStringLiteral("--limit"),
            QStringLiteral("1"),
            QStringLiteral("--query-vector-json"),
            QStringLiteral("[1,0]"),
        });
        ctl.start();
        QVERIFY2(ctl.waitForFinished(10000), qPrintable(ctl.errorString()));
        QCOMPARE(ctl.exitStatus(), QProcess::NormalExit);
        QCOMPARE(ctl.exitCode(), 0);

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(ctl.readAllStandardOutput(), &parseError);
        QVERIFY2(parseError.error == QJsonParseError::NoError, qPrintable(parseError.errorString()));
        QVERIFY(document.isArray());
        const QJsonArray results = document.array();
        QCOMPARE(results.size(), 1);
        const QJsonObject result = results.at(0).toObject();
        QCOMPARE(result.value(QStringLiteral("content_group_id")).toInt(), 101);
        QCOMPARE(result.value(QStringLiteral("path")).toString(), warm.path);
        QCOMPARE(result.value(QStringLiteral("title")).toString(), warm.title);
        QVERIFY(result.value(QStringLiteral("score")).toDouble() > 0.99);
    }

    void semanticSearchWithoutSidecarReportsCleanError()
    {
        const QString ctlPath = muzaitenCtlPath();
        QVERIFY2(QFileInfo::exists(ctlPath), qPrintable(ctlPath));

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PATH"), m_temp.path() + QStringLiteral("/empty-bin"));

        QProcess ctl;
        ctl.setProgram(ctlPath);
        ctl.setProcessEnvironment(env);
        ctl.setArguments({
            QStringLiteral("semantic-search"),
            QStringLiteral("warm piano"),
            QStringLiteral("--limit"),
            QStringLiteral("1"),
        });
        ctl.start();
        QVERIFY2(ctl.waitForFinished(10000), qPrintable(ctl.errorString()));
        QCOMPARE(ctl.exitStatus(), QProcess::NormalExit);
        QCOMPARE(ctl.exitCode(), 1);
        QVERIFY(QString::fromUtf8(ctl.readAllStandardError())
                    .contains(QStringLiteral("semantic search requires the embedder sidecar")));
    }

    void radioLearnDryRunAndSaveUseJoinedTelemetry()
    {
        const QString dataDir = qEnvironmentVariable("MUZAITEN_DATA_DIR");
        createRadioLearnFixture(dataDir);

        const QString ctlPath = muzaitenCtlPath();
        QVERIFY2(QFileInfo::exists(ctlPath), qPrintable(ctlPath));

        QProcess dryRun;
        dryRun.setProgram(ctlPath);
        dryRun.setArguments({
            QStringLiteral("--json"),
            QStringLiteral("radio-learn"),
            QStringLiteral("--dry-run"),
            QStringLiteral("--min-samples"),
            QStringLiteral("3"),
        });
        dryRun.start();
        QVERIFY2(dryRun.waitForFinished(10000), qPrintable(dryRun.errorString()));
        QCOMPARE(dryRun.exitStatus(), QProcess::NormalExit);
        QCOMPARE(dryRun.exitCode(), 0);

        QJsonParseError parseError;
        const QJsonDocument dryDocument = QJsonDocument::fromJson(dryRun.readAllStandardOutput(), &parseError);
        QVERIFY2(parseError.error == QJsonParseError::NoError, qPrintable(parseError.errorString()));
        const QJsonObject dry = dryDocument.object();
        QVERIFY(dry.value(QStringLiteral("dry_run")).toBool());
        QCOMPARE(dry.value(QStringLiteral("sample_count")).toInt(), 3);
        QCOMPARE(dry.value(QStringLiteral("positive_labels")).toInt(), 1);
        QCOMPARE(dry.value(QStringLiteral("join_window_seconds")).toInt(), 12 * 60 * 60);
        QVERIFY(dry.value(QStringLiteral("components")).toArray().size() > 0);

        {
            Database db(QStringLiteral("radio-learn-dry-check-%1")
                            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            QVERIFY2(db.open(QDir(dataDir).filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));
            QVERIFY(db.radioWeightProfiles().isEmpty());
        }

        QProcess save;
        save.setProgram(ctlPath);
        save.setArguments({
            QStringLiteral("--json"),
            QStringLiteral("radio-learn"),
            QStringLiteral("--min-samples"),
            QStringLiteral("3"),
        });
        save.start();
        QVERIFY2(save.waitForFinished(10000), qPrintable(save.errorString()));
        QCOMPARE(save.exitStatus(), QProcess::NormalExit);
        QCOMPARE(save.exitCode(), 0);

        const QJsonDocument saveDocument = QJsonDocument::fromJson(save.readAllStandardOutput(), &parseError);
        QVERIFY2(parseError.error == QJsonParseError::NoError, qPrintable(parseError.errorString()));
        const QJsonObject saved = saveDocument.object();
        QVERIFY(!saved.value(QStringLiteral("dry_run")).toBool());
        QVERIFY(saved.value(QStringLiteral("profile")).toString().startsWith(QStringLiteral("learned-")));
        QCOMPARE(saved.value(QStringLiteral("sample_count")).toInt(), 3);
        QCOMPARE(saved.value(QStringLiteral("positive_labels")).toInt(), 1);

        Database db(QStringLiteral("radio-learn-save-check-%1")
                        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY2(db.open(QDir(dataDir).filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));
        const QVector<Database::RadioWeightProfile> profiles = db.radioWeightProfiles();
        QCOMPARE(profiles.size(), 1);
        QCOMPARE(profiles.first().name, saved.value(QStringLiteral("profile")).toString());
        QVERIFY(!profiles.first().weightsJson.isEmpty());
    }
};

QTEST_MAIN(SearchCliTest)
#include "test_search_cli.moc"
