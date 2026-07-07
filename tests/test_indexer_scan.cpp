#include "indexer/Dsp.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>

#include <cmath>
#include <limits>
#include <optional>

class IndexerScanTest final : public QObject {
    Q_OBJECT

private slots:
    void generatedFixtureMatrixWritesSchemaV3Features();
};

namespace {

QString muzaitenIndexPath()
{
    QDir buildDir(QCoreApplication::applicationDirPath());
    buildDir.cdUp();
    return buildDir.filePath(QStringLiteral("muzaiten-index"));
}

void requireTool(const QString &name)
{
    if (QStandardPaths::findExecutable(name).isEmpty()) {
        QSKIP(qPrintable(QStringLiteral("%1 is not installed").arg(name)));
    }
}

bool runProcess(const QString &program, const QStringList &arguments, QByteArray *stdoutBytes,
                QByteArray *stderrBytes, QString *error)
{
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(5000)) {
        if (error != nullptr) {
            *error = QStringLiteral("could not start %1").arg(program);
        }
        return false;
    }
    if (!process.waitForFinished(10 * 60 * 1000)) {
        process.kill();
        process.waitForFinished(1000);
        if (error != nullptr) {
            *error = QStringLiteral("%1 timed out").arg(program);
        }
        return false;
    }
    if (stdoutBytes != nullptr) {
        *stdoutBytes = process.readAllStandardOutput();
    }
    if (stderrBytes != nullptr) {
        *stderrBytes = process.readAllStandardError();
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 exited %2: %3")
                         .arg(program)
                         .arg(process.exitCode())
                         .arg(QString::fromUtf8(process.readAllStandardError()).trimmed());
        }
        return false;
    }
    return true;
}

void ffmpeg(const QStringList &arguments)
{
    QString error;
    QByteArray stderrBytes;
    QStringList args{
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"),
        QStringLiteral("error"),
        QStringLiteral("-y"),
    };
    args.append(arguments);
    QVERIFY2(runProcess(QStringLiteral("ffmpeg"), args, nullptr, &stderrBytes, &error),
             qPrintable(error.isEmpty() ? QString::fromUtf8(stderrBytes) : error));
}

QJsonObject runIndexer(const QStringList &arguments)
{
    QByteArray stdoutBytes;
    QByteArray stderrBytes;
    QString error;
    if (!runProcess(muzaitenIndexPath(), arguments, &stdoutBytes, &stderrBytes, &error)) {
        const QByteArray message = (error.isEmpty() ? QString::fromUtf8(stderrBytes) : error).toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(stdoutBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        const QByteArray message = QStringLiteral("invalid indexer JSON: %1\n%2")
                                       .arg(parseError.errorString(), QString::fromUtf8(stdoutBytes))
                                       .toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        return {};
    }
    return document.object();
}

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

QPair<qint64, qint64> fileStats(const QString &path)
{
    const QFileInfo info(path);
    return {info.lastModified().toSecsSinceEpoch(), info.size()};
}

void createLibrary(const QString &path, const QStringList &files)
{
    const QString connectionName =
        QStringLiteral("indexer-scan-library-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QString error;
        QVERIFY2(execSql(query, QStringLiteral(
                                   "CREATE TABLE tracks("
                                   " path TEXT PRIMARY KEY,"
                                   " file_mtime INTEGER NOT NULL,"
                                   " file_size INTEGER NOT NULL,"
                                   " missing INTEGER NOT NULL DEFAULT 0)"),
                         &error),
                 qPrintable(error));

        for (const QString &file : files) {
            const QPair<qint64, qint64> stats = fileStats(file);
            QSqlQuery insert(db);
            insert.prepare(QStringLiteral(
                "INSERT INTO tracks(path, file_mtime, file_size, missing) VALUES(?, ?, ?, 0)"));
            insert.addBindValue(file);
            insert.addBindValue(stats.first);
            insert.addBindValue(stats.second);
            QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
}

QSqlDatabase openReadOnly(const QString &path, QString *connectionName)
{
    *connectionName = QStringLiteral("indexer-scan-read-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), *connectionName);
    db.setDatabaseName(path);
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    return db;
}

qint64 groupFor(QSqlDatabase &db, const QString &path)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT content_group_id FROM files WHERE path = ?"));
    query.addBindValue(path);
    if (!query.exec() || !query.next()) {
        const QByteArray message = QStringLiteral("missing content group for %1: %2")
                                       .arg(path, query.lastError().text())
                                       .toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        return -1;
    }
    return query.value(0).toLongLong();
}

QString decodeHashFor(QSqlDatabase &db, const QString &path)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT decode_hash FROM files WHERE path = ?"));
    query.addBindValue(path);
    if (!query.exec() || !query.next()) {
        const QByteArray message = QStringLiteral("missing decode hash for %1: %2")
                                       .arg(path, query.lastError().text())
                                       .toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        return {};
    }
    return query.value(0).toString();
}

std::optional<double> optionalFeature(QSqlDatabase &db, qint64 groupId, const QString &column)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT %1 FROM features WHERE content_group_id = ?").arg(column));
    query.addBindValue(groupId);
    if (!query.exec() || !query.next()) {
        const QByteArray message = QStringLiteral("missing feature %1 for group %2: %3")
                                       .arg(column)
                                       .arg(groupId)
                                       .arg(query.lastError().text())
                                       .toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        return std::nullopt;
    }
    if (query.value(0).isNull()) {
        return std::nullopt;
    }
    return query.value(0).toDouble();
}

QStringList featureRows(QSqlDatabase &db)
{
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral(
            "SELECT content_group_id, tempo_bpm, loudness_lufs, onset_rate_hz, energy, version "
            "FROM features ORDER BY content_group_id"))) {
        const QByteArray message = query.lastError().text().toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        return {};
    }
    QStringList rows;
    while (query.next()) {
        QStringList columns;
        for (int index = 0; index < 6; ++index) {
            columns << (query.value(index).isNull() ? QStringLiteral("NULL") : query.value(index).toString());
        }
        rows << columns.join(QLatin1Char('|'));
    }
    return rows;
}

