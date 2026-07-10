#include "indexer/Dsp.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
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
#include <QVector>
#include <QtEndian>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <thread>

class IndexerScanTest final : public QObject {
    Q_OBJECT

private slots:
    void generatedFixtureMatrixWritesSchemaV4Features();
    void groupIdsStayStableWhenLibraryGrows();
    void incrementalRegroupSplitsAndMergesAffectedGroup();
    void powerOptionsReportEffectiveJobs();
    void cancelPersistsCompletedRowsAndRerunSkipsThem();
    void incrementalRescanPreservesFeatureRows();
    void featurePhaseProgressAndStaleDenom();
    void featurePhaseCancelPreservesWrittenRows();
    void schemaV3StoreUpgradesInPlaceKeepingFeatureRows();
    void forcedRefreshCopiesWithoutTouchingAudio();
    void orphanedFileFeatureRowsAreSwept();
};

namespace {

constexpr double kFingerprintBerGate = 0.05;
constexpr int kChromaprintOffsetFrames = 3;

QString muzaitenIndexPath()
{
    QDir buildDir(QCoreApplication::applicationDirPath());
    buildDir.cdUp();
    return buildDir.filePath(QStringLiteral("muzaiten-features"));
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

QJsonObject runIndexer(const QStringList &arguments, QByteArray *stderrBytes = nullptr)
{
    QByteArray stdoutBytes;
    QByteArray capturedStderr;
    QString error;
    if (!runProcess(muzaitenIndexPath(), arguments, &stdoutBytes, &capturedStderr, &error)) {
        const QByteArray message = (error.isEmpty() ? QString::fromUtf8(capturedStderr) : error).toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        return {};
    }
    if (stderrBytes != nullptr) {
        *stderrBytes = capturedStderr;
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

QJsonObject parseJsonObject(const QByteArray &bytes)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        const QByteArray message = QStringLiteral("invalid JSON: %1\n%2")
                                       .arg(parseError.errorString(), QString::fromUtf8(bytes))
                                       .toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        return {};
    }
    return document.object();
}

QByteArray encodeFingerprint(const QJsonArray &fingerprint)
{
    QByteArray blob;
    blob.resize(static_cast<qsizetype>(fingerprint.size() * static_cast<qsizetype>(sizeof(qint32))));
    auto *data = reinterpret_cast<uchar *>(blob.data());
    for (qsizetype index = 0; index < fingerprint.size(); ++index) {
        const auto raw = static_cast<quint32>(fingerprint.at(index).toDouble());
        qToLittleEndian<qint32>(static_cast<qint32>(raw),
                                data + index * static_cast<qsizetype>(sizeof(qint32)));
    }
    return blob;
}

QByteArray fpcalcFingerprint(const QString &path)
{
    QByteArray stdoutBytes;
    QByteArray stderrBytes;
    QString error;
    const QStringList args{
        QStringLiteral("-raw"),
        QStringLiteral("-json"),
        QStringLiteral("-length"),
        QStringLiteral("120"),
        path,
    };
    if (!runProcess(QStringLiteral("fpcalc"), args, &stdoutBytes, &stderrBytes, &error)) {
        const QByteArray message = (error.isEmpty() ? QString::fromUtf8(stderrBytes) : error).toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        return {};
    }
    const QJsonObject object = parseJsonObject(stdoutBytes);
    return encodeFingerprint(object.value(QStringLiteral("fingerprint")).toArray());
}

std::vector<qint32> decodeFingerprint(const QByteArray &blob)
{
    std::vector<qint32> values;
    if (blob.size() % static_cast<qsizetype>(sizeof(qint32)) != 0) {
        return values;
    }
    const qsizetype count = blob.size() / static_cast<qsizetype>(sizeof(qint32));
    values.reserve(static_cast<std::size_t>(count));
    const auto *data = reinterpret_cast<const uchar *>(blob.constData());
    for (qsizetype index = 0; index < count; ++index) {
        values.push_back(qFromLittleEndian<qint32>(
            data + index * static_cast<qsizetype>(sizeof(qint32))));
    }
    return values;
}

double bitErrorRate(const std::vector<qint32> &left, const std::vector<qint32> &right, int offset)
{
    const std::size_t leftStart = offset >= 0 ? static_cast<std::size_t>(offset) : 0;
    const std::size_t rightStart = offset < 0 ? static_cast<std::size_t>(-offset) : 0;
    if (leftStart >= left.size() || rightStart >= right.size()) {
        return std::numeric_limits<double>::infinity();
    }
    const std::size_t overlap = std::min(left.size() - leftStart, right.size() - rightStart);
    if (overlap == 0) {
        return std::numeric_limits<double>::infinity();
    }
    quint64 errors = 0;
    for (std::size_t index = 0; index < overlap; ++index) {
        const std::uint32_t diff = static_cast<std::uint32_t>(
            left[leftStart + index] ^ right[rightStart + index]);
        errors += std::popcount(diff);
    }
    return static_cast<double>(errors) / (static_cast<double>(overlap) * 32.0);
}

double bestBitErrorRate(const QByteArray &leftBlob, const QByteArray &rightBlob)
{
    const std::vector<qint32> left = decodeFingerprint(leftBlob);
    const std::vector<qint32> right = decodeFingerprint(rightBlob);
    double best = std::numeric_limits<double>::infinity();
    for (int offset = -kChromaprintOffsetFrames; offset <= kChromaprintOffsetFrames; ++offset) {
        best = std::min(best, bitErrorRate(left, right, offset));
    }
    return best;
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

QByteArray chromaprintFor(QSqlDatabase &db, const QString &path)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT chromaprint_fp FROM files WHERE path = ?"));
    query.addBindValue(path);
    if (!query.exec() || !query.next()) {
        const QByteArray message = QStringLiteral("missing chromaprint for %1: %2")
                                       .arg(path, query.lastError().text())
                                       .toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        return {};
    }
    return query.value(0).toByteArray();
}

void replaceStoredFingerprint(QSqlDatabase &db, const QString &path, const QByteArray &fingerprint)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE files SET chromaprint_fp = ? WHERE path = ?"));
    query.addBindValue(fingerprint);
    query.addBindValue(path);
    QVERIFY2(query.exec(), qPrintable(query.lastError().text()));
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

int fileRowCount(const QString &featuresPath)
{
    QString connectionName;
    QSqlDatabase db = openReadOnly(featuresPath, &connectionName);
    if (!db.open()) {
        const QByteArray message = db.lastError().text().toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        QSqlDatabase::removeDatabase(connectionName);
        return 0;
    }
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM files")) || !query.next()) {
        const QByteArray message = query.lastError().text().toUtf8();
        QTest::qFail(message.constData(), __FILE__, __LINE__);
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
        return 0;
    }
    const int count = query.value(0).toInt();
    db.close();
    QSqlDatabase::removeDatabase(connectionName);
    return count;
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

void IndexerScanTest::generatedFixtureMatrixWritesSchemaV4Features()
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

    QByteArray progressStderr;
    const QJsonObject first = runIndexer({
        QStringLiteral("scan"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--features"),
        features,
        QStringLiteral("--jobs"),
        QStringLiteral("2"),
        QStringLiteral("--json"),
        QStringLiteral("--progress"),
    }, &progressStderr);
    const QString progressLog = QString::fromUtf8(progressStderr);
    QVERIFY2(progressLog.contains(QRegularExpression(
                 QStringLiteral("^progress \\d+/\\d+ elapsed=[0-9.]+ rate=([0-9.]+|-) eta=(\\d+|-)$"),
                 QRegularExpression::MultilineOption)),
             qPrintable(progressLog));
    const qsizetype groupingIndex = progressLog.indexOf(QStringLiteral("phase grouping"));
    const qsizetype featuresIndex = progressLog.indexOf(QStringLiteral("phase features"));
    QVERIFY2(groupingIndex >= 0, qPrintable(progressLog));
    QVERIFY2(featuresIndex > groupingIndex, qPrintable(progressLog));
    const QString afterFeatures = progressLog.mid(featuresIndex);
    QVERIFY2(afterFeatures.contains(QRegularExpression(
                 QStringLiteral("^progress 0/\\d+ elapsed=[0-9.]+ rate=- eta=-$"),
                 QRegularExpression::MultilineOption)),
             qPrintable(afterFeatures));
    QVERIFY(first.contains(QStringLiteral("feature_groups_processed")));
    QVERIFY(first.contains(QStringLiteral("features_written")));
    QVERIFY(first.contains(QStringLiteral("feature_groups_failed")));
    QVERIFY(first.value(QStringLiteral("features_written")).toInt() >= 6);
    QCOMPARE(first.value(QStringLiteral("feature_groups_failed")).toInt(), 0);
    QCOMPARE(first.value(QStringLiteral("schema_version")).toInt(), 4);
    QCOMPARE(first.value(QStringLiteral("scanned")).toInt(), 8);
    QCOMPARE(first.value(QStringLiteral("skipped")).toInt(), 0);
    QCOMPARE(first.value(QStringLiteral("failed")).toInt(), 0);
    QCOMPARE(first.value(QStringLiteral("dsp_version")).toString(), QString::fromLatin1(Dsp::kDspVersion));
    QVERIFY(first.contains(QStringLiteral("elapsed_secs")));
    const QJsonObject timings = first.value(QStringLiteral("timings")).toObject();
    for (const QString &stage : {QStringLiteral("decode"), QStringLiteral("hash"), QStringLiteral("dsp"), QStringLiteral("fp")}) {
        const QJsonObject stats = timings.value(stage).toObject();
        QVERIFY2(stats.contains(QStringLiteral("total_ms")), qPrintable(stage));
        QVERIFY2(stats.contains(QStringLiteral("mean_ms")), qPrintable(stage));
        QVERIFY2(stats.contains(QStringLiteral("p50_ms")), qPrintable(stage));
        QVERIFY2(stats.contains(QStringLiteral("p95_ms")), qPrintable(stage));
    }
    QVERIFY(first.value(QStringLiteral("featured_groups")).toInt() >= 6);
    QCOMPARE(first.value(QStringLiteral("featured_fresh")).toInt(),
             first.value(QStringLiteral("featured_groups")).toInt());
    QCOMPARE(first.value(QStringLiteral("featured_stale")).toInt(), 0);

    QString connectionName;
    QSqlDatabase db = openReadOnly(features, &connectionName);
    QVERIFY(db.open());

    // PF-1 contract: every successful file analysis persists its scalar
    // result immediately, not only whichever paths later become group
    // representatives. Optional scalar fields remain SQL NULL and the row
    // itself is still a fresh, final result.
    {
        QSqlQuery fileFeatureCount(db);
        fileFeatureCount.prepare(QStringLiteral(
            "SELECT COUNT(*), SUM(version = ?) FROM file_features"));
        fileFeatureCount.addBindValue(QString::fromLatin1(Dsp::kDspVersion));
        QVERIFY(fileFeatureCount.exec());
        QVERIFY(fileFeatureCount.next());
        QCOMPARE(fileFeatureCount.value(0).toInt(), 8);
        QCOMPARE(fileFeatureCount.value(1).toInt(), 8);

        QSqlQuery silentFileFeatures(db);
        silentFileFeatures.prepare(QStringLiteral(
            "SELECT tempo_bpm, loudness_lufs, energy, version "
            "FROM file_features WHERE path = ?"));
        silentFileFeatures.addBindValue(silence);
        QVERIFY(silentFileFeatures.exec());
        QVERIFY(silentFileFeatures.next());
        QVERIFY(silentFileFeatures.value(0).isNull());
        QVERIFY(silentFileFeatures.value(1).isNull());
        QVERIFY(silentFileFeatures.value(2).isNull());
        QCOMPARE(silentFileFeatures.value(3).toString(), QString::fromLatin1(Dsp::kDspVersion));
    }

    for (const QString &file : {sineFlac, sineWav, sineMp3, padded, click120, click90, pink, silence}) {
        const QByteArray indexed = chromaprintFor(db, file);
        const QByteArray fpcalc = fpcalcFingerprint(file);
        const auto indexedValues = decodeFingerprint(indexed);
        const auto fpcalcValues = decodeFingerprint(fpcalc);
        QVERIFY2(!indexedValues.empty(), qPrintable(file));
        QVERIFY2(!fpcalcValues.empty(), qPrintable(file));
        const double lengthRatio = static_cast<double>(indexedValues.size()) / static_cast<double>(fpcalcValues.size());
        QVERIFY2(lengthRatio >= 0.9 && lengthRatio <= 1.1,
                 qPrintable(QStringLiteral("%1 fingerprint length ratio %2").arg(file).arg(lengthRatio)));
        const double ber = bestBitErrorRate(indexed, fpcalc);
        QVERIFY2(ber < kFingerprintBerGate,
                 qPrintable(QStringLiteral("%1 BER %2").arg(file).arg(ber)));
    }

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
    QCOMPARE(status.value(QStringLiteral("schema_version")).toInt(), 4);
    QCOMPARE(status.value(QStringLiteral("dsp_version")).toString(), QString::fromLatin1(Dsp::kDspVersion));
    QCOMPARE(status.value(QStringLiteral("files")).toInt(), 8);
    QCOMPARE(status.value(QStringLiteral("statuses")).toObject().value(QStringLiteral("ok")).toInt(), 8);
    QCOMPARE(status.value(QStringLiteral("featured_groups")).toInt(),
             first.value(QStringLiteral("featured_groups")).toInt());
    QCOMPARE(status.value(QStringLiteral("featured_fresh")).toInt(),
             status.value(QStringLiteral("featured_groups")).toInt());
    QCOMPARE(status.value(QStringLiteral("featured_stale")).toInt(), 0);
}

void IndexerScanTest::groupIdsStayStableWhenLibraryGrows()
{
    requireTool(QStringLiteral("ffmpeg"));
    requireTool(QStringLiteral("fpcalc"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir audioDir(dir.filePath(QStringLiteral("audio")));
    QVERIFY(QDir().mkpath(audioDir.absolutePath()));

    // The file added later sorts alphabetically FIRST: under renumber-style
    // regrouping it would claim id 1 and shift every existing group.
    const QString tone = audioDir.filePath(QStringLiteral("m-tone.flac"));
    const QString click = audioDir.filePath(QStringLiteral("z-click.wav"));
    const QString added = audioDir.filePath(QStringLiteral("aaa-noise.wav"));

    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("sine=frequency=550:duration=12:sample_rate=44100"), tone});
    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("aevalsrc=(lt(mod(t\\,0.5)\\,0.002))*0.9:duration=15:sample_rate=44100"),
            click});

