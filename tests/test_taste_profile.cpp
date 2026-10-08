#include "app/AppPaths.h"
#include "core/FoldKey.h"
#include "core/MetadataBlob.h"
#include "db/Database.h"
#include "features/FeatureStore.h"
#include "indexer/DspVersion.h"
#include "reco/RadioFilters.h"
#include "reco/RadioSession.h"
#include "reco/ReasonText.h"
#include "reco/TasteProfile.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace {

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QMap<QString, QByteArray> snapshot(const QString &root)
{
    QMap<QString, QByteArray> files;
    QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        files.insert(QDir(root).relativeFilePath(path), readFile(path));
    }
    return files;
}

QVector<TrackScorer::Candidate> poolFor(Database &db, const QString &featuresPath)
{
    FeatureStore features(featuresPath);
    QVector<TrackScorer::Candidate> pool;
    for (const auto &row : db.radioFallbackCandidates(5000)) {
        TrackScorer::Candidate candidate;
        candidate.path = row.path;
        candidate.songKey = FoldKey::songKey(row.mbRecordingId, row.artistName, row.title);
        candidate.artistFolded = FoldKey::fold(row.artistName);
        candidate.albumKey = FoldKey::albumGroupKey(row.releaseGroupId, row.albumArtistName, row.albumTitle);
        candidate.genresFolded = row.genresFolded;
        candidate.year = row.year;
        const auto scalars = features.scalarsForGroup(features.contentGroupForPath(row.path));
        if (scalars.valid) {
            candidate.tempoBpm = scalars.tempoBpm;
            candidate.energy = scalars.energy;
        }
        pool.append(candidate);
    }
    std::sort(pool.begin(), pool.end(), [](const auto &left, const auto &right) { return left.path < right.path; });
    return RadioFilters::excludeFlaggedCandidates(pool, db.flaggedPaths(Database::TrackFlag::NeverRadio));
}

Track libraryTrack(const QString &path, const QString &genre, const QString &date, int index)
{
    Track track;
    track.path = path;
    track.parentDir = QFileInfo(track.path).absolutePath();
    track.filename = QFileInfo(track.path).fileName();
    track.title = QStringLiteral("Fixture title %1").arg(index);
    track.artistName = QStringLiteral("Fixture artist %1").arg(index);
    track.albumArtistName = track.artistName;
    track.albumTitle = QStringLiteral("Fixture album %1").arg(index);
    track.date = date;
    track.durationMs = 180000;
    track.codec = QStringLiteral("flac");
    MetadataBlob::FullMetadata metadata;
    metadata.tags.insert(QStringLiteral("GENRE"), {genre});
    const auto encoded = MetadataBlob::encode(metadata);
    track.fullMetadataBlob = encoded.data;
    track.fullMetadataRawSize = encoded.rawSize;
    return track;
}

bool atQuantum(double value, double quantum)
{
    return std::abs(value / quantum - std::round(value / quantum)) < 1e-9;
}

// Empty when every exported number carries at most its quantum's precision:
// shares and energy in hundredths (shares summing to exactly 1.00), tempo in
// whole BPM, and no longer decimal spelled anywhere in the file.
QString quantizationViolation(const TasteProfile &profile, const QByteArray &bytes)
{
    for (const auto *shares : {&profile.genres, &profile.decades}) {
        qint64 hundredths = 0;
        for (const double share : *shares) {
            if (!atQuantum(share, 0.01)) {
                return QStringLiteral("share %1").arg(share, 0, 'g', 17);
            }
            hundredths += std::llround(share * 100.0);
        }
        if (!shares->isEmpty() && hundredths != 100) {
            return QStringLiteral("shares sum to %1 hundredths").arg(hundredths);
        }
    }
    for (auto it = profile.features.begin(); it != profile.features.end(); ++it) {
        const double quantum = it.key() == QLatin1String("tempo_bpm") ? 1.0 : 0.01;
        for (const double value : {it->q25, it->q50, it->q75}) {
            if (!atQuantum(value, quantum)) {
                return QStringLiteral("%1 quartile %2").arg(it.key()).arg(value, 0, 'g', 17);
            }
        }
    }
    const auto precise = QRegularExpression(QStringLiteral("\\d\\.\\d{3,}|[eE][-+]?\\d")).match(QString::fromUtf8(bytes));
    return precise.hasMatch() ? QStringLiteral("spelled %1").arg(precise.captured()) : QString{};
}

QString seedName(int index)
{
    return QStringLiteral("seed-%1").arg(index, 2, 10, QLatin1Char('0'));
}

QStringList seedNames(const QByteArray &listing)
{
    return QString::fromUtf8(listing).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

// 256 buckets at share 1/256 with labels of `cjk` CJK characters (three bytes
// each) and three letters. At cjk = 76 the normalized profile is 65388 bytes,
// 148 bytes under the bound; at cjk = 77 it is over the bound.
TasteProfile nearLimitProfile(int cjk)
{
    TasteProfile profile;
    for (int i = 0; i < 256; ++i) {
        const QString letters = QStringLiteral("%1%2a").arg(QChar('a' + i / 26)).arg(QChar('a' + i % 26));
        profile.genres.insert(QString(cjk, QChar(0x7237)) + letters, 1.0 / 256.0);
    }
    return profile;
}

// The compact external spelling of a profile's genres, without the
// pretty-printed indentation that encode() adds.
QByteArray compactExchange(const TasteProfile &profile)
{
    QJsonObject genres;
    for (auto it = profile.genres.begin(); it != profile.genres.end(); ++it) {
        genres.insert(it.key(), it.value());
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("format"), QStringLiteral("muzaiten-taste-profile")},
                                     {QStringLiteral("version"), 1}, {QStringLiteral("genres"), genres},
                                     {QStringLiteral("features"), QJsonObject{}}}).toJson(QJsonDocument::Compact);
}