double ffmpegIntegratedLufs(const QString &path)
{
    QByteArray stdoutBytes;
    QByteArray stderrBytes;
    QString error;
    const QStringList args{
        QStringLiteral("-hide_banner"),
        QStringLiteral("-nostats"),
        QStringLiteral("-i"),
        path,
        QStringLiteral("-vn"),
        QStringLiteral("-af"),
        QStringLiteral("aresample=22050,ebur128"),
        QStringLiteral("-f"),
        QStringLiteral("null"),
        QStringLiteral("-"),
    };
    if (!runProcess(QStringLiteral("ffmpeg"), args, &stdoutBytes, &stderrBytes, &error)) {
        const QByteArray message = (error.isEmpty() ? QString::fromUtf8(stderrBytes) : error).toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        return std::numeric_limits<double>::quiet_NaN();
    }
    Q_UNUSED(stdoutBytes);

    const QString log = QString::fromUtf8(stderrBytes);
    const QRegularExpression integrated(QStringLiteral("\\bI:\\s*(-?\\d+(?:\\.\\d+)?)\\s+LUFS"));
    QRegularExpressionMatchIterator it = integrated.globalMatch(log);
    std::optional<double> last;
    while (it.hasNext()) {
        last = it.next().captured(1).toDouble();
    }
    if (!last.has_value()) {
        const QByteArray message = QStringLiteral("could not parse ebur128 output:\n%1").arg(log).toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        return std::numeric_limits<double>::quiet_NaN();
    }
    return *last;
}

} // namespace