    const QString library = dir.filePath(QStringLiteral("library.sqlite"));
    const QString features = dir.filePath(QStringLiteral("features.sqlite"));
    createLibrary(library, {tone, click});

    const QStringList scanArgs{
        QStringLiteral("scan"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--features"),
        features,
        QStringLiteral("--json"),
    };
    runIndexer(scanArgs);

    {
        const QString connectionName = QStringLiteral("indexer-scan-fpcalc-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(features);
            QVERIFY(db.open());
            replaceStoredFingerprint(db, tone, fpcalcFingerprint(tone));
            replaceStoredFingerprint(db, click, fpcalcFingerprint(click));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }

    qint64 toneGroup = -1;
    qint64 clickGroup = -1;
    QStringList toneFeatureBefore;
    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        toneGroup = groupFor(db, tone);
        clickGroup = groupFor(db, click);
        QVERIFY(toneGroup != clickGroup);
        for (const QString &row : featureRows(db)) {
            if (row.startsWith(QStringLiteral("%1|").arg(toneGroup))) {
                toneFeatureBefore << row;
            }
        }
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }
    QCOMPARE(toneFeatureBefore.size(), 1);

    // Plant embedder-owned rows the way tools/features-clap writes them; a
    // regroup must never orphan or misassociate these while their content
    // still exists.
    {
        const QString connectionName = QStringLiteral("indexer-scan-embed-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(features);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QString error;
            QVERIFY2(execSql(query, QStringLiteral(
                                       "CREATE TABLE IF NOT EXISTS embeddings("
                                       " content_group_id INTEGER PRIMARY KEY,"
                                       " model TEXT NOT NULL, version TEXT NOT NULL,"
                                       " dim INTEGER NOT NULL, vector BLOB NOT NULL)"),
                             &error),
                     qPrintable(error));
            QVERIFY2(execSql(query, QStringLiteral(
                                       "CREATE TABLE IF NOT EXISTS track_neighbors("
                                       " content_group_id INTEGER NOT NULL,"
                                       " neighbor_group_id INTEGER NOT NULL,"
                                       " rank INTEGER NOT NULL, cosine REAL NOT NULL,"
                                       " PRIMARY KEY(content_group_id, rank))"),
                             &error),
                     qPrintable(error));
            QSqlQuery insert(db);
            insert.prepare(QStringLiteral(
                "INSERT INTO embeddings(content_group_id, model, version, dim, vector)"
                " VALUES(?, 'test-model', 'v1', 2, x'0000803f00000000')"));
            insert.addBindValue(toneGroup);
            QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));
            QSqlQuery neighbor(db);
            neighbor.prepare(QStringLiteral(
                "INSERT INTO track_neighbors(content_group_id, neighbor_group_id, rank, cosine)"
                " VALUES(?, ?, 1, 0.5)"));
            neighbor.addBindValue(toneGroup);
            neighbor.addBindValue(clickGroup);
            QVERIFY2(neighbor.exec(), qPrintable(neighbor.lastError().text()));
            // A one-file addition must not rebuild/delete stable components.
            // The full-regroup path deletes every group before reinserting it;
            // this trigger makes that accidental fallback a hard failure.
            QVERIFY2(execSql(query, QStringLiteral(
                "CREATE TRIGGER preserve_existing_groups BEFORE DELETE ON content_groups "
                "WHEN OLD.id IN (%1, %2) "
                "BEGIN SELECT RAISE(ABORT, 'stable group was rebuilt'); END")
                .arg(toneGroup)
                .arg(clickGroup), &error), qPrintable(error));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }

    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("anoisesrc=color=pink:duration=12:amplitude=0.2:sample_rate=44100"),
            added});
    {
        const QString connectionName = QStringLiteral("indexer-scan-grow-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(library);
            QVERIFY(db.open());
            const QPair<qint64, qint64> stats = fileStats(added);
            QSqlQuery insert(db);
            insert.prepare(QStringLiteral(
                "INSERT INTO tracks(path, file_mtime, file_size, missing) VALUES(?, ?, ?, 0)"));
            insert.addBindValue(added);
            insert.addBindValue(stats.first);
            insert.addBindValue(stats.second);
            QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }

    const QJsonObject grown = runIndexer(scanArgs);
    QCOMPARE(grown.value(QStringLiteral("scanned")).toInt(), 1);
    QCOMPARE(grown.value(QStringLiteral("skipped")).toInt(), 2);

    QString connectionName;
    QSqlDatabase db = openReadOnly(features, &connectionName);
    QVERIFY(db.open());

    QCOMPARE(groupFor(db, tone), toneGroup);
    QCOMPARE(groupFor(db, click), clickGroup);
    const qint64 addedGroup = groupFor(db, added);
    QVERIFY(addedGroup != toneGroup && addedGroup != clickGroup);

    QStringList toneFeatureAfter;
    for (const QString &row : featureRows(db)) {
        if (row.startsWith(QStringLiteral("%1|").arg(toneGroup))) {
            toneFeatureAfter << row;
        }
    }
    QCOMPARE(toneFeatureAfter, toneFeatureBefore);

    QSqlQuery embedding(db);
    embedding.prepare(QStringLiteral("SELECT model FROM embeddings WHERE content_group_id = ?"));
    embedding.addBindValue(toneGroup);
    QVERIFY2(embedding.exec() && embedding.next(), "planted embedding lost its group");
    QCOMPARE(embedding.value(0).toString(), QStringLiteral("test-model"));

    QSqlQuery neighbor(db);
    neighbor.prepare(QStringLiteral(
        "SELECT neighbor_group_id FROM track_neighbors WHERE content_group_id = ?"));
    neighbor.addBindValue(toneGroup);
    QVERIFY2(neighbor.exec() && neighbor.next(), "planted neighbor row lost its group");
    QCOMPARE(neighbor.value(0).toLongLong(), clickGroup);

    db.close();
    QSqlDatabase::removeDatabase(connectionName);
}