// A seed store with `count` entries in the indented layout, each profile in its
// normalized form.
QByteArray indentedSeedStore(const TasteProfile &profile, int count)
{
    QByteArray encoded;
    QString error;
    if (!TasteProfiles::encode(profile, &encoded, &error)) {
        return {};
    }
    const QJsonObject normalized = QJsonDocument::fromJson(encoded).object();
    QJsonArray entries;
    for (int i = 1; i <= count; ++i) {
        entries.append(QJsonObject{{QStringLiteral("name"), seedName(i)},
                                   {QStringLiteral("profile"), normalized}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("format"), QStringLiteral("muzaiten-taste-seeds")},
                                     {QStringLiteral("version"), 1}, {QStringLiteral("profiles"), entries}}).toJson();
}

} // namespace

class TasteProfileTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void supportAndScoreBounds();
    void quantizedAggregates();
    void libraryScope();
    void genreLabels();
    void strictParsing();
    void twoInstallations();
    void seedStoreLimits();
    void legacySeedStore();
    void constraintsAndSessionSnapshots();
private:
    bool fixture(const QString &root, bool source);
    QByteArray cli(const QString &root, const QStringList &arguments, int expected = 0);
    QString m_evidence;
};

bool TasteProfileTest::fixture(const QString &root, bool source)
{
    QDir().mkpath(root + QStringLiteral("/data"));
    Database library(QStringLiteral("taste-fixture-%1").arg(source ? "A" : "B"));
    if (!library.open(root + QStringLiteral("/data/library.sqlite"))) {
        return false;
    }
    const QString connection = QStringLiteral("taste-scalars");
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(root + QStringLiteral("/data/features.sqlite"));
        ok = db.open();
        QSqlQuery query(db);
        for (const QString &sql : {
            QStringLiteral("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT)"),
            QStringLiteral("INSERT INTO meta VALUES('schema_version', '1')"),
            QStringLiteral("CREATE TABLE files(path TEXT PRIMARY KEY, content_group_id INTEGER)"),
            QStringLiteral("CREATE TABLE features(content_group_id INTEGER PRIMARY KEY, tempo_bpm REAL, loudness REAL, energy REAL, brightness REAL, extractor TEXT, version TEXT)")}) {
            ok = ok && query.exec(sql);
        }
        const int total = source ? 77 : 200;
        for (int i = 0; ok && i < total; ++i) {
            const bool jazz = source ? i < 61 : i % 5 == 0;
            const QString genre = source && i == 76 ? QStringLiteral("rare genre")
                : jazz ? (source && i % 2 ? QStringLiteral("Bebop") : QStringLiteral("Jazz")) : QStringLiteral("Rock");
            const Track track = libraryTrack(
                root + QStringLiteral("/synthetic/%1.flac").arg(i, 4, 10, QLatin1Char('0')), genre,
                source && i == 76 ? QStringLiteral("1971") : jazz ? QStringLiteral("1987") : QStringLiteral("2005"), i);
            ok = library.upsertTrack(track);
            query.prepare(QStringLiteral("INSERT INTO files VALUES(?, ?)"));
            query.addBindValue(track.path);
            query.addBindValue(i + 1);
            ok = ok && query.exec();
            query.prepare(QStringLiteral("INSERT INTO features VALUES(?, ?, -12, ?, 0.5, 'fixture', ?)"));
            query.addBindValue(i + 1);
            query.addBindValue(jazz ? 100.0 + i % 9 : 170.0 + i % 9);
            query.addBindValue(jazz ? 0.35 + i % 9 * 0.01 : 0.8 + i % 9 * 0.01);
            query.addBindValue(QLatin1String(Dsp::kDspVersion));
            ok = ok && query.exec();
        }
        db.close();
        QDir().mkpath(root + QStringLiteral("/state"));
        db.setDatabaseName(root + QStringLiteral("/state/state.sqlite"));
        ok = ok && db.open();
        QSqlQuery state(db);
        ok = ok && state.exec(QStringLiteral("CREATE TABLE fixture_state(value TEXT)"));
        ok = ok && state.exec(QStringLiteral("INSERT INTO fixture_state VALUES('synthetic state sentinel')"));
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    if (source) {
        ok = ok && library.setGenreAlias(QStringLiteral("Bebop"), QStringLiteral("Jazz"));
    }
    return ok;
}

QByteArray TasteProfileTest::cli(const QString &root, const QStringList &arguments, int expected)
{
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const QString &key : environment.keys()) {
        if (key.startsWith(QLatin1String("MUZAITEN_"))) {
            environment.remove(key);
        }
    }
    environment.insert(QStringLiteral("MUZAITEN_STATE_ROOT"), root);
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    process.setProcessEnvironment(environment);
    const QString binary = QCoreApplication::applicationDirPath() + QStringLiteral("/../muzaitenctl");
    process.start(binary, arguments);
    const bool finished = process.waitForFinished(30000);
    const QByteArray output = process.readAllStandardOutput();
    const QByteArray errors = process.readAllStandardError();
    if (!m_evidence.isEmpty()) {
        QFile log(m_evidence + QStringLiteral("/commands.jsonl"));
        if (log.open(QIODevice::Append)) {
            log.write(QJsonDocument(QJsonObject{{QStringLiteral("binary"), binary},
                {QStringLiteral("arguments"), QJsonArray::fromStringList(arguments)},
                {QStringLiteral("state_root"), root}, {QStringLiteral("exit"), process.exitCode()},
                {QStringLiteral("stdout"), QString::fromUtf8(output)},
                {QStringLiteral("stderr"), QString::fromUtf8(errors)}}).toJson(QJsonDocument::Compact) + '\n');
        }
    }
    if (!finished || process.exitStatus() != QProcess::NormalExit || process.exitCode() != expected) {
        QTest::qFail(qPrintable(QStringLiteral("CLI failure: %1; %2").arg(arguments.join(' '), QString::fromUtf8(errors))), __FILE__, __LINE__);
    }
    return output;
}

void TasteProfileTest::initTestCase()
{
    const auto environment = QProcessEnvironment::systemEnvironment();
    const QString evidence = environment.value(QStringLiteral("MUZAITEN_TASTE_EVIDENCE"));
    for (const QString &key : environment.keys()) {
        if (key.startsWith(QLatin1String("MUZAITEN_"))) {
            qunsetenv(key.toUtf8().constData());
        }
    }
    if (!evidence.isEmpty()) {
        qputenv("MUZAITEN_TASTE_EVIDENCE", evidence.toUtf8());
    }
}

