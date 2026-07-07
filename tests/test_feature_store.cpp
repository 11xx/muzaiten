#include "features/FeatureStore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>

#include <optional>

class FeatureStoreTest final : public QObject {
    Q_OBJECT

private slots:
    void absentFileIsClosed();
    void versionMismatchIsClosed();
    void versionTwoIsOpen();
    void versionThreeIsOpen();
    void groupLookupsRoundTrip();
    void batchLookupsAndScalarsRoundTrip();
    void statusCountsRows();
    void contentGroupIdsHonorMinimumSize();
    void versionOneEmbeddingReadsAreEmpty();
    void embeddingsAndNeighborsRoundTrip();
};

namespace {

bool execSql(QSqlQuery &query, const QString &sql, QString *error)
{
    if (query.exec(sql)) {
        return true;
    }
    if (error != nullptr) {
        *error = query.lastError().text() + QStringLiteral(": ") + sql;
    }
    return false;
}

bool createFixtureDatabase(const QString &path, int schemaVersion, QString *error)
{
    const QString connectionName =
        QStringLiteral("feature-store-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
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
            ok = ok && execSql(query, QStringLiteral("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT)"), error);
            ok = ok && execSql(query, QStringLiteral(
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
            ok = ok && execSql(query, QStringLiteral(
                                      "CREATE TABLE content_groups(id INTEGER PRIMARY KEY AUTOINCREMENT)"),
                                  error);
            if (schemaVersion >= 3) {
                ok = ok && execSql(query, QStringLiteral(
                                          "CREATE TABLE features("
                                          " content_group_id INTEGER PRIMARY KEY,"
                                          " tempo_bpm REAL,"
                                          " loudness_lufs REAL,"
                                          " loudness_std_db REAL,"
                                          " spectral_centroid_mean_hz REAL,"
                                          " spectral_centroid_std_hz REAL,"
                                          " spectral_flatness_mean REAL,"
                                          " zero_crossing_rate REAL,"
                                          " onset_rate_hz REAL,"
                                          " energy REAL,"
                                          " extractor TEXT NOT NULL,"
                                          " version TEXT NOT NULL)"),
                                      error);
            } else {
                ok = ok && execSql(query, QStringLiteral(
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
            }
            if (schemaVersion >= 2) {
                ok = ok && execSql(query, QStringLiteral(
                                          "CREATE TABLE embeddings("
                                          " content_group_id INTEGER PRIMARY KEY,"
                                          " model TEXT NOT NULL,"
                                          " version TEXT NOT NULL,"
                                          " dim INTEGER NOT NULL,"
                                          " vector BLOB NOT NULL)"),
                                      error);
                ok = ok && execSql(query, QStringLiteral(
                                          "CREATE TABLE track_neighbors("
                                          " content_group_id INTEGER NOT NULL,"
                                          " neighbor_group_id INTEGER NOT NULL,"
                                          " rank INTEGER NOT NULL,"
                                          " cosine REAL NOT NULL,"
                                          " PRIMARY KEY(content_group_id, rank))"),
                                      error);
            }
        }

        if (ok) {
            QSqlQuery meta(db);
            meta.prepare(QStringLiteral("INSERT INTO meta(key, value) VALUES('schema_version', ?)"));
            meta.addBindValue(QString::number(schemaVersion));
            if (!meta.exec()) {
                if (error != nullptr) {
                    *error = meta.lastError().text();
                }
                ok = false;
            }
        }

        if (ok) {
            QSqlQuery groups(db);
            ok = ok && execSql(groups, QStringLiteral("INSERT INTO content_groups(id) VALUES(10)"), error);
            ok = ok && execSql(groups, QStringLiteral("INSERT INTO content_groups(id) VALUES(11)"), error);
        }

        auto insertFile = [&](const QString &filePath, qint64 mtime, qint64 size, qint64 durationMs,
                              const QString &hash, const QByteArray &fingerprint,
                              std::optional<qint64> groupId, const QString &status) {
            if (!ok) {
                return;
            }
            QSqlQuery insert(db);
            insert.prepare(QStringLiteral(
                "INSERT INTO files(path, mtime, size, duration_ms, decode_hash, chromaprint_fp, "
                "content_group_id, analyzed_at, status) VALUES(?, ?, ?, ?, ?, ?, ?, 1000, ?)"));
            insert.addBindValue(filePath);
            insert.addBindValue(mtime);
            insert.addBindValue(size);
            insert.addBindValue(durationMs);
            insert.addBindValue(hash);
            insert.addBindValue(fingerprint);
            insert.addBindValue(groupId.has_value() ? QVariant(*groupId) : QVariant());
            insert.addBindValue(status);
            if (!insert.exec()) {
                if (error != nullptr) {
                    *error = insert.lastError().text();
                }
                ok = false;
            }
        };

        insertFile(QStringLiteral("/music/a.flac"), 11, 100, 5000, QStringLiteral("hash-a"),
                   QByteArray::fromHex("01020304"), 10, QStringLiteral("ok"));
        insertFile(QStringLiteral("/music/a-copy.wav"), 12, 120, 5000, QStringLiteral("hash-a"),
                   QByteArray::fromHex("01020304"), 10, QStringLiteral("ok"));
        insertFile(QStringLiteral("/music/b.flac"), 13, 130, 7000, QStringLiteral("hash-b"),
                   QByteArray::fromHex("05060708"), 11, QStringLiteral("ok"));
        insertFile(QStringLiteral("/music/broken.flac"), 14, 140, 0, QString(),
                   QByteArray(), std::nullopt, QStringLiteral("decode_failed"));

        if (ok && schemaVersion >= 3) {
            QSqlQuery featureA(db);
            featureA.prepare(QStringLiteral(
                "INSERT INTO features(content_group_id, tempo_bpm, loudness_lufs, loudness_std_db, "
                "spectral_centroid_mean_hz, spectral_centroid_std_hz, spectral_flatness_mean, "
                "zero_crossing_rate, onset_rate_hz, energy, extractor, version) "
                "VALUES(10, 120.5, -12.0, 1.5, 2300.0, 120.0, 0.2, 0.04, 2.0, 0.75, 'fixture', 'dsp')"));
            if (!featureA.exec()) {
                if (error != nullptr) {
                    *error = featureA.lastError().text();
                }
                ok = false;
            }
        }
        if (ok && schemaVersion >= 3) {
            QSqlQuery featureB(db);
            featureB.prepare(QStringLiteral(
                "INSERT INTO features(content_group_id, tempo_bpm, loudness_lufs, loudness_std_db, "
                "spectral_centroid_mean_hz, spectral_centroid_std_hz, spectral_flatness_mean, "
                "zero_crossing_rate, onset_rate_hz, energy, extractor, version) "
                "VALUES(11, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 'fixture', 'dsp')"));
            if (!featureB.exec()) {
                if (error != nullptr) {
                    *error = featureB.lastError().text();
                }
                ok = false;
            }
        }

        if (ok && schemaVersion < 3) {
            QSqlQuery featureA(db);
            featureA.prepare(QStringLiteral(
                "INSERT INTO features(content_group_id, bliss_vector, tempo_bpm, loudness, energy, "
                "brightness, extractor, version) VALUES(10, ?, 120.5, -12.0, 0.75, 0.6, 'fixture', '1')"));
            featureA.addBindValue(QByteArray::fromHex("0000803f"));
            if (!featureA.exec()) {
                if (error != nullptr) {
                    *error = featureA.lastError().text();
                }
                ok = false;
            }
        }
        if (ok && schemaVersion < 3) {
            QSqlQuery featureB(db);
            featureB.prepare(QStringLiteral(
                "INSERT INTO features(content_group_id, bliss_vector, tempo_bpm, loudness, energy, "
                "brightness, extractor, version) VALUES(11, ?, NULL, NULL, NULL, NULL, 'fixture', '1')"));
            featureB.addBindValue(QByteArray::fromHex("00000040"));
            if (!featureB.exec()) {
                if (error != nullptr) {
                    *error = featureB.lastError().text();
                }
                ok = false;
            }
        }

        if (ok && schemaVersion >= 2) {
            QSqlQuery embeddingA(db);
            embeddingA.prepare(QStringLiteral(
                "INSERT INTO embeddings(content_group_id, model, version, dim, vector) "
                "VALUES(10, 'fixture-model', 'fixture-version', 2, ?)"));
            embeddingA.addBindValue(QByteArray::fromHex("0000803f00000000"));
            if (!embeddingA.exec()) {
                if (error != nullptr) {
                    *error = embeddingA.lastError().text();
                }
                ok = false;
            }
        }
        if (ok && schemaVersion >= 2) {
            QSqlQuery embeddingB(db);
            embeddingB.prepare(QStringLiteral(
                "INSERT INTO embeddings(content_group_id, model, version, dim, vector) "
                "VALUES(11, 'fixture-model', 'fixture-version', 2, ?)"));
            embeddingB.addBindValue(QByteArray::fromHex("000000000000803f"));
            if (!embeddingB.exec()) {
                if (error != nullptr) {
                    *error = embeddingB.lastError().text();
                }
                ok = false;
            }
        }
        if (ok && schemaVersion >= 2) {
            QSqlQuery neighbors(db);
            ok = ok && execSql(neighbors, QStringLiteral(
                                      "INSERT INTO track_neighbors(content_group_id, neighbor_group_id, rank, cosine) "
                                      "VALUES(10, 11, 1, 0.5)"),
                                  error);
            ok = ok && execSql(neighbors, QStringLiteral(
                                      "INSERT INTO track_neighbors(content_group_id, neighbor_group_id, rank, cosine) "
                                      "VALUES(11, 10, 1, 0.5)"),
                                  error);
        }

        db.close();
    }

    QSqlDatabase::removeDatabase(connectionName);
    return ok;
}

QString createFixture(QTemporaryDir &temp, int schemaVersion, QString *error)
{
    const QString path = temp.filePath(QStringLiteral("features.sqlite"));
    if (!createFixtureDatabase(path, schemaVersion, error)) {
        return {};
    }
    return path;
}

} // namespace

void FeatureStoreTest::absentFileIsClosed()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    FeatureStore store(temp.filePath(QStringLiteral("missing.sqlite")));
    QVERIFY(!store.isOpen());
    QCOMPARE(store.schemaVersion(), -1);
    QCOMPARE(store.contentGroupForPath(QStringLiteral("/music/a.flac")), -1);
    QVERIFY(store.contentGroupsForPaths({QStringLiteral("/music/a.flac")}).isEmpty());
    QVERIFY(store.pathsInGroup(10).isEmpty());
    QVERIFY(!store.scalarsForGroup(10).valid);
    QVERIFY(store.scalarsForGroups({10}).isEmpty());

    const FeatureStore::Status status = store.status();
    QCOMPARE(status.files, 0);
    QCOMPARE(status.ok, 0);
    QCOMPARE(status.failed, 0);
    QCOMPARE(status.groups, 0);
    QCOMPARE(status.featured, 0);
}

void FeatureStoreTest::versionMismatchIsClosed()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    QString error;
    const QString path = createFixture(temp, 4, &error);
    QVERIFY2(!path.isEmpty(), qPrintable(error));
    FeatureStore store(path);
    QVERIFY(!store.isOpen());
    QCOMPARE(store.schemaVersion(), -1);
    QCOMPARE(store.contentGroupForPath(QStringLiteral("/music/a.flac")), -1);
}

void FeatureStoreTest::versionTwoIsOpen()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    QString error;
    const QString path = createFixture(temp, 2, &error);
    QVERIFY2(!path.isEmpty(), qPrintable(error));
    FeatureStore store(path);
    QVERIFY(store.isOpen());
    QCOMPARE(store.schemaVersion(), 2);
    QCOMPARE(store.contentGroupForPath(QStringLiteral("/music/a.flac")), 10);
    QVERIFY(store.scalarsForGroup(10).valid);
}