void IndexerScanTest::incrementalRegroupSplitsAndMergesAffectedGroup()
{
    requireTool(QStringLiteral("ffmpeg"));
    QVERIFY(QFileInfo::exists(muzaitenIndexPath()));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir audioDir(dir.filePath(QStringLiteral("audio")));
    QVERIFY(QDir().mkpath(audioDir.absolutePath()));
    const QString original = audioDir.filePath(QStringLiteral("a-original.flac"));
    const QString copy = audioDir.filePath(QStringLiteral("b-copy.flac"));
    const QString stable = audioDir.filePath(QStringLiteral("z-stable.flac"));

    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("sine=frequency=440:duration=8:sample_rate=44100"), original});
    QVERIFY(QFile::copy(original, copy));
    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("anoisesrc=color=pink:duration=11:amplitude=0.2:sample_rate=44100:seed=77"),
            stable});

    const QString library = dir.filePath(QStringLiteral("library.sqlite"));
    const QString features = dir.filePath(QStringLiteral("features.sqlite"));
    createLibrary(library, {original, copy, stable});
    const QStringList scanArgs{
        QStringLiteral("scan"), QStringLiteral("--library"), library,
        QStringLiteral("--features"), features, QStringLiteral("--jobs"),
        QStringLiteral("1"), QStringLiteral("--json"),
    };
    runIndexer(scanArgs);

    qint64 originalGroup = -1;
    qint64 stableGroup = -1;
    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        originalGroup = groupFor(db, original);
        QCOMPARE(groupFor(db, copy), originalGroup);
        stableGroup = groupFor(db, stable);
        QVERIFY(stableGroup != originalGroup);
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }

    // Protect the unrelated component: incremental split/merge may delete an
    // affected id that becomes orphaned, but it must never rebuild this one.
    {
        const QString connectionName = QStringLiteral("incremental-trigger-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(features);
            QVERIFY(db.open());
            QSqlQuery trigger(db);
            QVERIFY(trigger.exec(QStringLiteral(
                "CREATE TRIGGER preserve_stable_group BEFORE DELETE ON content_groups "
                "WHEN OLD.id=%1 BEGIN SELECT RAISE(ABORT, 'stable group was rebuilt'); END")
                .arg(stableGroup)));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }

    const auto markChanged = [&](const QString &path) {
        const QString connectionName = QStringLiteral("incremental-library-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(library);
            QVERIFY(db.open());
            QSqlQuery update(db);
            update.prepare(QStringLiteral(
                "UPDATE tracks SET file_mtime=file_mtime+1, file_size=? WHERE path=?"));
            update.addBindValue(QFileInfo(path).size());
            update.addBindValue(path);
            QVERIFY(update.exec());
            QCOMPARE(update.numRowsAffected(), 1);
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    };

    // Changing one member can split a previously transitive component. The
    // untouched member keeps the stable id; the changed path gets a fresh id.
    QVERIFY(QFile::remove(copy));
    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("anoisesrc=color=white:duration=13:amplitude=0.2:sample_rate=44100:seed=91"),
            copy});
    markChanged(copy);
    const QJsonObject split = runIndexer(scanArgs);
    QCOMPARE(split.value(QStringLiteral("scanned")).toInt(), 1);

    qint64 splitGroup = -1;
    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        QCOMPARE(groupFor(db, original), originalGroup);
        splitGroup = groupFor(db, copy);
        QVERIFY(splitGroup != originalGroup);
        QCOMPARE(groupFor(db, stable), stableGroup);
        QCOMPARE(split.value(QStringLiteral("groups")).toInt(), 3);
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }

    // Restoring exact decoded identity merges only the two affected groups;
    // the smaller surviving old id wins and the transient split id is swept.
    QVERIFY(QFile::remove(copy));
    QVERIFY(QFile::copy(original, copy));
    markChanged(copy);
    const QJsonObject merged = runIndexer(scanArgs);
    QCOMPARE(merged.value(QStringLiteral("scanned")).toInt(), 1);
    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        QCOMPARE(groupFor(db, original), originalGroup);
        QCOMPARE(groupFor(db, copy), originalGroup);
        QCOMPARE(groupFor(db, stable), stableGroup);
        QCOMPARE(merged.value(QStringLiteral("groups")).toInt(), 2);
        QSqlQuery transient(db);
        transient.prepare(QStringLiteral("SELECT COUNT(*) FROM content_groups WHERE id=?"));
        transient.addBindValue(splitGroup);
        QVERIFY(transient.exec());
        QVERIFY(transient.next());
        QCOMPARE(transient.value(0).toInt(), 0);
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }
}