void TasteProfileTest::supportAndScoreBounds()
{
    QVector<TrackScorer::Candidate> tracks;
    for (int i = 0; i < 5; ++i) {
        TrackScorer::Candidate candidate;
        candidate.path = QString::number(i);
        candidate.genresFolded = {QStringLiteral("jazz"), QStringLiteral("jazz")};
        candidate.tempoBpm = 100.0 + i;
        candidate.energy = i < 4 ? 0.5 : -1;
        candidate.year = i < 4 ? 1987 : 2000;
        tracks.append(candidate);
    }
    TasteProfile profile;
    QString error;
    QVERIFY(!TasteProfiles::aggregate(tracks, false, true, &profile, &error));
    QVERIFY(TasteProfiles::aggregate(tracks, true, true, &profile, &error));
    QCOMPARE(profile.genres.value(QStringLiteral("jazz")), 1.0);
    QVERIFY(profile.features.contains(QStringLiteral("tempo_bpm")));
    QVERIFY(!profile.features.contains(QStringLiteral("energy")));
    QVERIFY(profile.decades.isEmpty());
    QCOMPARE(profile.features.value(QStringLiteral("tempo_bpm")).q50, 102.0);
    TrackScorer::SeedContext context;
    context.tasteProfile = &profile;
    context.tasteProfileName = QStringLiteral("Example seed");
    for (double tempo : {-1.0, 0.0, 102.0, 300.0, std::numeric_limits<double>::infinity()}) {
        auto candidate = tracks[0];
        candidate.tempoBpm = tempo;
        const double term = TasteProfiles::similarity(profile, candidate);
        QVERIFY(std::isfinite(term) && term >= 0.0 && term <= TasteProfiles::MaximumScore);
        const auto scored = TrackScorer::score(candidate, {}, context);
        QVERIFY(ReasonText::sentence(scored.components).contains(context.tasteProfileName));
    }
    auto duplicates = tracks;
    for (auto &track : duplicates) {
        track.songKey = QStringLiteral("one identity");
    }
    QVERIFY(!TasteProfiles::aggregate(duplicates, true, true, &profile, &error));
    duplicates = tracks;
    for (auto &track : duplicates) {
        track.contentGroupId = 42;
    }
    QVERIFY(!TasteProfiles::aggregate(duplicates, true, true, &profile, &error));
}

void TasteProfileTest::quantizedAggregates()
{
    const auto tracksWith = [](const QList<QPair<QString, int>> &genres) {
        QVector<TrackScorer::Candidate> tracks;
        for (const auto &[genre, count] : genres) {
            for (int i = 0; i < count; ++i) {
                TrackScorer::Candidate candidate;
                candidate.path = QStringLiteral("%1-%2").arg(genre).arg(i);
                candidate.genresFolded = {genre};
                const auto index = static_cast<int>(tracks.size());
                candidate.tempoBpm = 96.123456789 + index % 50 * 0.731;
                candidate.energy = 0.3123456789 + index % 50 * 0.0137;
                candidate.year = 1987 + index % 3 * 10;
                tracks.append(candidate);
            }
        }
        return tracks;
    };
    TasteProfile profile;
    QByteArray bytes;
    QString error;

    // 61/76 and 15/76 recover exactly as fractions at full precision.
    QVERIFY2(TasteProfiles::aggregate(tracksWith({{QStringLiteral("jazz"), 61}, {QStringLiteral("rock"), 15}}),
                                      true, true, &profile, &error), qPrintable(error));
    QCOMPARE(profile.genres, (QMap<QString, double>{{QStringLiteral("jazz"), 0.8}, {QStringLiteral("rock"), 0.2}}));
    QVERIFY(TasteProfiles::encode(profile, &bytes, &error));
    QVERIFY2(quantizationViolation(profile, bytes).isEmpty(), qPrintable(quantizationViolation(profile, bytes)));
    QCOMPARE(profile.decades.size(), 3);

    // Equal thirds: the leftover hundredth goes to the earliest label.
    QVERIFY(TasteProfiles::aggregate(tracksWith({{QStringLiteral("ambient"), 7}, {QStringLiteral("blues"), 7},
                                                 {QStringLiteral("cumbia"), 7}}), true, false, &profile, &error));
    QCOMPARE(profile.genres, (QMap<QString, double>{{QStringLiteral("ambient"), 0.34},
                                                    {QStringLiteral("blues"), 0.33},
                                                    {QStringLiteral("cumbia"), 0.33}}));
    QVERIFY(TasteProfiles::encode(profile, &bytes, &error));
    QVERIFY2(quantizationViolation(profile, bytes).isEmpty(), qPrintable(quantizationViolation(profile, bytes)));

    // A supported bucket below half a hundredth rounds away entirely.
    QVERIFY(TasteProfiles::aggregate(tracksWith({{QStringLiteral("jazz"), 1000}, {QStringLiteral("rock"), 5}}),
                                     true, false, &profile, &error));
    QCOMPARE(profile.genres, (QMap<QString, double>{{QStringLiteral("jazz"), 1.0}}));
    QVERIFY(TasteProfiles::encode(profile, &bytes, &error));
    QVERIFY2(quantizationViolation(profile, bytes).isEmpty(), qPrintable(quantizationViolation(profile, bytes)));
    QVERIFY(profile.features.contains(QStringLiteral("tempo_bpm")));
    QVERIFY(profile.features.contains(QStringLiteral("energy")));

    // The v1 parser keeps accepting finer finite values.
    QVERIFY(TasteProfiles::parse(R"({"format":"muzaiten-taste-profile","version":1,)"
                                 R"("genres":{"jazz":0.8026315789473685,"rock":0.19736842105263158},)"
                                 R"("features":{"energy":{"q25":0.39999999999999997,"q50":0.4,"q75":0.41}}})",
                                 &profile, &error));
}