void IndexerScanTest::generatedFixtureMatrixWritesSchemaV3Features()
{
    requireTool(QStringLiteral("ffmpeg"));
    requireTool(QStringLiteral("fpcalc"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir audioDir(dir.filePath(QStringLiteral("audio")));
    QVERIFY(QDir().mkpath(audioDir.absolutePath()));

    const QString sineFlac = audioDir.filePath(QStringLiteral("sine.flac"));
    const QString sineWav = audioDir.filePath(QStringLiteral("sine.wav"));
    const QString sineMp3 = audioDir.filePath(QStringLiteral("sine.mp3"));
    const QString padded = audioDir.filePath(QStringLiteral("padded.flac"));
    const QString click120 = audioDir.filePath(QStringLiteral("click-120.wav"));
    const QString click90 = audioDir.filePath(QStringLiteral("click-90.wav"));
    const QString pink = audioDir.filePath(QStringLiteral("pink.wav"));
    const QString silence = audioDir.filePath(QStringLiteral("near-silence.wav"));

    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("sine=frequency=440:duration=30:sample_rate=44100"), sineFlac});
    ffmpeg({QStringLiteral("-i"), sineFlac, sineWav});
    ffmpeg({QStringLiteral("-i"), sineFlac, QStringLiteral("-b:a"), QStringLiteral("128k"), sineMp3});
    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("sine=frequency=440:duration=33:sample_rate=44100"), padded});
    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("aevalsrc=(lt(mod(t\\,0.5)\\,0.002))*0.9:duration=30:sample_rate=44100"),
            click120});
    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("aevalsrc=(lt(mod(t\\,0.6666667)\\,0.002))*0.9:duration=33:sample_rate=44100"),
            click90});
    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("anoisesrc=color=pink:duration=12:amplitude=0.2:sample_rate=44100"), pink});
    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("sine=frequency=440:duration=12:sample_rate=44100"),
            QStringLiteral("-af"), QStringLiteral("volume=0.0001"), silence});

    const QString library = dir.filePath(QStringLiteral("library.sqlite"));
    const QString features = dir.filePath(QStringLiteral("features.sqlite"));
    createLibrary(library, {sineFlac, sineWav, sineMp3, padded, click120, click90, pink, silence});

    const QJsonObject first = runIndexer({
        QStringLiteral("scan"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--features"),
        features,
        QStringLiteral("--jobs"),
        QStringLiteral("2"),
        QStringLiteral("--json"),
    });
    QCOMPARE(first.value(QStringLiteral("schema_version")).toInt(), 3);
    QCOMPARE(first.value(QStringLiteral("scanned")).toInt(), 8);
    QCOMPARE(first.value(QStringLiteral("skipped")).toInt(), 0);
    QCOMPARE(first.value(QStringLiteral("failed")).toInt(), 0);
    QCOMPARE(first.value(QStringLiteral("dsp_version")).toString(), QString::fromLatin1(Dsp::kDspVersion));
    QVERIFY(first.value(QStringLiteral("featured_groups")).toInt() >= 6);

    QString connectionName;
    QSqlDatabase db = openReadOnly(features, &connectionName);
    QVERIFY(db.open());

    const qint64 sineGroup = groupFor(db, sineFlac);
    QCOMPARE(groupFor(db, sineWav), sineGroup);
    QCOMPARE(groupFor(db, sineMp3), sineGroup);
    QVERIFY(groupFor(db, padded) != sineGroup);
    QCOMPARE(decodeHashFor(db, sineFlac), decodeHashFor(db, sineWav));

    const qint64 click120Group = groupFor(db, click120);
    const qint64 click90Group = groupFor(db, click90);
    const std::optional<double> tempo120 = optionalFeature(db, click120Group, QStringLiteral("tempo_bpm"));
    const std::optional<double> tempo90 = optionalFeature(db, click90Group, QStringLiteral("tempo_bpm"));
    QVERIFY(tempo120.has_value());
    QVERIFY(tempo90.has_value());
    QVERIFY2(std::abs(*tempo120 - 120.0) <= 3.0,
             qPrintable(QStringLiteral("120 BPM fixture estimated %1").arg(*tempo120)));
    QVERIFY2(std::abs(*tempo90 - 90.0) <= 3.0,
             qPrintable(QStringLiteral("90 BPM fixture estimated %1").arg(*tempo90)));

    const std::optional<double> onset120 = optionalFeature(db, click120Group, QStringLiteral("onset_rate_hz"));
    QVERIFY(onset120.has_value());
    QVERIFY2(std::abs(*onset120 - 2.0) <= 0.4,
             qPrintable(QStringLiteral("120 BPM onset rate estimated %1").arg(*onset120)));

    const qint64 pinkGroup = groupFor(db, pink);
    const std::optional<double> indexedPinkLufs = optionalFeature(db, pinkGroup, QStringLiteral("loudness_lufs"));
    QVERIFY(indexedPinkLufs.has_value());
    const double measuredPinkLufs = ffmpegIntegratedLufs(pink);
    QVERIFY2(std::abs(*indexedPinkLufs - measuredPinkLufs) <= 0.5,
             qPrintable(QStringLiteral("indexed %1 LUFS, ffmpeg ebur128 %2 LUFS")
                            .arg(*indexedPinkLufs)
                            .arg(measuredPinkLufs)));

    const qint64 silenceGroup = groupFor(db, silence);
    QVERIFY(!optionalFeature(db, silenceGroup, QStringLiteral("tempo_bpm")).has_value());
    QVERIFY(!optionalFeature(db, silenceGroup, QStringLiteral("loudness_lufs")).has_value());
    QVERIFY(!optionalFeature(db, silenceGroup, QStringLiteral("energy")).has_value());

    const QStringList beforeRows = featureRows(db);
    db.close();
    QSqlDatabase::removeDatabase(connectionName);

    const QJsonObject second = runIndexer({
        QStringLiteral("scan"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--features"),
        features,
        QStringLiteral("--jobs"),
        QStringLiteral("2"),
        QStringLiteral("--json"),
    });
    QCOMPARE(second.value(QStringLiteral("scanned")).toInt(), 0);
    QCOMPARE(second.value(QStringLiteral("skipped")).toInt(), 8);

    connectionName.clear();
    db = openReadOnly(features, &connectionName);
    QVERIFY(db.open());
    QCOMPARE(featureRows(db), beforeRows);
    db.close();
    QSqlDatabase::removeDatabase(connectionName);

    const QJsonObject status = runIndexer({
        QStringLiteral("status"),
        QStringLiteral("--features"),
        features,
        QStringLiteral("--json"),
    });
    QCOMPARE(status.value(QStringLiteral("schema_version")).toInt(), 3);
    QCOMPARE(status.value(QStringLiteral("dsp_version")).toString(), QString::fromLatin1(Dsp::kDspVersion));
    QCOMPARE(status.value(QStringLiteral("files")).toInt(), 8);
    QCOMPARE(status.value(QStringLiteral("statuses")).toObject().value(QStringLiteral("ok")).toInt(), 8);
    QCOMPARE(status.value(QStringLiteral("featured_groups")).toInt(),
             first.value(QStringLiteral("featured_groups")).toInt());
}

QTEST_MAIN(IndexerScanTest)
#include "test_indexer_scan.moc"