void IndexerScanTest::powerOptionsReportEffectiveJobs()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const int cores = static_cast<int>(std::max<std::size_t>(1, std::thread::hardware_concurrency()));
    struct Case {
        QString power;
        QStringList extra;
        int expectedJobs;
    };
    const QVector<Case> cases{
        {QStringLiteral("background"), {}, std::max(1, cores / 4)},
        {QStringLiteral("balanced"), {}, std::max(2, cores / 2)},
        {QStringLiteral("turbo"), {}, cores},
        {QStringLiteral("background"), {QStringLiteral("--jobs"), QStringLiteral("3")}, 3},
    };

    for (const Case &item : cases) {
        QStringList args{
            QStringLiteral("scan"),
            QStringLiteral("--stage"),
            QStringLiteral("features"),
            QStringLiteral("--features"),
            dir.filePath(QStringLiteral("%1.sqlite").arg(item.power + item.extra.join(QString()))),
            QStringLiteral("--power"),
            item.power,
            QStringLiteral("--json"),
        };
        args.append(item.extra);
        const QJsonObject result = runIndexer(args);
        QCOMPARE(result.value(QStringLiteral("power")).toString(), item.power);
        QCOMPARE(result.value(QStringLiteral("jobs")).toInt(), item.expectedJobs);
    }
}

void IndexerScanTest::cancelPersistsCompletedRowsAndRerunSkipsThem()
{
    requireTool(QStringLiteral("ffmpeg"));
    requireTool(QStringLiteral("fpcalc"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir audioDir(dir.filePath(QStringLiteral("audio")));
    QVERIFY(QDir().mkpath(audioDir.absolutePath()));

    QStringList files;
    for (int index = 0; index < 4; ++index) {
        const QString file = audioDir.filePath(QStringLiteral("cancel-%1.wav").arg(index));
        ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                QStringLiteral("sine=frequency=%1:duration=120:sample_rate=44100").arg(330 + index * 70),
                file});
        files << file;
    }

    const QString library = dir.filePath(QStringLiteral("library.sqlite"));
    const QString features = dir.filePath(QStringLiteral("features.sqlite"));
    createLibrary(library, files);

    const QStringList scanArgs{
        QStringLiteral("scan"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--features"),
        features,
        QStringLiteral("--jobs"),
        QStringLiteral("1"),
        QStringLiteral("--json"),
        QStringLiteral("--progress"),
    };

    QProcess process;
    process.setReadChannel(QProcess::StandardError);
    process.start(muzaitenIndexPath(), scanArgs);
    QVERIFY2(process.waitForStarted(5000), qPrintable(process.errorString()));

    QByteArray stderrBytes;
    QElapsedTimer timer;
    timer.start();
    bool sawProgress = false;
    const QRegularExpression progressPattern(QStringLiteral("^progress\\s+\\d+/\\d+"),
                                             QRegularExpression::MultilineOption);
    while (timer.elapsed() < 120000 && process.state() != QProcess::NotRunning && !sawProgress) {
        if (process.waitForReadyRead(1000)) {
            stderrBytes.append(process.readAllStandardError());
            sawProgress = QString::fromUtf8(stderrBytes).contains(progressPattern);
        }
    }
    QVERIFY2(sawProgress, qPrintable(QString::fromUtf8(stderrBytes)));

    process.terminate();
    QVERIFY2(process.waitForFinished(120000), qPrintable(process.errorString()));
    stderrBytes.append(process.readAllStandardError());
    const QByteArray stdoutBytes = process.readAllStandardOutput();
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);

    const QJsonObject canceled = parseJsonObject(stdoutBytes);
    QVERIFY(canceled.value(QStringLiteral("canceled")).toBool());
    QVERIFY(canceled.value(QStringLiteral("scanned")).toInt() > 0);
    const int persisted = fileRowCount(features);
    QVERIFY(persisted > 0);
    QCOMPARE(persisted, canceled.value(QStringLiteral("scanned")).toInt());

    const QJsonObject rerun = runIndexer(scanArgs);
    QCOMPARE(rerun.value(QStringLiteral("canceled")).toBool(), false);
    QCOMPARE(rerun.value(QStringLiteral("skipped")).toInt(), persisted);
    QCOMPARE(rerun.value(QStringLiteral("scanned")).toInt(), files.size() - persisted);
}