void TasteProfileTest::libraryScope()
{
    const QString evidence = qEnvironmentVariable("MUZAITEN_TASTE_EVIDENCE");
    QTemporaryDir temporary(evidence.isEmpty() ? QDir::tempPath() + QStringLiteral("/taste-scope-XXXXXX")
                                               : evidence + QStringLiteral("/scope-XXXXXX"));
    QVERIFY(temporary.isValid());
    temporary.setAutoRemove(evidence.isEmpty());
    const QString root = temporary.path();
    const QString libraryPath = root + QStringLiteral("/library.sqlite");
    {
        Database library(QStringLiteral("taste-scope-fixture"));
        QVERIFY(library.open(libraryPath));
        for (const auto &[directory, libraryEnabled] : {std::pair{QStringLiteral("music"), true},
                                                        std::pair{QStringLiteral("memos"), false}}) {
            QVERIFY(QDir(root).mkpath(directory));
            ScanRoot scanRoot;
            scanRoot.path = root + QLatin1Char('/') + directory;
            scanRoot.libraryEnabled = libraryEnabled;
            QVERIFY2(library.saveScanRoot(scanRoot), qPrintable(library.lastError()));
        }
        int index = 0;
        const auto add = [&](const QString &directory, const QString &genre, int count,
                             const std::function<void(Track &)> &adjust = {}) {
            QStringList paths;
            for (int i = 0; i < count; ++i, ++index) {
                Track track = libraryTrack(root + QStringLiteral("/%1/%2.flac").arg(directory).arg(index),
                                           genre, QStringLiteral("1999"), index);
                if (adjust) {
                    adjust(track);
                }
                if (!library.upsertTrack(track)) {
                    return QStringList{};
                }
                paths.append(track.path);
            }
            return paths;
        };
        QCOMPARE(add(QStringLiteral("music"), QStringLiteral("Jazz"), 10).size(), 10);
        QCOMPARE(add(QStringLiteral("memos"), QStringLiteral("Field recording"), 6).size(), 6);
        QCOMPARE(add(QStringLiteral("removed"), QStringLiteral("Polka"), 6).size(), 6);
        QCOMPARE(add(QStringLiteral("music"), QStringLiteral("Noise"), 6,
                     [](Track &track) { track.scanError = QStringLiteral("no decodable audio"); }).size(), 6);
        const QStringList guilty = add(QStringLiteral("music"), QStringLiteral("Schlager"), 6);
        const QStringList exiled = add(QStringLiteral("music"), QStringLiteral("Rock"), 5);
        QCOMPARE(guilty.size(), 6);
        QCOMPARE(exiled.size(), 5);
        for (const QString &path : guilty) {
            QVERIFY(library.setTrackFlag(path, Database::TrackFlag::NoLearn, true));
        }
        for (const QString &path : exiled) {
            QVERIFY(library.setTrackFlag(path, Database::TrackFlag::NeverRadio, true));
        }
    }
    const QByteArray before = QCryptographicHash::hash(readFile(libraryPath), QCryptographicHash::Sha256);
    TasteProfile profile;
    QString error;
    QVERIFY2(TasteProfiles::fromLibrary(libraryPath, root + QStringLiteral("/features.sqlite"), true, false,
                                        &profile, &error), qPrintable(error));
    // Library-disabled roots, rows outside every enabled root, unplayable rows
    // and "Don't learn from this" tracks stay out; "Never play on radio" stays in.
    QCOMPARE(profile.genres, (QMap<QString, double>{{QStringLiteral("jazz"), 0.67}, {QStringLiteral("rock"), 0.33}}));
    QByteArray scopeBytes;
    QVERIFY(TasteProfiles::encode(profile, &scopeBytes, &error));
    QVERIFY(writeFile(root + QStringLiteral("/scope-export.json"), scopeBytes));
    QCOMPARE(QCryptographicHash::hash(readFile(libraryPath), QCryptographicHash::Sha256), before);

    const QString outdated = root + QStringLiteral("/outdated.sqlite");
    {
        const QString connection = QStringLiteral("taste-scope-outdated");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(outdated);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY)")));
            QVERIFY(query.exec(QStringLiteral("INSERT INTO schema_migrations VALUES(1)")));
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    const QByteArray outdatedBefore = readFile(outdated);
    QVERIFY(!TasteProfiles::fromLibrary(outdated, {}, true, false, &profile, &error));
    QVERIFY2(error.contains(QStringLiteral("schema")), qPrintable(error));
    QCOMPARE(readFile(outdated), outdatedBefore);
}