void FeatureStoreTest::versionThreeIsOpen()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    QString error;
    const QString path = createFixture(temp, 3, &error);
    QVERIFY2(!path.isEmpty(), qPrintable(error));
    FeatureStore store(path);
    QVERIFY(store.isOpen());
    QCOMPARE(store.schemaVersion(), 3);
    QCOMPARE(store.contentGroupForPath(QStringLiteral("/music/a.flac")), 10);

    const FeatureStore::Scalars groupA = store.scalarsForGroup(10);
    QVERIFY(groupA.valid);
    QCOMPARE(groupA.tempoBpm, 120.5);
    QCOMPARE(groupA.loudness, -12.0);
    QCOMPARE(groupA.energy, 0.75);
    QCOMPARE(groupA.brightness, 2300.0);
}

void FeatureStoreTest::groupLookupsRoundTrip()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    QString error;
    const QString path = createFixture(temp, 1, &error);
    QVERIFY2(!path.isEmpty(), qPrintable(error));
    FeatureStore store(path);
    QVERIFY(store.isOpen());
    QCOMPARE(store.schemaVersion(), 1);
    QCOMPARE(store.contentGroupForPath(QStringLiteral("/music/a.flac")), 10);
    QCOMPARE(store.contentGroupForPath(QStringLiteral("/music/a-copy.wav")), 10);
    QCOMPARE(store.contentGroupForPath(QStringLiteral("/music/b.flac")), 11);
    QCOMPARE(store.contentGroupForPath(QStringLiteral("/music/broken.flac")), -1);
    QCOMPARE(store.contentGroupForPath(QStringLiteral("/music/missing.flac")), -1);

    const QStringList paths = store.pathsInGroup(10);
    QCOMPARE(paths.size(), 2);
    QVERIFY(paths.contains(QStringLiteral("/music/a-copy.wav")));
    QVERIFY(paths.contains(QStringLiteral("/music/a.flac")));
    QVERIFY(store.pathsInGroup(999).isEmpty());
}