void IndexerScanTest::incrementalRescanPreservesFeatureRows()
{
    requireTool(QStringLiteral("ffmpeg"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir audioDir(dir.filePath(QStringLiteral("audio")));
    QVERIFY(QDir().mkpath(audioDir.absolutePath()));

    // Synthetic steady signals produce chromaprints degenerate enough to
    // BER-match each other (even 440 vs 660 sines), so distinct durations
    // beyond the ±2 s grouping bucket are what guarantee separate groups.
    const QString toneA = audioDir.filePath(QStringLiteral("tone-a.wav"));
    const QString toneB = audioDir.filePath(QStringLiteral("tone-b.wav"));
    const QString added = audioDir.filePath(QStringLiteral("tone-added.wav"));
    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("sine=frequency=440:duration=10:sample_rate=44100"), toneA});
    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("sine=frequency=660:duration=15:sample_rate=44100"), toneB});

    const QString library = dir.filePath(QStringLiteral("library.sqlite"));
    const QString features = dir.filePath(QStringLiteral("features.sqlite"));
    createLibrary(library, {toneA, toneB});

    const QStringList scanArgs{
        QStringLiteral("scan"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--features"),
        features,
        QStringLiteral("--json"),
    };
    const QJsonObject first = runIndexer(scanArgs);
    QCOMPARE(first.value(QStringLiteral("scanned")).toInt(), 2);

    qint64 toneAGroup = -1;
    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        toneAGroup = groupFor(db, toneA);
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }

    // Plant a sentinel in toneA's feature row: incremental rescans must keep
    // the row (freshness is keyed on the dsp version), not wipe + recompute.
    {
        const QString connectionName = QStringLiteral("indexer-scan-sentinel-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(features);
            QVERIFY(db.open());
            QSqlQuery update(db);
            update.prepare(QStringLiteral("UPDATE features SET tempo_bpm = 999.5 WHERE content_group_id = ?"));
            update.addBindValue(toneAGroup);
            QVERIFY2(update.exec(), qPrintable(update.lastError().text()));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }

    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("anoisesrc=color=pink:duration=20:amplitude=0.2:sample_rate=44100"), added});
    {
        const QString connectionName = QStringLiteral("indexer-scan-grow-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(library);
            QVERIFY(db.open());
            const QPair<qint64, qint64> stats = fileStats(added);
            QSqlQuery insert(db);
            insert.prepare(QStringLiteral(
                "INSERT INTO tracks(path, file_mtime, file_size, missing) VALUES(?, ?, ?, 0)"));
            insert.addBindValue(added);
            insert.addBindValue(stats.first);
            insert.addBindValue(stats.second);
            QVERIFY2(insert.exec(), qPrintable(insert.lastError().text()));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }

    const QJsonObject second = runIndexer(scanArgs);
    QCOMPARE(second.value(QStringLiteral("scanned")).toInt(), 1);
    QCOMPARE(second.value(QStringLiteral("skipped")).toInt(), 2);

    const QJsonObject third = runIndexer(scanArgs);
    QCOMPARE(third.value(QStringLiteral("scanned")).toInt(), 0);

    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        const std::optional<double> tempo = optionalFeature(db, toneAGroup, QStringLiteral("tempo_bpm"));
        QVERIFY2(tempo.has_value(), "sentinel feature row was wiped by a rescan");
        QCOMPARE(*tempo, 999.5);
        const qint64 addedGroup = groupFor(db, added);
        QVERIFY(addedGroup != toneAGroup);
        QSqlQuery count(db);
        QVERIFY(count.exec(QStringLiteral("SELECT COUNT(*) FROM features")));
        QVERIFY(count.next());
        QCOMPARE(count.value(0).toInt(), 3);
        // The no-op third run must not clobber the incremental run's summary.
        QSqlQuery meta(db);
        QVERIFY(meta.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'last_scan_scanned'")));
        QVERIFY(meta.next());
        QCOMPARE(meta.value(0).toString(), QStringLiteral("1"));
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }
}

void IndexerScanTest::featurePhaseProgressAndStaleDenom()
{
    requireTool(QStringLiteral("ffmpeg"));
    QVERIFY(QFileInfo::exists(muzaitenIndexPath()));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString audioDirPath = dir.filePath(QStringLiteral("audio"));
    QVERIFY(QDir().mkpath(audioDirPath));
    const QDir audioDir(audioDirPath);

    // Distinct duration + timbre so content grouping yields enough separate groups.
    const QStringList sources{
        QStringLiteral("aevalsrc=(lt(mod(t\\,0.5)\\,0.002))*0.9:duration=6:sample_rate=44100"),
        QStringLiteral("aevalsrc=(lt(mod(t\\,0.6666667)\\,0.002))*0.9:duration=8:sample_rate=44100"),
        QStringLiteral("aevalsrc=(lt(mod(t\\,0.4)\\,0.002))*0.9:duration=10:sample_rate=44100"),
        QStringLiteral("anoisesrc=color=pink:duration=5:amplitude=0.2:sample_rate=44100:seed=41"),
        QStringLiteral("anoisesrc=color=brown:duration=9:amplitude=0.2:sample_rate=44100:seed=42"),
        QStringLiteral("sine=frequency=440:duration=7:sample_rate=44100"),
    };
    QStringList paths;
    for (int i = 0; i < sources.size(); ++i) {
        const QString path = audioDir.filePath(QStringLiteral("item%1.wav").arg(i));
        ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), sources.at(i), path});
        paths.push_back(path);
    }

    const QString library = dir.filePath(QStringLiteral("library.sqlite"));
    const QString features = dir.filePath(QStringLiteral("features.sqlite"));
    createLibrary(library, paths);

    const QJsonObject first = runIndexer({
        QStringLiteral("scan"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--features"),
        features,
        QStringLiteral("--jobs"),
        QStringLiteral("1"),
        QStringLiteral("--json"),
    });
    QCOMPARE(first.value(QStringLiteral("scanned")).toInt(), sources.size());
    QCOMPARE(first.value(QStringLiteral("canceled")).toBool(), false);

    QVector<qint64> groupIds;
    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("SELECT content_group_id FROM features ORDER BY content_group_id")));
        while (query.next()) {
            groupIds.push_back(query.value(0).toLongLong());
        }
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }
    // Need missing + older + NULL + at least one fresh remaining.
    QVERIFY2(groupIds.size() >= 4,
             qPrintable(QStringLiteral("expected >=4 content groups, got %1").arg(groupIds.size())));
    const int totalGroups = static_cast<int>(groupIds.size());
    const int expectedStale = 3;

    {
        QString connectionName =
            QStringLiteral("feature-stale-mutate-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(features);
            QVERIFY(db.open());
            QSqlQuery del(db);
            del.prepare(QStringLiteral("DELETE FROM features WHERE content_group_id = ?"));
            del.addBindValue(groupIds.at(0));
            QVERIFY2(del.exec(), qPrintable(del.lastError().text()));

            QSqlQuery stale(db);
            stale.prepare(QStringLiteral("UPDATE features SET version = '0' WHERE content_group_id = ?"));
            stale.addBindValue(groupIds.at(1));
            QVERIFY2(stale.exec(), qPrintable(stale.lastError().text()));

            // Schema is NOT NULL; empty version is the null-ish stale stand-in
            // (true SQL NULL only appears via LEFT JOIN on a missing row).
            QSqlQuery emptyVersion(db);
            emptyVersion.prepare(QStringLiteral("UPDATE features SET version = '' WHERE content_group_id = ?"));
            emptyVersion.addBindValue(groupIds.at(2));
            QVERIFY2(emptyVersion.exec(), qPrintable(emptyVersion.lastError().text()));
            // Remaining groups stay current version → fresh, excluded from N
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }

    const QJsonObject staleStatus = runIndexer({
        QStringLiteral("status"),
        QStringLiteral("--features"),
        features,
        QStringLiteral("--json"),
    });
    QCOMPARE(staleStatus.value(QStringLiteral("featured_groups")).toInt(), totalGroups - 1);
    QCOMPARE(staleStatus.value(QStringLiteral("featured_fresh")).toInt(), totalGroups - expectedStale);
    QCOMPARE(staleStatus.value(QStringLiteral("featured_stale")).toInt(), expectedStale - 1);

    QByteArray progressStderr;
    const QJsonObject refresh = runIndexer({
        QStringLiteral("scan"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--features"),
        features,
        QStringLiteral("--stage"),
        QStringLiteral("features"),
        QStringLiteral("--jobs"),
        QStringLiteral("2"),
        QStringLiteral("--json"),
        QStringLiteral("--progress"),
    }, &progressStderr);
    const QString progressLog = QString::fromUtf8(progressStderr);
    const qsizetype featuresIndex = progressLog.indexOf(QStringLiteral("phase features"));
    QVERIFY2(featuresIndex >= 0, qPrintable(progressLog));
    const QString afterFeatures = progressLog.mid(featuresIndex);
    QVERIFY2(afterFeatures.contains(QRegularExpression(
                 QStringLiteral("^progress 0/%1 elapsed=[0-9.]+ rate=- eta=-$").arg(expectedStale),
                 QRegularExpression::MultilineOption)),
             qPrintable(afterFeatures));
    QCOMPARE(refresh.value(QStringLiteral("feature_groups_processed")).toInt(), expectedStale);
    QCOMPARE(refresh.value(QStringLiteral("features_written")).toInt(), expectedStale);
    QCOMPARE(refresh.value(QStringLiteral("feature_groups_failed")).toInt(), 0);
    QCOMPARE(refresh.value(QStringLiteral("canceled")).toBool(), false);
    QCOMPARE(refresh.value(QStringLiteral("featured_fresh")).toInt(), totalGroups);
    QCOMPARE(refresh.value(QStringLiteral("featured_stale")).toInt(), 0);

    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        QSqlQuery count(db);
        QVERIFY(count.exec(QStringLiteral("SELECT COUNT(*) FROM features")));
        QVERIFY(count.next());
        QCOMPARE(count.value(0).toInt(), totalGroups);
        QSqlQuery versions(db);
        versions.prepare(QStringLiteral("SELECT COUNT(*) FROM features WHERE version = ?"));
        versions.addBindValue(QString::fromLatin1(Dsp::kDspVersion));
        QVERIFY(versions.exec());
        QVERIFY(versions.next());
        QCOMPARE(versions.value(0).toInt(), totalGroups);
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }
}