void TasteProfileTest::genreLabels()
{
    // Thai lukthung carries three nonspacing marks; Devanagari sangeet carries
    // a nonspacing and a spacing combining mark; "cafe" + U+0301 is NFD.
    const QStringList accepted{QStringLiteral("ลูกทุ่ง"), QStringLiteral("संगीत"),
                               QStringLiteral("rock'n'roll"), QStringLiteral("drum\u2019n\u2019bass"),
                               QStringLiteral("children's music"), QStringLiteral("a.o.r."),
                               QStringLiteral("soundtrack (film)"), QStringLiteral("cafe\u0301 jazz")};
    TasteProfile profile;
    for (const QString &label : accepted) {
        profile.genres.insert(label, 0.125);
    }
    QByteArray bytes;
    QString error;
    QVERIFY2(TasteProfiles::encode(profile, &bytes, &error), qPrintable(error));
    TasteProfile parsed;
    QVERIFY2(TasteProfiles::parse(bytes, &parsed, &error), qPrintable(error));
    QCOMPARE(parsed.genres, profile.genres);
    for (const QString &label : {QStringLiteral("rock/pop"), QStringLiteral("https://example.test"),
                                 QStringLiteral("jazz\u0007"), QStringLiteral("<b>jazz</b>"),
                                 QStringLiteral("Jazz"), QStringLiteral("unknown")}) {
        TasteProfile single;
        single.genres.insert(label, 1.0);
        QVERIFY2(!TasteProfiles::encode(single, &bytes, &error), qPrintable(label));
    }

    // Export keeps a supported label in any of these scripts.
    QVector<TrackScorer::Candidate> tracks;
    for (int i = 0; i < 5; ++i) {
        TrackScorer::Candidate candidate;
        candidate.path = QString::number(i);
        candidate.genresFolded = {accepted.first()};
        tracks.append(candidate);
    }
    QVERIFY(TasteProfiles::aggregate(tracks, true, false, &profile, &error));
    QCOMPARE(profile.genres, (QMap<QString, double>{{accepted.first(), 1.0}}));

    // Aliases merge labels; a canonical label outside the rules drops alone.
    TasteProfile source;
    source.genres = {{QStringLiteral("bebop"), 0.3}, {QStringLiteral("hard bop"), 0.3},
                     {QStringLiteral("noise"), 0.4}};
    source.features.insert(QStringLiteral("energy"), {0.3, 0.4, 0.5});
    const QHash<QString, QString> aliases{{QStringLiteral("bebop"), QStringLiteral("jazz")},
                                          {QStringLiteral("hard bop"), QStringLiteral("jazz")},
                                          {QStringLiteral("noise"), QStringLiteral("noise/drone")}};
    const TasteProfile canonical = TasteProfiles::withAliases(source, aliases);
    QCOMPARE(canonical.genres.keys(), QStringList{QStringLiteral("jazz")});
    QVERIFY(std::abs(canonical.genres.value(QStringLiteral("jazz")) - 1.0) < 1e-12);
    QCOMPARE(canonical.features.value(QStringLiteral("energy")).q50, 0.4);
    QVERIFY2(TasteProfiles::encode(canonical, &bytes, &error), qPrintable(error));
    source.genres.remove(QStringLiteral("noise"));
    source.genres.insert(QStringLiteral("swing"), 0.4);
    QCOMPARE(TasteProfiles::withAliases(source, aliases).genres,
             (QMap<QString, double>{{QStringLiteral("jazz"), 0.6}, {QStringLiteral("swing"), 0.4}}));
    source.genres = {{QStringLiteral("noise"), 1.0}};
    const TasteProfile featureOnly = TasteProfiles::withAliases(source, aliases);
    QVERIFY(featureOnly.genres.isEmpty());
    QVERIFY2(TasteProfiles::encode(featureOnly, &bytes, &error), qPrintable(error));
    source.features.clear();
    QVERIFY(!TasteProfiles::encode(TasteProfiles::withAliases(source, aliases), &bytes, &error));
}

void TasteProfileTest::strictParsing()
{
    TasteProfile profile;
    QString error;
    const QByteArray good = R"({"format":"muzaiten-taste-profile","version":1,"genres":{"jazz":1},"features":{}})";
    QVERIFY(TasteProfiles::parse(good, &profile, &error));
    QByteArray encoded;
    QVERIFY(TasteProfiles::encode(profile, &encoded, &error));
    QVERIFY(TasteProfiles::parse(encoded, &profile, &error));
    const QList<QByteArray> invalid{
        QByteArray("{"), good.left(good.size() - 1), good + "x", QByteArray(65537, ' '),
        QByteArray(good).replace("\"version\":1", "\"version\":2"),
        QByteArray(good).replace("\"version\":1", "\"version\":\"1\""),
        QByteArray(good).replace("\"features\":{}", "\"features\":{},\"artist\":\"private\""),
        QByteArray(good).replace("\"jazz\":1", "\"jazz\":NaN"),
        QByteArray(good).replace("\"jazz\":1", "\"jazz\":1e999"),
        QByteArray(good).replace("\"jazz\":1", "\"jazz\":-1"),
        QByteArray(good).replace("\"jazz\":1", "\"jazz\":0.3"),
        QByteArray(good).replace("\"jazz\":1", "\"jazz\":1,\"j\\u0061zz\":1"),
        QByteArray(good).replace("\"jazz\":1", "\"https://example.test\":1"),
        QByteArray(good).replace("\"features\":{}", "\"features\":{\"energy\":{\"q25\":1,\"q50\":0,\"q75\":0}}"),
        QByteArray(good).replace("\"features\":{}", "\"features\":{\"energy\":{\"q25\":0,\"q50\":0.5,\"q75\":2}}"),
        QByteArray(good).replace("\"features\":{}", "\"features\":{\"energy\":{\"q25\":0,\"q50\":null,\"q75\":1}}"),
        QByteArray(good).replace("\"features\":{}", "\"features\":{\"energy\":{\"q25\":0,\"q50\":0.5,\"q75\":1,\"count\":8}}"),
        QByteArray(good).replace("\"jazz\":1", "\"jazz\":1,\"count\":1"),
        QByteArray(good).replace("\"features\":{}", "\"features\":{},\"decades\":{\"1987\":1}")};
    for (const QByteArray &bytes : invalid) {
        const auto before = profile.genres;
        QVERIFY2(!TasteProfiles::parse(bytes, &profile, &error), bytes.constData());
        QCOMPARE(profile.genres, before);
    }
    profile.features.insert(QStringLiteral("energy"), {0, std::numeric_limits<double>::quiet_NaN(), 1});
    QVERIFY(!TasteProfiles::encode(profile, &encoded, &error));
}

