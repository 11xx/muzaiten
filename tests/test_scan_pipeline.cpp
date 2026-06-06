#include "db/Database.h"
#include "scanner/ScanPipeline.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

// Integration test for the fast-first-pass scan: a real ScanPipeline drives a
// real Database over hand-written WAV files (no encoder dependency), exercising
// the deferred-placeholder partition, the fill-mode tag read, bit-depth
// extraction, browse isolation, and the incremental-rescan diff.
class ScanPipelineTest final : public QObject {
    Q_OBJECT

private slots:
    void fastFirstPassDefersThenFills();

private:
    static void writeWav(const QString &path, int sampleRate, int bitsPerSample, int channels, int frames);
};

namespace {

void appendLe16(QByteArray &b, quint16 v)
{
    b.append(static_cast<char>(v & 0xff));
    b.append(static_cast<char>((v >> 8) & 0xff));
}

void appendLe32(QByteArray &b, quint32 v)
{
    b.append(static_cast<char>(v & 0xff));
    b.append(static_cast<char>((v >> 8) & 0xff));
    b.append(static_cast<char>((v >> 16) & 0xff));
    b.append(static_cast<char>((v >> 24) & 0xff));
}

// Drive a synchronous scan, collecting placeholders + ingested tracks via direct
// (same-thread) signal connections, and upserting batches into the database.
struct ScanRun {
    int placeholderCount = 0;
    int batchTrackCount = 0;
};

ScanRun runPipeline(ScanPipeline &pipeline, Database &database, bool ingestPlaceholders)
{
    ScanRun run;
    QObject::connect(&pipeline, &ScanPipeline::enumeratedReady,
                     [&](const QVector<Track> &placeholders) {
                         run.placeholderCount += placeholders.size();
                         if (ingestPlaceholders) {
                             database.insertEnumeratedPlaceholders(placeholders);
                         }
                     });
    QObject::connect(&pipeline, &ScanPipeline::batchReady,
                     [&](const QVector<Track> &tracks) {
                         run.batchTrackCount += tracks.size();
                         database.beginTransaction();
                         for (const Track &track : tracks) {
                             database.upsertTrack(track);
                         }
                         database.commitTransaction();
                     });
    pipeline.run();  // synchronous; direct connections fire inline
    return run;
}

} // namespace

void ScanPipelineTest::writeWav(const QString &path, int sampleRate, int bitsPerSample, int channels, int frames)
{
    const int bytesPerSample = bitsPerSample / 8;
    const int dataSize = frames * channels * bytesPerSample;
    const int byteRate = sampleRate * channels * bytesPerSample;

    QByteArray wav;
    wav.append("RIFF");
    appendLe32(wav, static_cast<quint32>(36 + dataSize));
    wav.append("WAVE");
    wav.append("fmt ");
    appendLe32(wav, 16);                                              // PCM fmt chunk size
    appendLe16(wav, 1);                                              // PCM
    appendLe16(wav, static_cast<quint16>(channels));
    appendLe32(wav, static_cast<quint32>(sampleRate));
    appendLe32(wav, static_cast<quint32>(byteRate));
    appendLe16(wav, static_cast<quint16>(channels * bytesPerSample)); // block align
    appendLe16(wav, static_cast<quint16>(bitsPerSample));
    wav.append("data");
    appendLe32(wav, static_cast<quint32>(dataSize));
    wav.append(QByteArray(dataSize, 0));

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(wav), static_cast<qint64>(wav.size()));
    file.close();
}

void ScanPipelineTest::fastFirstPassDefersThenFills()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    const QString musicRoot = temp.filePath(QStringLiteral("Music"));
    const QString albumDir = musicRoot + QStringLiteral("/Album");
    QVERIFY(QDir().mkpath(albumDir));
    const QString wav16 = albumDir + QStringLiteral("/01 a.wav");
    const QString wav24 = albumDir + QStringLiteral("/02 b.wav");
    writeWav(wav16, 44100, 16, 2, 64);
    writeWav(wav24, 48000, 24, 2, 64);

    const QString connectionName = QStringLiteral("scan-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    Database database(connectionName);
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    // --- Foreground scan: new files become placeholders, tag read deferred. ---
    ScanPipeline::Options options;
    options.lowPriority = false;
    {
        ScanPipeline scan(musicRoot, 0, database.trackFingerprints(), options);
        const ScanRun run = runPipeline(scan, database, /*ingestPlaceholders=*/true);
        QCOMPARE(run.placeholderCount, 2);   // both files are new
        QCOMPARE(run.batchTrackCount, 0);    // nothing changed -> no foreground read
    }

    // Visible in the directory view, isolated from the artist/album browse.
    QCOMPARE(database.tracksForDirectory(albumDir).size(), 2);
    QVERIFY(database.albumArtists().isEmpty());
    QVERIFY(database.allTracksForSearch().isEmpty());
    QCOMPARE(database.enumeratedOnlyPaths().size(), 2);
    QCOMPARE(database.enumeratedOnlyPaths(albumDir).size(), 2);
    QVERIFY(!database.trackFingerprints().value(wav16).metadataScanned);

    // --- Background fill: read tags for the placeholder backlog. ---
    {
        const QStringList pending = database.enumeratedOnlyPaths();
        ScanPipeline fill(albumDir, pending, options);
        const ScanRun run = runPipeline(fill, database, /*ingestPlaceholders=*/false);
        QCOMPARE(run.batchTrackCount, 2);
    }

    // Now in the artist/album browse with bit depth populated, no backlog left.
    QCOMPARE(database.albumArtists().size(), 1);
    QVERIFY(database.enumeratedOnlyPaths().isEmpty());
    QVERIFY(database.trackFingerprints().value(wav16).metadataScanned);
    const QVector<Search::SearchRecord> records = database.allTracksForSearch();
    QCOMPARE(records.size(), 2);
    QList<int> depths;
    for (const Search::SearchRecord &record : records) {
        depths.append(record.bitDepth);
    }
    std::sort(depths.begin(), depths.end());
    QCOMPARE(depths, (QList<int>{16, 24}));

    // --- Rescan: unchanged + scanned -> all skipped, nothing re-read. ---
    {
        ScanPipeline rescan(musicRoot, 0, database.trackFingerprints(), options);
        const ScanRun run = runPipeline(rescan, database, /*ingestPlaceholders=*/true);
        QCOMPARE(run.placeholderCount, 0);
        QCOMPARE(run.batchTrackCount, 0);
    }
}

QTEST_MAIN(ScanPipelineTest)
#include "test_scan_pipeline.moc"