void IndexerScanTest::featurePhaseCancelPreservesWrittenRows()
{
    requireTool(QStringLiteral("ffmpeg"));
    QVERIFY(QFileInfo::exists(muzaitenIndexPath()));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString audioDirPath = dir.filePath(QStringLiteral("audio"));
    QVERIFY(QDir().mkpath(audioDirPath));
    const QDir audioDir(audioDirPath);

    // More than the 25-item progress cadence guarantees a non-final progress
    // line even when an optimized analyzer finishes the corpus in under two
    // seconds. Differently-duration reps keep the groups distinct.
    constexpr int kFeatureCancelItems = 32;
    QStringList paths;
    for (int index = 0; index < kFeatureCancelItems; ++index) {
        const QString path = audioDir.filePath(QStringLiteral("feat-cancel-%1.wav").arg(index));
        // Long enough to exercise real decoding/DSP without making the test's
        // cancellation trigger depend on wall-clock speed.
        const int duration = 20 + index * 3;
        if (index % 3 == 0) {
            ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                    QStringLiteral("anoisesrc=color=pink:duration=%1:amplitude=0.2:sample_rate=44100")
                        .arg(duration),
                    path});
        } else if (index % 3 == 1) {
            ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                    QStringLiteral(
                        "aevalsrc=(lt(mod(t\\,%1)\\,0.002))*0.9:duration=%2:sample_rate=44100")
                        .arg(QString::number(0.35 + 0.05 * (index % 4), 'f', 2))
                        .arg(duration),
                    path});
        } else {
            ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                    QStringLiteral("sine=frequency=%1:duration=%2:sample_rate=44100")
                        .arg(220 + index * 37)
                        .arg(duration),
                    path});
        }
        paths.push_back(path);
    }

    const QString library = dir.filePath(QStringLiteral("library.sqlite"));
    const QString features = dir.filePath(QStringLiteral("features.sqlite"));
    createLibrary(library, paths);

    const QStringList identityArgs{
        QStringLiteral("scan"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--features"),
        features,
        QStringLiteral("--jobs"),
        QStringLiteral("1"),
        QStringLiteral("--json"),
        QStringLiteral("--stage"),
        QStringLiteral("identity"),
    };
    const QJsonObject first = runIndexer(identityArgs);
    QCOMPARE(first.value(QStringLiteral("scanned")).toInt(), kFeatureCancelItems);

    runIndexer({
        QStringLiteral("scan"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--features"),
        features,
        QStringLiteral("--jobs"),
        QStringLiteral("2"),
        QStringLiteral("--json"),
        QStringLiteral("--stage"),
        QStringLiteral("features"),
    });

    int featureCountBefore = 0;
    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        QSqlQuery count(db);
        QVERIFY(count.exec(QStringLiteral("SELECT COUNT(*) FROM features")));
        QVERIFY(count.next());
        featureCountBefore = count.value(0).toInt();
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }
    QVERIFY2(featureCountBefore >= 2,
             qPrintable(QStringLiteral("need multiple feature rows, got %1").arg(featureCountBefore)));

    {
        QString connectionName =
            QStringLiteral("feature-cancel-stale-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(features);
            QVERIFY(db.open());
            // Force every group stale without wiping row structure: version older.
            QSqlQuery stale(db);
            QVERIFY2(stale.exec(QStringLiteral("UPDATE features SET version = '0'")),
                     qPrintable(stale.lastError().text()));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }

    QProcess process;
    process.setReadChannel(QProcess::StandardError);
    process.start(muzaitenIndexPath(), {
                                           QStringLiteral("scan"),
                                           QStringLiteral("--library"),
                                           library,
                                           QStringLiteral("--features"),
                                           features,
                                           QStringLiteral("--stage"),
                                           QStringLiteral("features"),
                                           QStringLiteral("--jobs"),
                                           QStringLiteral("2"),
                                           QStringLiteral("--json"),
                                           QStringLiteral("--progress"),
                                       });
    QVERIFY2(process.waitForStarted(5000), qPrintable(process.errorString()));

    QElapsedTimer timer;
    timer.start();
    bool sawLiveProgress = false;
    QByteArray stderrBytes;
    QByteArray stdoutBytes;
    // Wait for nonzero mid-phase progress (cadence ≥2s or ≥25), then stop.
    const QRegularExpression midProgress(QStringLiteral("^progress ([1-9]\\d*)/(\\d+)"),
                                         QRegularExpression::MultilineOption);
    while (timer.elapsed() < 180'000 && process.state() != QProcess::NotRunning && !sawLiveProgress) {
        if (process.waitForReadyRead(200)) {
            stderrBytes += process.readAllStandardError();
            QRegularExpressionMatchIterator it =
                midProgress.globalMatch(QString::fromUtf8(stderrBytes));
            while (it.hasNext()) {
                const QRegularExpressionMatch match = it.next();
                const int n = match.captured(1).toInt();
                const int m = match.captured(2).toInt();
                if (n > 0 && n < m) {
                    sawLiveProgress = true;
                    break;
                }
            }
        }
    }
    QVERIFY2(sawLiveProgress, qPrintable(QString::fromUtf8(stderrBytes)));
    process.terminate();
    QVERIFY2(process.waitForFinished(180'000), qPrintable(process.errorString()));
    stderrBytes += process.readAllStandardError();
    stdoutBytes += process.readAllStandardOutput();
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);

    const QJsonObject canceledPayload = parseJsonObject(stdoutBytes);
    // With per-file scalar rows fresh, this refresh runs on the SQL copy
    // path: the mid-phase progress line at the 25-item cadence gives the
    // terminate() above a real window because every copy is an autocommit
    // write with a stopRequested() check between rows. The assertions below
    // therefore pin cancel/durability/resume semantics for copies too.
    QVERIFY2(canceledPayload.value(QStringLiteral("canceled")).toBool(),
             qPrintable(QString::fromUtf8(stdoutBytes) + QString::fromUtf8(stderrBytes)));
    const int writtenOnCancel = canceledPayload.value(QStringLiteral("features_written")).toInt();
    QVERIFY(writtenOnCancel >= 1);
    QVERIFY(writtenOnCancel < featureCountBefore);
    QVERIFY(canceledPayload.value(QStringLiteral("feature_groups_processed")).toInt()
            >= writtenOnCancel);
    QCOMPARE(canceledPayload.value(QStringLiteral("featured_fresh")).toInt(), writtenOnCancel);
    QCOMPARE(canceledPayload.value(QStringLiteral("featured_stale")).toInt(),
             featureCountBefore - writtenOnCancel);

    int freshAfterCancel = 0;
    int staleAfterCancel = 0;
    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        QSqlQuery fresh(db);
        fresh.prepare(QStringLiteral("SELECT COUNT(*) FROM features WHERE version = ?"));
        fresh.addBindValue(QString::fromLatin1(Dsp::kDspVersion));
        QVERIFY(fresh.exec());
        QVERIFY(fresh.next());
        freshAfterCancel = fresh.value(0).toInt();
        QSqlQuery stale(db);
        QVERIFY(stale.exec(QStringLiteral("SELECT COUNT(*) FROM features WHERE version = '0'")));
        QVERIFY(stale.next());
        staleAfterCancel = stale.value(0).toInt();
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }
    QCOMPARE(freshAfterCancel, writtenOnCancel);
    QCOMPARE(staleAfterCancel, featureCountBefore - writtenOnCancel);

    QByteArray resumeStderr;
    const QJsonObject resume = runIndexer({
        QStringLiteral("scan"),
        QStringLiteral("--library"),
        library,
        QStringLiteral("--features"),
        features,
        QStringLiteral("--stage"),
        QStringLiteral("features"),
        QStringLiteral("--jobs"),
        QStringLiteral("2"),
        QStringLiteral("--json"),
        QStringLiteral("--progress"),
    }, &resumeStderr);
    const QString resumeLog = QString::fromUtf8(resumeStderr);
    const qsizetype resumeFeatures = resumeLog.indexOf(QStringLiteral("phase features"));
    QVERIFY2(resumeFeatures >= 0, qPrintable(resumeLog));
    const QRegularExpression zeroProgress(QStringLiteral("^progress 0/(\\d+) "),
                                          QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = zeroProgress.match(resumeLog.mid(resumeFeatures));
    QVERIFY2(match.hasMatch(), qPrintable(resumeLog));
    QCOMPARE(match.captured(1).toInt(), staleAfterCancel);
    QCOMPARE(resume.value(QStringLiteral("feature_groups_processed")).toInt(), staleAfterCancel);
    QCOMPARE(resume.value(QStringLiteral("canceled")).toBool(), false);
    QCOMPARE(resume.value(QStringLiteral("featured_fresh")).toInt(), featureCountBefore);
    QCOMPARE(resume.value(QStringLiteral("featured_stale")).toInt(), 0);

    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        QSqlQuery fresh(db);
        fresh.prepare(QStringLiteral("SELECT COUNT(*) FROM features WHERE version = ?"));
        fresh.addBindValue(QString::fromLatin1(Dsp::kDspVersion));
        QVERIFY(fresh.exec());
        QVERIFY(fresh.next());
        QCOMPARE(fresh.value(0).toInt(), featureCountBefore);
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }
}

void IndexerScanTest::schemaV3StoreUpgradesInPlaceKeepingFeatureRows()
{
    requireTool(QStringLiteral("ffmpeg"));
    QVERIFY(QFileInfo::exists(muzaitenIndexPath()));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString audioDirPath = dir.filePath(QStringLiteral("audio"));
    QVERIFY(QDir().mkpath(audioDirPath));
    const QDir audioDir(audioDirPath);
    QStringList paths;
    for (int i = 0; i < 2; ++i) {
        const QString path = audioDir.filePath(QStringLiteral("up%1.wav").arg(i));
        ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                QStringLiteral("sine=frequency=%1:duration=6:sample_rate=44100").arg(330 + i * 220),
                path});
        paths.push_back(path);
    }
    const QString library = dir.filePath(QStringLiteral("library.sqlite"));
    const QString features = dir.filePath(QStringLiteral("features.sqlite"));
    createLibrary(library, paths);

    const QJsonObject first = runIndexer({
        QStringLiteral("scan"), QStringLiteral("--library"), library,
        QStringLiteral("--features"), features,
        QStringLiteral("--jobs"), QStringLiteral("1"), QStringLiteral("--json"),
    });
    QCOMPARE(first.value(QStringLiteral("scanned")).toInt(), 2);
    const int featuredBefore = first.value(QStringLiteral("featured_groups")).toInt();
    QVERIFY(featuredBefore >= 1);

    // Regress the store to schema v3: version stamp back, additive table
    // gone. The upgrade must recreate the table and must NOT drop the
    // v3-shaped features rows (the historical version-mismatch drop wiped
    // them; upgrades are additive now).
    {
        QString connectionName = QStringLiteral("upgrade-mutate-%1")
                                     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(features);
            QVERIFY(db.open());
            QSqlQuery mutate(db);
            QVERIFY(mutate.exec(QStringLiteral("UPDATE meta SET value='3' WHERE key='schema_version'")));
            QVERIFY(mutate.exec(QStringLiteral("DROP TABLE IF EXISTS file_features")));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }

    const QJsonObject rescan = runIndexer({
        QStringLiteral("scan"), QStringLiteral("--library"), library,
        QStringLiteral("--features"), features,
        QStringLiteral("--jobs"), QStringLiteral("1"), QStringLiteral("--json"),
    });
    QCOMPARE(rescan.value(QStringLiteral("scanned")).toInt(), 0);
    QCOMPARE(rescan.value(QStringLiteral("featured_groups")).toInt(), featuredBefore);
    QCOMPARE(rescan.value(QStringLiteral("schema_version")).toInt(), 4);

    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        QSqlQuery meta(db);
        QVERIFY(meta.exec(QStringLiteral("SELECT value FROM meta WHERE key='schema_version'")));
        QVERIFY(meta.next());
        QCOMPARE(meta.value(0).toString(), QStringLiteral("4"));
        QSqlQuery count(db);
        QVERIFY(count.exec(QStringLiteral("SELECT COUNT(*) FROM features")));
        QVERIFY(count.next());
        QCOMPARE(count.value(0).toInt(), featuredBefore);
        QSqlQuery table(db);
        QVERIFY(table.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='file_features'")));
        QVERIFY(table.next());
        QCOMPARE(table.value(0).toInt(), 1);
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }
}