void TasteProfileTest::twoInstallations()
{
    const QString evidence = qEnvironmentVariable("MUZAITEN_TASTE_EVIDENCE");
    if (!evidence.isEmpty()) {
        QDir().mkpath(evidence);
    }
    QTemporaryDir temporary(evidence.isEmpty() ? QDir::tempPath() + QStringLiteral("/taste-XXXXXX")
                                               : evidence + QStringLiteral("/fixture-XXXXXX"));
    QVERIFY(temporary.isValid());
    temporary.setAutoRemove(evidence.isEmpty());
    m_evidence = evidence.isEmpty() ? QString{} : temporary.path();
    const QString a = temporary.path() + QStringLiteral("/A");
    const QString b = temporary.path() + QStringLiteral("/B");
    QVERIFY(fixture(a, true));
    QVERIFY(fixture(b, false));
    const QString exported = temporary.path() + QStringLiteral("/taste.json");
    const auto aBefore = snapshot(a);
    cli(a, {"radio-taste", "export", exported}, 1);
    QVERIFY(!QFile::exists(exported));
    const auto preview = cli(a, {"radio-taste", "export", exported, "--opt-in", "--decades", "--dry-run"});
    QVERIFY(!QFile::exists(exported));
    QCOMPARE(cli(a, {"radio-taste", "export", exported, "--opt-in", "--decades"}), preview);
    QCOMPARE(readFile(exported), preview);
    const auto aAfter = snapshot(a);
    QCOMPARE(aAfter.value(QStringLiteral("data/library.sqlite")), aBefore.value(QStringLiteral("data/library.sqlite")));
    QCOMPARE(aAfter.value(QStringLiteral("data/features.sqlite")), aBefore.value(QStringLiteral("data/features.sqlite")));
    cli(a, {"radio-taste", "export", exported}, 1);
    QCOMPARE(readFile(exported), preview);
    cli(a, {"radio-taste", "export", exported, "--opt-in", "--decades"}, 1);
    QCOMPARE(readFile(exported), preview);
    QVERIFY(writeFile(temporary.path() + QStringLiteral("/preview.json"), preview));
    TasteProfile profile;
    QString error;
    QVERIFY(TasteProfiles::read(exported, &profile, &error));
    QCOMPARE(profile.genres, (QMap<QString, double>{{QStringLiteral("jazz"), 0.8}, {QStringLiteral("rock"), 0.2}}));
    QVERIFY2(quantizationViolation(profile, preview).isEmpty(), qPrintable(quantizationViolation(profile, preview)));
    QVERIFY(!profile.genres.contains(QStringLiteral("rare genre")));
    QVERIFY(!profile.decades.contains(QStringLiteral("1970")));
    for (const QByteArray &token : {QByteArray("artist"), QByteArray("album"), QByteArray("track"), QByteArray("mbid"),
                                  QByteArray("path"), QByteArray("time"), QByteArray("count"), QByteArray("skip"),
                                  QByteArray("rating"), QByteArray("Fixture"), a.toUtf8(), b.toUtf8()}) {
        QVERIFY2(!preview.contains(token), token.constData());
    }
    const auto bBefore = snapshot(b);
    cli(b, {"radio-taste", "import", exported, "--name", "Source taste", "--dry-run"});
    QCOMPARE(snapshot(b), bBefore);
    cli(b, {"radio-taste", "import", exported, "--name", "Source taste"});
    const auto bImported = snapshot(b);
    QJsonObject hashes;
    for (auto it = bImported.begin(); it != bImported.end(); ++it) {
        hashes.insert(it.key(), QString::fromLatin1(QCryptographicHash::hash(it.value(), QCryptographicHash::Sha256).toHex()));
        const QString baselinePath = temporary.path() + QStringLiteral("/B-before-rejections/") + it.key();
        QVERIFY(QDir().mkpath(QFileInfo(baselinePath).absolutePath()));
        QVERIFY(writeFile(baselinePath, it.value()));
    }
    QVERIFY(writeFile(temporary.path() + QStringLiteral("/B-unchanged-on-failure-sha256.json"), QJsonDocument(hashes).toJson()));
    cli(b, {"radio-taste", "import", exported, "--name", "Source taste"}, 1);
    QCOMPARE(snapshot(b), bImported);
    int badIndex = 0;
    for (QByteArray bytes : {
        QByteArray(preview).replace("\"version\": 1", "\"version\": 2"),
        QByteArray(preview).replace("\"genres\":", "\"artist\": 1, \"genres\":"),
        QByteArray(preview).replace("\"q50\": 0.4", "\"q50\": NaN"),
        QByteArray(preview).replace("\"q50\": 0.4", "\"q50\": 1e999"),
        QByteArray(preview).replace("\"genres\":", "\"count\": 77, \"genres\":"),
        QByteArray(preview).replace("\"genres\":", "\"decode_hash\": \"recording\", \"genres\":"),
        QByteArray(preview).replace("\"version\": 1", "\"version\": 1, \"vers\\u0069on\": 1"),
        QByteArray(preview).replace("\"jazz\": 0.8", "\"jazz\": 0.7"),
        QByteArray(preview).replace("jazz", QByteArray(1, char(0xff))),
        preview.left(preview.size() - 4), QByteArray(65537, ' ')}) {
        QVERIFY(bytes != preview);
        const QString bad = temporary.path() + QStringLiteral("/bad-%1.json").arg(++badIndex);
        QVERIFY(writeFile(bad, bytes));
        cli(b, {"radio-taste", "import", bad, "--name", "Source taste", "--replace"}, 1);
        QCOMPARE(snapshot(b), bImported);
    }
    qputenv("MUZAITEN_STATE_ROOT", b.toUtf8());
    QMap<QString, TasteProfile> seeds;
    QVERIFY(TasteProfiles::load(&seeds, &error));
    QVERIFY(seeds.contains(QStringLiteral("Source taste")));
    Database db(QStringLiteral("taste-B-consumer"));
    QVERIFY(db.open(b + QStringLiteral("/data/library.sqlite")));
    const auto pool = poolFor(db, b + QStringLiteral("/data/features.sqlite"));
    QCOMPARE(pool.size(), 200);
    QRandomGenerator baselineRng(1701), seededRng(1701), absentRng(1701);
    RadioSession baseline(pool, {}, {{"jazz", 1.6}, {"rock", 0.3}}, 30, 100000, &baselineRng);
    RadioSession seeded(pool, {}, {{"jazz", 1.6}, {"rock", 0.3}}, 30, 100000, &seededRng);
    seeded.setTasteProfile(QStringLiteral("Source taste"), seeds.value(QStringLiteral("Source taste")));
    RadioSession absent(pool, {}, {{"jazz", 1.6}, {"rock", 0.3}}, 30, 100000, &absentRng);
    TasteProfile absentProfile;
    absentProfile.genres.insert(QStringLiteral("cumbia"), 1.0);
    absent.setTasteProfile(QStringLiteral("Absent genre"), absentProfile);
    int baselineJazz = 0, seededJazz = 0;
    QJsonArray baselinePicks, seededPicks, absentPicks;
    for (int i = 0; i < 40; ++i) {
        const auto resolve = [&db](const QString &path) { return db.trackForPath(path); };
        const auto baselinePick = baseline.nextTracks(1, {}, resolve);
        const auto seededPick = seeded.nextTracks(1, {}, resolve);
        const auto absentPick = absent.nextTracks(1, {}, resolve);
        QCOMPARE(baselinePick.size(), 1);
        QCOMPARE(seededPick.size(), 1);
        QCOMPARE(absentPick.size(), 1);
        QCOMPARE(absentPick[0].path, baselinePick[0].path);
        const QString baselineGenre = db.radioCandidatesForPaths({baselinePick[0].path})[0].genresFolded[0];
        const QString seededGenre = db.radioCandidatesForPaths({seededPick[0].path})[0].genresFolded[0];
        baselineJazz += baselineGenre == QLatin1String("jazz");
        seededJazz += seededGenre == QLatin1String("jazz");
        QVERIFY(seeded.reasonFor(seededPick[0].path).contains(QStringLiteral("Source taste")));
        QVERIFY(ReasonText::sentence(seeded.reasonComponentsFor(seededPick[0].path)).contains(QStringLiteral("Source taste")));
        QJsonArray components;
        bool tasteComponent = false;
        for (const auto &component : seeded.reasonComponentsFor(seededPick[0].path)) {
            components.append(QJsonObject{{"name", component.name}, {"value", component.value}});
            if (component.name == QLatin1String("taste seed: Source taste")) {
                tasteComponent = true;
                QVERIFY(component.value >= 0.0 && component.value <= TasteProfiles::MaximumScore);
            }
        }
        QVERIFY(tasteComponent);
        for (const auto &component : absent.reasonComponentsFor(absentPick[0].path)) {
            if (component.name == QLatin1String("taste seed: Absent genre")) {
                QCOMPARE(component.value, 0.0);
            }
        }
        baselinePicks.append(QJsonObject{{"path", baselinePick[0].path}, {"genre", baselineGenre}});
        seededPicks.append(QJsonObject{{"path", seededPick[0].path}, {"genre", seededGenre},
                                      {"reason", seeded.reasonFor(seededPick[0].path)}, {"components", components}});
        absentPicks.append(QJsonObject{{"path", absentPick[0].path}, {"genre", baselineGenre}});
        baseline.notePlayed(baselinePick[0]);
        seeded.notePlayed(seededPick[0]);
        absent.notePlayed(absentPick[0]);
    }
    qInfo("Fixed seed 1701, 40 picks: baseline jazz %d, taste-seeded jazz %d", baselineJazz, seededJazz);
    QVERIFY(seededJazz >= baselineJazz + 8);
    QVERIFY(writeFile(temporary.path() + QStringLiteral("/picks.json"), QJsonDocument(QJsonObject{
        {"rng_seed", 1701}, {"pick_total", 40}, {"baseline_jazz", baselineJazz}, {"seeded_jazz", seededJazz},
        {"baseline", baselinePicks}, {"seeded", seededPicks}, {"absent_genre", absentPicks}}).toJson()));
    cli(b, {"radio-taste", "remove", "Source taste"});
    QVERIFY(TasteProfiles::load(&seeds, &error));
    QVERIFY(seeds.isEmpty());
    QVERIFY(!seeded.tasteProfileName().isEmpty());
    seeded.setTasteProfile({}, {});
    QVERIFY(seeded.tasteProfileName().isEmpty());
    m_evidence.clear();
}

