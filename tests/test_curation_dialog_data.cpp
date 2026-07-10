#include "core/GenreTags.h"
#include "core/MetadataBlob.h"
#include "core/Track.h"
#include "db/Database.h"
#include "features/FeatureStore.h"
#include "indexer/DspVersion.h"
#include "reco/GenreCuration.h"
#include "ui/AudioAnalysisData.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

class CurationDialogDataTest final : public QObject {
    Q_OBJECT

private slots:
    void aliasValidationFoldsAndRejectsSelf();
    void genreReportRowsMarkAliasAndIgnoredGenres();
    void statusSummaryIncludesEmbeddingCoverage();
    void audioAnalysisProgressLabelsAreHumanReadable();
    void duplicateGroupsRankCopiesAndRespectPins();
};

namespace {

Track makeTrack(const QString &path, const QString &title, const QString &artist,
                const QString &genre = {}, const QString &codec = {},
                int bitDepth = 0, int sampleRateHz = 0, int bitrateKbps = 0)
{
    const QFileInfo info(path);
    Track track;
    track.path = path;
    track.parentDir = info.path();
    track.filename = info.fileName();
    track.title = title;
    track.artistName = artist;
    track.albumArtistName = artist;
    track.albumTitle = QStringLiteral("Album");
    track.durationMs = 240000;
    track.fileSize = 1024;
    track.fileMtime = 1234;
    track.codec = codec;
    track.bitDepth = bitDepth;
    track.sampleRateHz = sampleRateHz;
    track.bitrateKbps = bitrateKbps;
    if (!genre.isEmpty()) {
        MetadataBlob::FullMetadata metadata;
        metadata.tags.insert(QStringLiteral("GENRE"), {genre});
        const MetadataBlob::Encoded encoded = MetadataBlob::encode(metadata);
        track.fullMetadataBlob = encoded.data;
        track.fullMetadataRawSize = encoded.rawSize;
    }
    return track;
}

bool execSql(QSqlDatabase &db, const QString &sql, QString *error)
{
    QSqlQuery query(db);
    if (query.exec(sql)) {
        return true;
    }
    if (error != nullptr) {
        *error = query.lastError().text() + QStringLiteral(": ") + sql;
    }
    return false;
}

void createFeatureDb(const QString &path, const QVector<QPair<QString, qint64>> &files,
                     bool withEmbeddings = false)
{
    const QString connectionName =
        QStringLiteral("curation-feature-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QString error;
        QVERIFY2(execSql(db, QStringLiteral("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT)"), &error),
                 qPrintable(error));
        QVERIFY2(execSql(db, QStringLiteral(
                             "INSERT INTO meta(key, value) VALUES"
                             "('schema_version', '3'),"
                             "('dsp_version', 'muzaiten-dsp-test'),"
                             "('last_scan_finished_at', '1767225600'),"
                             "('last_scan_elapsed_secs', '2530.0'),"
                             "('last_scan_scanned', '12'),"
                             "('last_scan_skipped', '3'),"
                             "('last_scan_failed', '1'),"
                             "('last_scan_mean_ms_per_track', '1200.5'),"
                             "('last_scan_power', 'background')"),
                         &error),
                 qPrintable(error));
        QVERIFY2(execSql(db, QStringLiteral(
                             "CREATE TABLE files("
                             " path TEXT PRIMARY KEY, mtime INTEGER NOT NULL, size INTEGER NOT NULL,"
                             " duration_ms INTEGER, decode_hash TEXT, chromaprint_fp BLOB,"
                             " content_group_id INTEGER, analyzed_at INTEGER NOT NULL,"
                             " status TEXT NOT NULL DEFAULT 'ok')"),
                         &error),
                 qPrintable(error));
        QVERIFY2(execSql(db, QStringLiteral("CREATE TABLE content_groups(id INTEGER PRIMARY KEY)"), &error),
                 qPrintable(error));
        QVERIFY2(execSql(db, QStringLiteral(
                             "CREATE TABLE features("
                             " content_group_id INTEGER PRIMARY KEY, tempo_bpm REAL, loudness_lufs REAL,"
                             " loudness_std_db REAL, spectral_centroid_mean_hz REAL,"
                             " spectral_centroid_std_hz REAL, spectral_flatness_mean REAL,"
                             " zero_crossing_rate REAL, onset_rate_hz REAL, energy REAL,"
                             " extractor TEXT NOT NULL, version TEXT NOT NULL)"),
                         &error),
                 qPrintable(error));
        QSet<qint64> insertedGroups;
        for (const auto &file : files) {
            if (!insertedGroups.contains(file.second)) {
                QSqlQuery group(db);
                group.prepare(QStringLiteral("INSERT INTO content_groups(id) VALUES(?)"));
                group.addBindValue(file.second);
                QVERIFY2(group.exec(), qPrintable(group.lastError().text()));
                insertedGroups.insert(file.second);
            }
            QSqlQuery insert(db);
            insert.prepare(QStringLiteral(
                "INSERT INTO files(path, mtime, size, content_group_id, analyzed_at, status)"
                " VALUES(?, 1, 1024, ?, 100, 'ok')"));
            insert.addBindValue(file.first);
            insert.addBindValue(file.second);
            QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));
        }
        if (!files.isEmpty()) {
            QSqlQuery feature(db);
            feature.prepare(QStringLiteral(
                "INSERT INTO features(content_group_id, extractor, version) VALUES(?, 'test', 'v1')"));
            feature.addBindValue(files.first().second);
            QVERIFY2(feature.exec(), qPrintable(feature.lastError().text()));
        }
        if (withEmbeddings) {
            QVERIFY2(execSql(db, QStringLiteral(
                                 "CREATE TABLE embeddings("
                                 " content_group_id INTEGER PRIMARY KEY, model TEXT NOT NULL,"
                                 " version TEXT NOT NULL, dim INTEGER NOT NULL, vector BLOB NOT NULL)"),
                             &error),
                     qPrintable(error));
            QVERIFY2(execSql(db, QStringLiteral(
                                 "CREATE TABLE track_neighbors("
                                 " content_group_id INTEGER NOT NULL, neighbor_group_id INTEGER NOT NULL,"
                                 " rank INTEGER NOT NULL, cosine REAL NOT NULL,"
                                 " PRIMARY KEY(content_group_id, rank))"),
                             &error),
                     qPrintable(error));
            for (const auto &file : files) {
                QSqlQuery embedding(db);
                embedding.prepare(QStringLiteral(
                    "INSERT OR IGNORE INTO embeddings(content_group_id, model, version, dim, vector)"
                    " VALUES(?, 'test-clap', 'v1', 2, x'0000803f00000000')"));
                embedding.addBindValue(file.second);
                QVERIFY2(embedding.exec(), qPrintable(embedding.lastError().text()));
            }
            if (files.size() >= 2) {
                QSqlQuery neighbor(db);
                neighbor.prepare(QStringLiteral(
                    "INSERT INTO track_neighbors(content_group_id, neighbor_group_id, rank, cosine)"
                    " VALUES(?, ?, ?, 0.5)"));
                neighbor.addBindValue(files.at(0).second);
                neighbor.addBindValue(files.at(1).second);
                neighbor.addBindValue(1);
                QVERIFY2(neighbor.exec(), qPrintable(neighbor.lastError().text()));
                neighbor.prepare(QStringLiteral(
                    "INSERT INTO track_neighbors(content_group_id, neighbor_group_id, rank, cosine)"
                    " VALUES(?, ?, ?, 0.5)"));
                neighbor.addBindValue(files.at(1).second);
                neighbor.addBindValue(files.at(0).second);
                neighbor.addBindValue(1);
                QVERIFY2(neighbor.exec(), qPrintable(neighbor.lastError().text()));
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
}

const GenreCuration::ReportRow *findGenre(const QVector<GenreCuration::ReportRow> &rows, const QString &genre)
{
    const QString folded = GenreTags::folded(genre);
    for (const GenreCuration::ReportRow &row : rows) {
        if (row.genre == folded) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

void CurationDialogDataTest::aliasValidationFoldsAndRejectsSelf()
{
    const GenreCuration::AliasValidation valid =
        GenreCuration::validateAlias(QStringLiteral("  Alt Rock  "), QStringLiteral(" Rock "));
    QVERIFY(valid.ok());
    QCOMPARE(valid.aliasFolded, QStringLiteral("alt rock"));
    QCOMPARE(valid.canonicalFolded, QStringLiteral("rock"));

    const GenreCuration::AliasValidation self =
        GenreCuration::validateAlias(QStringLiteral("Rock"), QStringLiteral(" rock "));
    QVERIFY(!self.ok());
    QVERIFY(self.error.contains(QStringLiteral("different")));
}

void CurationDialogDataTest::genreReportRowsMarkAliasAndIgnoredGenres()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("curation-genre-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(makeTrack(dir.filePath(QStringLiteral("a.flac")), QStringLiteral("A"), QStringLiteral("Artist A"), QStringLiteral("Alt Rock"))),
             qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(makeTrack(dir.filePath(QStringLiteral("b.flac")), QStringLiteral("B"), QStringLiteral("Artist B"), QStringLiteral("Rock"))),
             qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(makeTrack(dir.filePath(QStringLiteral("c.flac")), QStringLiteral("C"), QStringLiteral("Artist C"), QStringLiteral("Pop"))),
             qPrintable(db.lastError()));
    QVERIFY2(db.setGenreAlias(QStringLiteral("Alt Rock"), QStringLiteral("Rock")), qPrintable(db.lastError()));
    QVERIFY2(db.setRadioGenreIgnored(QStringLiteral("Pop"), true), qPrintable(db.lastError()));

    int tagged = 0;
    const QVector<GenreCuration::ReportRow> rows = GenreCuration::buildReportRows(db, &tagged);
    QCOMPARE(tagged, 3);
    const GenreCuration::ReportRow *alt = findGenre(rows, QStringLiteral("Alt Rock"));
    QVERIFY(alt != nullptr);
    QCOMPARE(alt->canonical, QStringLiteral("rock"));
    QCOMPARE(alt->status, QStringLiteral("alias"));
    const GenreCuration::ReportRow *pop = findGenre(rows, QStringLiteral("Pop"));
    QVERIFY(pop != nullptr);
    QCOMPARE(pop->status, QStringLiteral("ignored"));
}

void CurationDialogDataTest::statusSummaryIncludesEmbeddingCoverage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString one = dir.filePath(QStringLiteral("one.flac"));
    const QString two = dir.filePath(QStringLiteral("two.flac"));
    const QString features = dir.filePath(QStringLiteral("features.sqlite"));
    createFeatureDb(features, {{one, 10}, {two, 11}}, true);

    const AudioAnalysisData::StatusSummary summary = AudioAnalysisData::loadStatus(features);
    QVERIFY(summary.found);
    QVERIFY(summary.open);
    QCOMPARE(summary.schemaVersion, 3);
    QCOMPARE(summary.status.dspVersion, QStringLiteral("muzaiten-dsp-test"));
    QCOMPARE(summary.status.files, qint64(2));
    QCOMPARE(summary.status.groups, qint64(2));
    QCOMPARE(summary.status.featured, qint64(1));
    QCOMPARE(summary.status.featuredFresh, qint64(0));
    QCOMPARE(summary.status.featuredStale, qint64(1));
    QCOMPARE(summary.status.expectedDspVersion, QLatin1String(Dsp::kDspVersion));
    QCOMPARE(summary.status.embeddedGroups, qint64(2));
    QCOMPARE(summary.status.embeddingModel, QStringLiteral("test-clap"));
    QCOMPARE(summary.status.embeddingVersion, QStringLiteral("v1"));
    QCOMPARE(summary.status.neighborRows, qint64(2));
    QVERIFY(summary.lastRun.present);
    QCOMPARE(summary.lastRun.elapsedSecs, 2530.0);
    QCOMPARE(summary.lastRun.scanned, 12);
    QCOMPARE(summary.lastRun.skipped, 3);
    QCOMPARE(summary.lastRun.failed, 1);
    QCOMPARE(summary.lastRun.meanMsPerTrack, 1200.5);
    QCOMPARE(summary.lastRun.power, QStringLiteral("background"));
}

void CurationDialogDataTest::audioAnalysisProgressLabelsAreHumanReadable()
{
    AudioAnalysisData::LiveStatus status;
    status.phase = AudioAnalysisData::LiveStatus::Phase::AnalyzingFiles;
    status.analyzed = 1234;
    status.total = 56789;
    status.rate = 1.4;
    status.etaSecs = 3900;
    status.elapsedSecs = 192;
    QCOMPARE(AudioAnalysisData::compactDuration(3900), QStringLiteral("1h05m"));
    QCOMPARE(AudioAnalysisData::compactDuration(750), QStringLiteral("12m30s"));
    QCOMPARE(AudioAnalysisData::compactDuration(45), QStringLiteral("45s"));
    QCOMPARE(AudioAnalysisData::progressLabel(status),
             QStringLiteral("Analyzing… 1234/56789 · 1.4/s · ~1h05m left · 03:12 elapsed"));

    status.phase = AudioAnalysisData::LiveStatus::Phase::WritingFeatures;
    status.analyzed = 700;
    status.total = 70590;
    status.rate = 0.5;
    status.etaSecs = 139780;
    status.elapsedSecs = 7200;
    QCOMPARE(AudioAnalysisData::phaseLabel(status.phase), QStringLiteral("Writing features"));
    QCOMPARE(AudioAnalysisData::progressLabel(status),
             QStringLiteral("Writing features… 700/70590 groups · 0.5/s · ~38h49m left · 2:00:00 elapsed"));

    QCOMPARE(AudioAnalysisData::finalSummary(1234, 56, 2, 1201, 2530.0),
             QStringLiteral("Audio analysis: scanned 1234, skipped 56, failed 2, groups 1201 — 42m 10s (2.1s/track)"));
    QCOMPARE(AudioAnalysisData::finalSummary(1234, 56, 2, 1201, 2530.0, 64012),
             QStringLiteral("Audio analysis: scanned 1234, skipped 56, failed 2, groups 1201, features written 64012 — 42m 10s (2.1s/track)"));
}

void CurationDialogDataTest::duplicateGroupsRankCopiesAndRespectPins()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString lowPath = dir.filePath(QStringLiteral("low.mp3"));
    const QString highPath = dir.filePath(QStringLiteral("high.flac"));
    Database db(QStringLiteral("curation-duplicates-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(makeTrack(lowPath, QStringLiteral("Low"), QStringLiteral("Artist"), {},
                                      QStringLiteral("mp3"), 0, 44100, 320)),
             qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(makeTrack(highPath, QStringLiteral("High"), QStringLiteral("Artist"), {},
                                      QStringLiteral("flac"), 24, 96000, 0)),
             qPrintable(db.lastError()));

    const QString featuresPath = dir.filePath(QStringLiteral("features.sqlite"));
    createFeatureDb(featuresPath, {{lowPath, 10}, {highPath, 10}});
    FeatureStore features(featuresPath);
    QVERIFY(features.isOpen());

    QVector<AudioAnalysisData::DuplicateGroup> groups = AudioAnalysisData::loadDuplicateGroups(db, features);
    QCOMPARE(groups.size(), qsizetype(1));
    QCOMPARE(groups.first().bestPath, highPath);
    QVERIFY(groups.first().copies.first().best);

    QVERIFY2(db.setContentGroupPin(10, lowPath), qPrintable(db.lastError()));
    groups = AudioAnalysisData::loadDuplicateGroups(db, features);
    QCOMPARE(groups.size(), qsizetype(1));
    QCOMPARE(groups.first().bestPath, lowPath);
    QCOMPARE(groups.first().copies.first().path, lowPath);
    QVERIFY(groups.first().copies.first().pinned);
}

QTEST_MAIN(CurationDialogDataTest)
#include "test_curation_dialog_data.moc"