void IndexerScanTest::forcedRefreshCopiesWithoutTouchingAudio()
{
    requireTool(QStringLiteral("ffmpeg"));
    QVERIFY(QFileInfo::exists(muzaitenIndexPath()));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString audioDirPath = dir.filePath(QStringLiteral("audio"));
    QVERIFY(QDir().mkpath(audioDirPath));
    const QDir audioDir(audioDirPath);
    QStringList paths;
    for (int i = 0; i < 4; ++i) {
        const QString path = audioDir.filePath(QStringLiteral("copy%1.wav").arg(i));
        ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                QStringLiteral("anoisesrc=color=pink:duration=%1:amplitude=0.2:sample_rate=44100:seed=%2")
                    .arg(6 + i * 3)
                    .arg(i),
                path});
        paths.push_back(path);
    }
    const QString library = dir.filePath(QStringLiteral("library.sqlite"));
    const QString features = dir.filePath(QStringLiteral("features.sqlite"));
    createLibrary(library, paths);

    const QStringList scanArgs{
        QStringLiteral("scan"), QStringLiteral("--library"), library,
        QStringLiteral("--features"), features,
        QStringLiteral("--jobs"), QStringLiteral("2"), QStringLiteral("--json"),
    };
    const QJsonObject first = runIndexer(scanArgs);
    QCOMPARE(first.value(QStringLiteral("scanned")).toInt(), 4);
    const int groups = first.value(QStringLiteral("featured_groups")).toInt();
    QVERIFY(groups >= 2);

    // Migration shape: BOTH group and per-file rows stale (an older-version
    // store meeting a newer binary). The refresh must decode representatives
    // and, as a side effect, backfill fresh file_features rows.
    {
        QString connectionName = QStringLiteral("copy-test-%1")
                                     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(features);
            QVERIFY(db.open());
            QSqlQuery stale(db);
            QVERIFY(stale.exec(QStringLiteral("UPDATE features SET version='old'")));
            // Poison the stale cache too: a version-blind copy would leak
            // these impossible values into current group rows. The fallback
            // must decode, replace each representative's cache row, and copy
            // none of these sentinels.
            QVERIFY(stale.exec(QStringLiteral(
                "UPDATE file_features SET version='old', tempo_bpm=999, energy=999")));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }
    const QJsonObject migration = runIndexer({
        QStringLiteral("scan"), QStringLiteral("--library"), library,
        QStringLiteral("--features"), features, QStringLiteral("--stage"),
        QStringLiteral("features"), QStringLiteral("--jobs"), QStringLiteral("2"),
        QStringLiteral("--json"),
    });
    QCOMPARE(migration.value(QStringLiteral("features_written")).toInt(), groups);
    QCOMPARE(migration.value(QStringLiteral("feature_groups_failed")).toInt(), 0);
    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        {
            QSqlQuery freshRepresentatives(db);
            freshRepresentatives.prepare(QStringLiteral(
                "SELECT COUNT(*) FROM ("
                " SELECT content_group_id, MIN(path) AS path FROM files "
                " WHERE content_group_id IS NOT NULL AND status='ok' "
                " GROUP BY content_group_id"
                ") reps JOIN file_features ff ON ff.path=reps.path "
                "WHERE ff.version=?"));
            freshRepresentatives.addBindValue(QString::fromLatin1(Dsp::kDspVersion));
            QVERIFY(freshRepresentatives.exec());
            QVERIFY(freshRepresentatives.next());
            QCOMPARE(freshRepresentatives.value(0).toInt(), groups);

            QSqlQuery poison(db);
            QVERIFY(poison.exec(QStringLiteral(
                "SELECT COUNT(*) FROM features WHERE tempo_bpm=999 OR energy=999")));
            QVERIFY(poison.next());
            QCOMPARE(poison.value(0).toInt(), 0);
        }
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }

    // Second forced refresh with fresh per-file rows — and NO audio on disk.
    // Success is only possible on the pure copy path; any attempted decode
    // would surface as feature_groups_failed. The trigger additionally proves
    // the feature-only command does not rebuild already-clean content groups;
    // that unrelated O(files) work would erase the warm-path latency win.
    {
        QString connectionName = QStringLiteral("copy-test2-%1")
                                     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(features);
            QVERIFY(db.open());
            QSqlQuery stale(db);
            QVERIFY(stale.exec(QStringLiteral("UPDATE features SET version='old'")));
            QVERIFY(stale.exec(QStringLiteral(
                "CREATE TRIGGER reject_unneeded_regroup BEFORE DELETE ON content_groups "
                "BEGIN SELECT RAISE(ABORT, 'feature-only refresh regrouped clean store'); END")));
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }
    for (const QString &path : paths) {
        QVERIFY(QFile::remove(path));
    }
    const QJsonObject copies = runIndexer({
        QStringLiteral("scan"), QStringLiteral("--library"), library,
        QStringLiteral("--features"), features, QStringLiteral("--stage"),
        QStringLiteral("features"), QStringLiteral("--jobs"), QStringLiteral("2"),
        QStringLiteral("--json"),
    });
    QCOMPARE(copies.value(QStringLiteral("features_written")).toInt(), groups);
    QCOMPARE(copies.value(QStringLiteral("feature_groups_failed")).toInt(), 0);
    QCOMPARE(copies.value(QStringLiteral("featured_stale")).toInt(), 0);
    QCOMPARE(copies.value(QStringLiteral("canceled")).toBool(), false);
}