void TasteProfileTest::seedStoreLimits()
{
    const QString evidence = qEnvironmentVariable("MUZAITEN_TASTE_EVIDENCE");
    QTemporaryDir temporary(evidence.isEmpty() ? QDir::tempPath() + QStringLiteral("/taste-store-XXXXXX")
                                               : evidence + QStringLiteral("/store-XXXXXX"));
    QVERIFY(temporary.isValid());
    temporary.setAutoRemove(evidence.isEmpty());
    const QString root = temporary.path();
    qputenv("MUZAITEN_STATE_ROOT", root.toUtf8());
    const QString store = TasteProfiles::storagePath();
    QVERIFY(QDir().mkpath(QFileInfo(store).absolutePath()));

    QByteArray encoded;
    QString error;
    QVERIFY2(TasteProfiles::encode(nearLimitProfile(76), &encoded, &error), qPrintable(error));
    QVERIFY(encoded.size() > 60 * 1024 && encoded.size() <= TasteProfiles::MaximumBytes);
    const QString profilePath = root + QStringLiteral("/near-limit.json");
    QVERIFY(writeFile(profilePath, encoded));

    // Every import of a valid seed exits 0, and list, remove and further imports
    // keep working with 31 or 32 near-limit profiles in the store.
    for (int i = 1; i <= 31; ++i) {
        cli(root, {"radio-taste", "import", profilePath, "--name", seedName(i)});
    }
    QCOMPARE(seedNames(cli(root, {"radio-taste", "list"})).size(), 31);
    cli(root, {"radio-taste", "import", profilePath, "--name", seedName(32)});
    const QByteArray full = readFile(store);
    QVERIFY(full.size() <= 4 * 1024 * 1024);
    QCOMPARE(seedNames(cli(root, {"radio-taste", "list"})).size(), 32);

    // A 33rd seed is refused before the store is written.
    const QByteArray digest = QCryptographicHash::hash(full, QCryptographicHash::Sha256);
    cli(root, {"radio-taste", "import", profilePath, "--name", seedName(33)}, 1);
    QCOMPARE(QCryptographicHash::hash(readFile(store), QCryptographicHash::Sha256), digest);
    cli(root, {"radio-taste", "remove", seedName(5)});
    QCOMPARE(seedNames(cli(root, {"radio-taste", "list"})).size(), 31);

    // Exchange files refused by size, syntax or the normalized bound leave the store unchanged.
    const QByteArray before = readFile(store);
    QByteArray oversized = encoded;
    oversized.append(QByteArray(TasteProfiles::MaximumBytes + 1 - encoded.size(), ' '));
    QVERIFY(writeFile(root + QStringLiteral("/oversized.json"), oversized));
    cli(root, {"radio-taste", "import", root + QStringLiteral("/oversized.json"), "--name", seedName(6)}, 1);
    QVERIFY(writeFile(root + QStringLiteral("/truncated.json"), encoded.left(encoded.size() - 8)));
    cli(root, {"radio-taste", "import", root + QStringLiteral("/truncated.json"), "--name", seedName(6)}, 1);
    // Compact input within the bound whose normalized form exceeds it is refused
    // by parse(), so reading and encoding agree.
    const QByteArray dense = compactExchange(nearLimitProfile(77));
    QVERIFY(dense.size() <= TasteProfiles::MaximumBytes);
    TasteProfile parsed;
    QVERIFY(!TasteProfiles::parse(dense, &parsed, &error));
    QVERIFY(writeFile(root + QStringLiteral("/dense.json"), dense));
    cli(root, {"radio-taste", "import", root + QStringLiteral("/dense.json"), "--name", seedName(6)}, 1);
    QCOMPARE(readFile(store), before);
    QCOMPARE(seedNames(cli(root, {"radio-taste", "list"})).size(), 31);
}

