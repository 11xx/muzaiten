#include "ExportPublicationFixture.h"
#include "fs/FilePublish.h"
#include "indexer/DspVersion.h"
#include "indexer/FeaturesPack.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

using namespace ExportPublicationFixture;

namespace {

const QString packRefusal = QStringLiteral("Export needs a regular file destination and --replace to overwrite it.");

struct ExportResult {
    bool published = false;
    int code = 0;
    QString message;
};

// Creates the smallest schema-5 store the export reads: one analysed recording.
bool createStore(const QString &path)
{
    const QString connection = QStringLiteral("features-pack-publish-fixture");
    bool created = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        created = db.open();
        QSqlQuery query(db);
        const QStringList statements{
            QStringLiteral("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT)"),
            QStringLiteral("INSERT INTO meta VALUES('schema_version', '5')"),
            QStringLiteral("CREATE TABLE files(path TEXT PRIMARY KEY, status TEXT, content_group_id INTEGER, "
                           "decode_hash TEXT)"),
            QStringLiteral("CREATE TABLE features(content_group_id INTEGER PRIMARY KEY, version TEXT)"),
            QStringLiteral("CREATE TABLE file_features(path TEXT PRIMARY KEY, extractor TEXT, version TEXT, "
                           "tempo_bpm REAL, loudness_lufs REAL, loudness_std_db REAL, "
                           "spectral_centroid_mean_hz REAL, spectral_centroid_std_hz REAL, "
                           "spectral_flatness_mean REAL, zero_crossing_rate REAL, onset_rate_hz REAL, energy REAL)"),
            QStringLiteral("INSERT INTO files VALUES('/synthetic/one.flac', 'ok', 1, '%1')")
                .arg(QString(64, QLatin1Char('a'))),
            QStringLiteral("INSERT INTO features VALUES(1, '%1')").arg(QLatin1String(Dsp::kDspVersion)),
            QStringLiteral("INSERT INTO file_features VALUES('/synthetic/one.flac', 'muzaiten-dsp', '%1', "
                           "120, -14, 3, 2000, 500, 0.2, 0.05, 2, 0.5)")
                .arg(QLatin1String(Dsp::kDspVersion)),
        };
        for (const QString &statement : statements) {
            created = created && query.exec(statement);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return created;
}

ExportResult exportPack(const QString &path, const QString &store, bool replace)
{
    QStringList arguments{QStringLiteral("export"), path, QStringLiteral("--features"), store,
                          QStringLiteral("--json")};
    if (replace) {
        arguments.append(QStringLiteral("--replace"));
    }
    try {
        FeaturesPack::run(arguments);
        return {true, 0, {}};
    } catch (const FeaturesPack::Error &error) {
        return {false, error.code(), QString::fromStdString(error.what())};
    }
}

} // namespace

class FeaturesPackPublishTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void exportPublishesCompletePack();
    void exportRefusesExistingFile();
    void exportRefusesExistingSymlink();
    void exportRefusesDestinationAppearingBeforePublication();
    void exportRefusesSymlinkAppearingBeforePublication();
    void exportRefusesDestinationWithoutHardLinks();
    void exportReplaceIsExplicitAfterDestinationAppeared();

private:
    QString freshDirectory(const QString &name) const;

    QTemporaryDir m_temp;
    QString m_store;
};

void FeaturesPackPublishTest::init()
{
    QVERIFY(m_temp.isValid());
    m_store = m_temp.filePath(QStringLiteral("features.sqlite"));
    QFile::remove(m_store);
    QVERIFY(createStore(m_store));
}

QString FeaturesPackPublishTest::freshDirectory(const QString &name) const
{
    const QString path = m_temp.filePath(name);
    QDir().mkpath(path);
    return path;
}

void FeaturesPackPublishTest::exportPublishesCompletePack()
{
    const QString directory = freshDirectory(QStringLiteral("publish"));
    const QString path = QDir(directory).filePath(QStringLiteral("scalars.ndjson"));
    const ExportResult result = exportPack(path, m_store, false);
    QVERIFY2(result.published, qPrintable(result.message));
    const QByteArray bytes = contents(path);
    QCOMPARE(bytes.count('\n'), qsizetype{2});
    QVERIFY(bytes.contains(QByteArray(64, 'a')));
    QCOMPARE(permissionBits(path), referencePermissionBits(freshDirectory(QStringLiteral("reference"))));
    QCOMPARE(entries(directory), QStringList{QStringLiteral("scalars.ndjson")});
}