void IndexerScanTest::orphanedFileFeatureRowsAreSwept()
{
    requireTool(QStringLiteral("ffmpeg"));
    QVERIFY(QFileInfo::exists(muzaitenIndexPath()));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString audioDirPath = dir.filePath(QStringLiteral("audio"));
    QVERIFY(QDir().mkpath(audioDirPath));
    const QDir audioDir(audioDirPath);
    QStringList paths;
    for (int i = 0; i < 2; ++i) {
        const QString path = audioDir.filePath(QStringLiteral("sweep%1.wav").arg(i));
        ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                QStringLiteral("anoisesrc=color=pink:duration=%1:amplitude=0.2:sample_rate=44100:seed=%2")
                    .arg(6 + i * 3)
                    .arg(10 + i),
                path});
        paths.push_back(path);
    }
    const QString library = dir.filePath(QStringLiteral("library.sqlite"));
    const QString features = dir.filePath(QStringLiteral("features.sqlite"));
    createLibrary(library, paths);
    runIndexer({
        QStringLiteral("scan"), QStringLiteral("--library"), library,
        QStringLiteral("--features"), features,
        QStringLiteral("--jobs"), QStringLiteral("1"), QStringLiteral("--json"),
    });

    // Simulate a pruned files row; the per-file scalar row must follow it on
    // the next scan that regroups (adding a third file forces that path).
    {
        QString connectionName = QStringLiteral("sweep-%1")
                                     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(features);
            QVERIFY(db.open());
            QSqlQuery prune(db);
            prune.prepare(QStringLiteral("DELETE FROM files WHERE path = ?"));
            prune.addBindValue(paths.first());
            QVERIFY(prune.exec());
            db.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }
    const QString extra = audioDir.filePath(QStringLiteral("sweep-extra.wav"));
    ffmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("anoisesrc=color=pink:duration=12:amplitude=0.2:sample_rate=44100:seed=99"),
            extra});
    // The pruned file leaves the library too, or the rescan would simply
    // re-analyze it and rewrite the very row the sweep should remove.
    QVERIFY(QFile::remove(library));
    createLibrary(library, QStringList() << paths.at(1) << extra);
    runIndexer({
        QStringLiteral("scan"), QStringLiteral("--library"), library,
        QStringLiteral("--features"), features,
        QStringLiteral("--jobs"), QStringLiteral("1"), QStringLiteral("--json"),
    });

    {
        QString connectionName;
        QSqlDatabase db = openReadOnly(features, &connectionName);
        QVERIFY(db.open());
        QSqlQuery gone(db);
        gone.prepare(QStringLiteral("SELECT COUNT(*) FROM file_features WHERE path = ?"));
        gone.addBindValue(paths.first());
        QVERIFY(gone.exec());
        QVERIFY(gone.next());
        QCOMPARE(gone.value(0).toInt(), 0);
        db.close();
        QSqlDatabase::removeDatabase(connectionName);
    }
}

QTEST_MAIN(IndexerScanTest)
#include "test_features_refresh.moc"