void FeatureStoreTest::batchLookupsAndScalarsRoundTrip()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    QString error;
    const QString path = createFixture(temp, 1, &error);
    QVERIFY2(!path.isEmpty(), qPrintable(error));
    FeatureStore store(path);
    const QHash<QString, qint64> groups = store.contentGroupsForPaths({
        QStringLiteral("/music/a.flac"),
        QStringLiteral("/music/b.flac"),
        QStringLiteral("/music/missing.flac"),
    });
    QCOMPARE(groups.size(), 2);
    QCOMPARE(groups.value(QStringLiteral("/music/a.flac")), 10);
    QCOMPARE(groups.value(QStringLiteral("/music/b.flac")), 11);

    const FeatureStore::Scalars groupA = store.scalarsForGroup(10);
    QVERIFY(groupA.valid);
    QCOMPARE(groupA.tempoBpm, 120.5);
    QCOMPARE(groupA.loudness, -12.0);
    QCOMPARE(groupA.energy, 0.75);
    QCOMPARE(groupA.brightness, 0.6);

    const FeatureStore::Scalars groupB = store.scalarsForGroup(11);
    QVERIFY(groupB.valid);
    QCOMPARE(groupB.tempoBpm, -1.0);
    QCOMPARE(groupB.loudness, 0.0);
    QCOMPARE(groupB.energy, -1.0);
    QCOMPARE(groupB.brightness, -1.0);
    QVERIFY(!store.scalarsForGroup(999).valid);

    const QHash<qint64, FeatureStore::Scalars> many = store.scalarsForGroups({10, 11, 999});
    QCOMPARE(many.size(), 2);
    QVERIFY(many.value(10).valid);
    QCOMPARE(many.value(10).tempoBpm, 120.5);
    QVERIFY(many.value(11).valid);
    QCOMPARE(many.value(11).energy, -1.0);
}