void FeaturesPackPublishTest::exportRefusesExistingFile()
{
    const QString directory = freshDirectory(QStringLiteral("existing"));
    const QString path = QDir(directory).filePath(QStringLiteral("scalars.ndjson"));
    writeFile(path, foreignContent);
    const ExportResult result = exportPack(path, m_store, false);
    QVERIFY(!result.published);
    QCOMPARE(result.code, 2);
    QCOMPARE(result.message, packRefusal);
    QCOMPARE(contents(path), foreignContent);
}

void FeaturesPackPublishTest::exportRefusesExistingSymlink()
{
    const QString directory = freshDirectory(QStringLiteral("symlink"));
    const QString path = QDir(directory).filePath(QStringLiteral("scalars.ndjson"));
    const QString target = QDir(directory).filePath(QStringLiteral("target.ndjson"));
    writeFile(target, foreignContent);
    makeSymlink(target, path);
    const ExportResult result = exportPack(path, m_store, false);
    QVERIFY(!result.published);
    QCOMPARE(result.code, 2);
    QCOMPARE(result.message, packRefusal);
    QVERIFY(QFileInfo(path).isSymLink());
    QCOMPARE(contents(target), foreignContent);
}

void FeaturesPackPublishTest::exportRefusesDestinationAppearingBeforePublication()
{
    const QString directory = freshDirectory(QStringLiteral("late-file"));
    const QString path = QDir(directory).filePath(QStringLiteral("scalars.ndjson"));
    SeamReset reset;
    FilePublish::setBeforePublishHookForTesting([path] { writeFile(path, foreignContent); });
    const ExportResult result = exportPack(path, m_store, false);
    QVERIFY(!result.published);
    QCOMPARE(result.code, 2);
    QCOMPARE(result.message, packRefusal);
    QCOMPARE(contents(path), foreignContent);
    QCOMPARE(entries(directory), QStringList{QStringLiteral("scalars.ndjson")});
}

void FeaturesPackPublishTest::exportRefusesSymlinkAppearingBeforePublication()
{
    const QString directory = freshDirectory(QStringLiteral("late-symlink"));
    const QString path = QDir(directory).filePath(QStringLiteral("scalars.ndjson"));
    const QString target = QDir(directory).filePath(QStringLiteral("target.ndjson"));
    writeFile(target, foreignContent);
    SeamReset reset;
    FilePublish::setBeforePublishHookForTesting([path, target] { makeSymlink(target, path); });
    const ExportResult result = exportPack(path, m_store, false);
    QVERIFY(!result.published);
    QCOMPARE(result.code, 2);
    QCOMPARE(result.message, packRefusal);
    QVERIFY(QFileInfo(path).isSymLink());
    QCOMPARE(contents(target), foreignContent);
    QCOMPARE(entries(directory), (QStringList{QStringLiteral("scalars.ndjson"), QStringLiteral("target.ndjson")}));
}

void FeaturesPackPublishTest::exportRefusesDestinationWithoutHardLinks()
{
    const QString directory = freshDirectory(QStringLiteral("late-fallback"));
    const QString path = QDir(directory).filePath(QStringLiteral("scalars.ndjson"));
    SeamReset reset;
    FilePublish::setLinkFunctionForTesting(linkWithoutHardLinks);
    FilePublish::setBeforePublishHookForTesting([path] { writeFile(path, foreignContent); });
    const ExportResult result = exportPack(path, m_store, false);
    QVERIFY(!result.published);
    QCOMPARE(result.code, 2);
    QCOMPARE(result.message, packRefusal);
    QCOMPARE(contents(path), foreignContent);
    QCOMPARE(entries(directory), QStringList{QStringLiteral("scalars.ndjson")});
}

void FeaturesPackPublishTest::exportReplaceIsExplicitAfterDestinationAppeared()
{
    const QString referenceDirectory = freshDirectory(QStringLiteral("reference-export"));
    const QString referencePath = QDir(referenceDirectory).filePath(QStringLiteral("scalars.ndjson"));
    QVERIFY(exportPack(referencePath, m_store, false).published);

    const QString directory = freshDirectory(QStringLiteral("late-replace"));
    const QString path = QDir(directory).filePath(QStringLiteral("scalars.ndjson"));
    SeamReset reset;
    FilePublish::setBeforePublishHookForTesting([path] { writeFile(path, foreignContent); });
    const ExportResult result = exportPack(path, m_store, true);
    QVERIFY2(result.published, qPrintable(result.message));
    QCOMPARE(contents(path), contents(referencePath));
    QCOMPARE(entries(directory), QStringList{QStringLiteral("scalars.ndjson")});
}

QTEST_MAIN(FeaturesPackPublishTest)
#include "test_features_pack_publish.moc"