void TasteProfileTest::legacySeedStore()
{
    const QString evidence = qEnvironmentVariable("MUZAITEN_TASTE_EVIDENCE");
    QTemporaryDir temporary(evidence.isEmpty() ? QDir::tempPath() + QStringLiteral("/taste-legacy-XXXXXX")
                                               : evidence + QStringLiteral("/legacy-XXXXXX"));
    QVERIFY(temporary.isValid());
    temporary.setAutoRemove(evidence.isEmpty());
    const QString root = temporary.path();
    qputenv("MUZAITEN_STATE_ROOT", root.toUtf8());
    const QString store = TasteProfiles::storagePath();
    QVERIFY(QDir().mkpath(QFileInfo(store).absolutePath()));

    // An indented store larger than 2 MiB stays listable and removable.
    const QByteArray legacy = indentedSeedStore(nearLimitProfile(76), 31);
    QVERIFY(legacy.size() > 2 * 1024 * 1024);
    QVERIFY(writeFile(store, legacy));
    QCOMPARE(seedNames(cli(root, {"radio-taste", "list"})).size(), 31);
    cli(root, {"radio-taste", "remove", seedName(1)});
    QCOMPARE(seedNames(cli(root, {"radio-taste", "list"})).size(), 30);

    // The read bound is inclusive at 4 MiB; one byte more is refused without change.
    QByteArray atBound = readFile(store);
    atBound.append(QByteArray(4 * 1024 * 1024 - atBound.size(), ' '));
    QVERIFY(writeFile(store, atBound));
    QCOMPARE(seedNames(cli(root, {"radio-taste", "list"})).size(), 30);
    QByteArray beyond = atBound;
    beyond.append(' ');
    QVERIFY(writeFile(store, beyond));
    cli(root, {"radio-taste", "list"}, 1);
    cli(root, {"radio-taste", "remove", seedName(2)}, 1);
    QCOMPARE(readFile(store), beyond);
}

void TasteProfileTest::constraintsAndSessionSnapshots()
{
    TasteProfile profile;
    profile.genres.insert(QStringLiteral("jazz"), 1.0);
    QVector<TrackScorer::Candidate> pool;
    for (int i = 0; i < 12; ++i) {
        TrackScorer::Candidate candidate;
        candidate.path = QString::number(i);
        candidate.songKey = i == 11 ? QStringLiteral("0") : candidate.path;
        candidate.artistFolded = QString::number(i % 4);
        candidate.albumKey = QString::number(i % 2);
        candidate.genresFolded = {QStringLiteral("jazz")};
        pool.append(candidate);
    }
    pool = RadioFilters::excludeFlaggedCandidates(pool, {"7"});
    QRandomGenerator rng(77);
    RadioSession session(pool, {}, {}, 30, 100, &rng);
    session.setTasteProfile(QStringLiteral("Snapshot seed"), profile);
    auto copy = session;
    const auto resolve = [](const QString &path) { Track track; track.path = path; return track; };
    const auto picks = copy.nextTracks(12, {"0"}, resolve);
    QVERIFY(picks.size() <= 6);
    QMap<QString, int> albums;
    for (int i = 0; i < picks.size(); ++i) {
        const auto candidate = *std::find_if(pool.begin(), pool.end(), [&](const auto &row) { return row.path == picks[i].path; });
        QVERIFY(candidate.path != QLatin1String("0") && candidate.path != QLatin1String("11")
            && candidate.path != QLatin1String("7"));
        QVERIFY(++albums[candidate.albumKey] <= 3);
        for (int previous = std::max(0, i - 3); previous < i; ++previous) {
            const auto prior = std::find_if(pool.begin(), pool.end(), [&](const auto &row) { return row.path == picks[previous].path; });
            QVERIFY(prior->artistFolded != candidate.artistFolded);
        }
    }
    RadioSession restored(pool, {}, {}, 30, 100, &rng);
    restored.restoreConstraintState(session.constraintState());
    QCOMPARE(restored.tasteProfileName(), session.tasteProfileName());
    QCOMPARE(restored.tasteProfileBytes(), session.tasteProfileBytes());
    const auto restoredPicks = restored.nextTracks(1, {}, resolve);
    QCOMPARE(restoredPicks.size(), 1);
    QVERIFY(restored.reasonFor(restoredPicks[0].path).contains(QStringLiteral("Snapshot seed")));
    auto oldState = session.constraintState();
    oldState.remove(QStringLiteral("tasteSeed"));
    RadioSession oldSession(pool, {}, {}, 30, 100, &rng);
    oldSession.restoreConstraintState(oldState);
    QVERIFY(oldSession.tasteProfileName().isEmpty());
    QCOMPARE(oldSession.nextTracks(1, {}, resolve).size(), 1);
}

QTEST_GUILESS_MAIN(TasteProfileTest)
#include "test_taste_profile.moc"
