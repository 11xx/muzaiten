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
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <limits>

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
        files.insert(QDir(root).relativeFilePath(path), QCryptographicHash::hash(readFile(path), QCryptographicHash::Sha256));
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

} // namespace

class TasteProfileTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void supportAndScoreBounds();
    void strictParsing();
    void twoInstallations();
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
            Track track;
            track.path = root + QStringLiteral("/synthetic/%1.flac").arg(i, 4, 10, QLatin1Char('0'));
            track.parentDir = QFileInfo(track.path).absolutePath();
            track.filename = QFileInfo(track.path).fileName();
            track.title = QStringLiteral("Fixture title %1").arg(i);
            track.artistName = QStringLiteral("Fixture artist %1").arg(i);
            track.albumArtistName = track.artistName;
            track.albumTitle = QStringLiteral("Fixture album %1").arg(i);
            track.date = source && i == 76 ? QStringLiteral("1971") : jazz ? QStringLiteral("1987") : QStringLiteral("2005");
            track.durationMs = 180000;
            track.codec = QStringLiteral("flac");
            MetadataBlob::FullMetadata metadata;
            metadata.tags.insert(QStringLiteral("GENRE"), {genre});
            const auto encoded = MetadataBlob::encode(metadata);
            track.fullMetadataBlob = encoded.data;
            track.fullMetadataRawSize = encoded.rawSize;
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
    QVERIFY(profile.genres.value(QStringLiteral("jazz")) > 0.8);
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
    cli(b, {"radio-taste", "import", exported, "--name", "Source taste"}, 1);
    QCOMPARE(snapshot(b), bImported);
    int badIndex = 0;
    for (QByteArray bytes : {
        QByteArray(preview).replace("\"version\": 1", "\"version\": 2"),
        QByteArray(preview).replace("\"genres\":", "\"artist\": 1, \"genres\":"),
        QByteArray(preview).replace("0.39", "NaN"),
        QByteArray(preview).replace("0.39", "1e999"),
        preview.left(preview.size() - 4), QByteArray(65537, ' ')}) {
        if (bytes == preview) {
            bytes = "NaN";
        }
        const QString bad = temporary.path() + QStringLiteral("/bad-%1.json").arg(++badIndex);
        QVERIFY(writeFile(bad, bytes));
        cli(b, {"radio-taste", "import", bad, "--name", "Source taste", "--replace"}, 1);
        QCOMPARE(snapshot(b), bImported);
    }
    QJsonObject hashes;
    for (auto it = bImported.begin(); it != bImported.end(); ++it) {
        hashes.insert(it.key(), QString::fromLatin1(it.value().toHex()));
    }
    QVERIFY(writeFile(temporary.path() + QStringLiteral("/B-unchanged-on-failure-sha256.json"), QJsonDocument(hashes).toJson()));
    qputenv("MUZAITEN_STATE_ROOT", b.toUtf8());
    QMap<QString, TasteProfile> seeds;
    QVERIFY(TasteProfiles::load(&seeds, &error));
    QVERIFY(seeds.contains(QStringLiteral("Source taste")));
    Database db(QStringLiteral("taste-B-consumer"));
    QVERIFY(db.open(b + QStringLiteral("/data/library.sqlite")));
    const auto pool = poolFor(db, b + QStringLiteral("/data/features.sqlite"));
    QCOMPARE(pool.size(), 200);
    QRandomGenerator baselineRng(1701), seededRng(1701);
    RadioSession baseline(pool, {}, {{"jazz", 1.6}, {"rock", 0.3}}, 30, 100000, &baselineRng);
    RadioSession seeded(pool, {}, {{"jazz", 1.6}, {"rock", 0.3}}, 30, 100000, &seededRng);
    seeded.setTasteProfile(QStringLiteral("Source taste"), seeds.value(QStringLiteral("Source taste")));
    int baselineJazz = 0, seededJazz = 0;
    QJsonArray baselinePicks, seededPicks;
    for (int i = 0; i < 40; ++i) {
        const auto resolve = [&db](const QString &path) { return db.trackForPath(path); };
        const auto baselinePick = baseline.nextTracks(1, {}, resolve);
        const auto seededPick = seeded.nextTracks(1, {}, resolve);
        QCOMPARE(baselinePick.size(), 1);
        QCOMPARE(seededPick.size(), 1);
        const QString baselineGenre = db.radioCandidatesForPaths({baselinePick[0].path})[0].genresFolded[0];
        const QString seededGenre = db.radioCandidatesForPaths({seededPick[0].path})[0].genresFolded[0];
        baselineJazz += baselineGenre == QLatin1String("jazz");
        seededJazz += seededGenre == QLatin1String("jazz");
        QVERIFY(seeded.reasonFor(seededPick[0].path).contains(QStringLiteral("Source taste")));
        QVERIFY(ReasonText::sentence(seeded.reasonComponentsFor(seededPick[0].path)).contains(QStringLiteral("Source taste")));
        baselinePicks.append(QJsonObject{{"path", baselinePick[0].path}, {"genre", baselineGenre}});
        seededPicks.append(QJsonObject{{"path", seededPick[0].path}, {"genre", seededGenre},
                                      {"reason", seeded.reasonFor(seededPick[0].path)}});
        baseline.notePlayed(baselinePick[0]);
        seeded.notePlayed(seededPick[0]);
    }
    qInfo("Fixed seed 1701, 40 picks: baseline jazz %d, taste-seeded jazz %d", baselineJazz, seededJazz);
    QVERIFY(seededJazz >= baselineJazz + 8);
    QVERIFY(writeFile(temporary.path() + QStringLiteral("/picks.json"), QJsonDocument(QJsonObject{
        {"rng_seed", 1701}, {"pick_total", 40}, {"baseline_jazz", baselineJazz}, {"seeded_jazz", seededJazz},
        {"baseline", baselinePicks}, {"seeded", seededPicks}}).toJson()));
    cli(b, {"radio-taste", "remove", "Source taste"});
    QVERIFY(TasteProfiles::load(&seeds, &error));
    QVERIFY(seeds.isEmpty());
    QVERIFY(!seeded.tasteProfileName().isEmpty());
    seeded.setTasteProfile({}, {});
    QVERIFY(seeded.tasteProfileName().isEmpty());
    m_evidence.clear();
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