void FeatureStoreTest::statusCountsRows()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    QString error;
    const QString path = createFixture(temp, 1, &error);
    QVERIFY2(!path.isEmpty(), qPrintable(error));
    FeatureStore store(path);
    const FeatureStore::Status status = store.status();
    QCOMPARE(status.files, 4);
    QCOMPARE(status.ok, 3);
    QCOMPARE(status.failed, 1);
    QCOMPARE(status.groups, 2);
    QCOMPARE(status.featured, 2);
}

void FeatureStoreTest::contentGroupIdsHonorMinimumSize()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    QString error;
    const QString path = createFixture(temp, 1, &error);
    QVERIFY2(!path.isEmpty(), qPrintable(error));
    FeatureStore store(path);

    QCOMPARE(store.contentGroupIds(1), QVector<qint64>({10, 11}));
    QCOMPARE(store.contentGroupIds(2), QVector<qint64>({10}));
    QVERIFY(store.contentGroupIds(3).isEmpty());
}

void FeatureStoreTest::versionOneEmbeddingReadsAreEmpty()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    QString error;
    const QString path = createFixture(temp, 1, &error);
    QVERIFY2(!path.isEmpty(), qPrintable(error));
    FeatureStore store(path);
    QVERIFY(store.isOpen());
    QVERIFY(store.embeddingForGroup(10).isEmpty());
    QVERIFY(store.embeddingsForGroups({10, 11}).isEmpty());
    QVERIFY(store.neighborsOfGroup(10, 10).isEmpty());
}

void FeatureStoreTest::embeddingsAndNeighborsRoundTrip()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    QString error;
    const QString path = createFixture(temp, 2, &error);
    QVERIFY2(!path.isEmpty(), qPrintable(error));
    FeatureStore store(path);
    QVERIFY(store.isOpen());

    const QVector<float> groupA = store.embeddingForGroup(10);
    QCOMPARE(groupA.size(), 2);
    QCOMPARE(groupA.at(0), 1.0F);
    QCOMPARE(groupA.at(1), 0.0F);
    QVERIFY(store.embeddingForGroup(999).isEmpty());

    const QHash<qint64, QVector<float>> embeddings = store.embeddingsForGroups({10, 11, 999});
    QCOMPARE(embeddings.size(), 2);
    QCOMPARE(embeddings.value(11).size(), 2);
    QCOMPARE(embeddings.value(11).at(0), 0.0F);
    QCOMPARE(embeddings.value(11).at(1), 1.0F);

    const QList<QPair<qint64, double>> neighbors = store.neighborsOfGroup(10, 10);
    QCOMPARE(neighbors.size(), 1);
    QCOMPARE(neighbors.first().first, qint64(11));
    QCOMPARE(neighbors.first().second, 0.5);
    QVERIFY(store.neighborsOfGroup(10, 0).isEmpty());
    QVERIFY(store.neighborsOfGroup(999, 10).isEmpty());
}

QTEST_MAIN(FeatureStoreTest)
#include "test_feature_store.moc"
