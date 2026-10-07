#include "search/IndexCache.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace Search;

class TestIndexCache : public QObject {
    Q_OBJECT
private:
    static CacheSignature signature()
    {
        return {IndexCache::kFormatVersion, 7, 19, QStringLiteral("db"),
                QStringLiteral("/isolated/library.sqlite"), 42};
    }

    static SearchRecord record(int i)
    {
        SearchRecord r;
        r.title = QStringLiteral("Title %1").arg(i);
        r.artistName = QStringLiteral("Artist");
        r.albumArtistName = QStringLiteral("Album artist");
        r.albumTitle = QStringLiteral("Album");
        r.date = QStringLiteral("2026");
        r.filename = QStringLiteral("track-%1.flac").arg(i);
        r.path = QStringLiteral("/music/") + r.filename;
        r.normTitle = QStringLiteral("title %1").arg(i);
        r.normArtist = QStringLiteral("artist");
        r.normAlbumArtist = QStringLiteral("album artist");
        r.normAlbum = QStringLiteral("album");
        r.normFilename = r.filename;
        r.normPath = r.path;
        r.durationMs = 123456 + i;
        r.rating0To100 = i;
        r.sampleRateHz = 48000;
        r.bitrateKbps = 900;
        r.channels = 2;
        r.bitDepth = 24;
        r.codec = QStringLiteral("flac");
        r.trackNumber = i + 1;
        r.discNumber = 2;
        r.fileMtime = 100000 + i;
        r.fileSize = 500000 + i;
        r.source = i % 2 ? TrackSource::Mpd : TrackSource::Local;
        return r;
    }

    static void equal(const SearchRecord &a, const SearchRecord &b)
    {
#define FIELD(name) QCOMPARE(a.name, b.name)
        FIELD(title); FIELD(artistName); FIELD(albumArtistName); FIELD(albumTitle);
        FIELD(date); FIELD(filename); FIELD(path); FIELD(normTitle); FIELD(normArtist);
        FIELD(normAlbumArtist); FIELD(normAlbum); FIELD(normFilename); FIELD(normPath);
        FIELD(durationMs); FIELD(rating0To100); FIELD(sampleRateHz); FIELD(bitrateKbps);
        FIELD(channels); FIELD(bitDepth); FIELD(codec); FIELD(trackNumber); FIELD(discNumber);
        FIELD(fileMtime); FIELD(fileSize); FIELD(source);
#undef FIELD
    }

    static QByteArray bytes(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }

    // The raw size of every chunk in a cache file, in order.
    static QVector<quint64> chunkRawSizes(const QString &path)
    {
        QDataStream s(bytes(path));
        s.setVersion(QDataStream::Qt_6_0);
        quint32 magic, version, fold;
        qint32 schema;
        QString id, dbPath;
        qint64 revision;
        quint64 total;
        s >> magic >> version >> fold >> schema >> id >> dbPath >> revision >> total;
        QVector<quint64> sizes;
        while (s.status() == QDataStream::Ok) {
            quint32 count = 0;
            s >> count;
            if (count == 0) break;
            quint64 raw = 0, compressed = 0;
            s >> raw >> compressed;
            s.skipRawData(static_cast<int>(compressed));
            sizes.push_back(raw);
        }
        return sizes;
    }

    static bool replace(const QString &path, const QByteArray &data)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            && file.write(data) == data.size();
    }

private slots:
    void stagedWritePublishesOnlyWhenAccepted()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("index.cache"));
        QVERIFY(IndexCache::write(path, signature(), {record(0)}));
        const auto original = bytes(path);
        {
            auto discarded = IndexCache::prepare(path, signature(), {record(1)});
            QVERIFY(discarded);
            QCOMPARE(bytes(path), original);
        }
        QCOMPARE(bytes(path), original);
        QCOMPARE(QDir(temp.path()).entryList(QDir::Files).size(), 1);
        auto accepted = IndexCache::prepare(path, signature(), {record(2)}, {}, {1});
        QVERIFY(accepted);
        QCOMPARE(bytes(path), original);
        QVERIFY(accepted->publish());
        QVERIFY(!accepted->publish());
        const auto loaded = IndexCache::read(path);
        QVERIFY(loaded.ok);
        QCOMPARE(loaded.records.size(), 1);
        equal(loaded.records.first(), record(2));
        QCOMPARE(QDir(temp.path()).entryList(QDir::Files).size(), 1);
    }

    void canceledPreparationPreservesPublishedCache()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("index.cache"));
        QVERIFY(IndexCache::write(path, signature(), {record(0)}));
        const auto original = bytes(path);
        int checks = 0;
        auto canceled = IndexCache::prepare(path, signature(), {record(1), record(2)},
                                           [&checks] { return ++checks >= 3; }, {1});
        QVERIFY(!canceled);
        QCOMPARE(bytes(path), original);
        QCOMPARE(QDir(temp.path()).entryList(QDir::Files).size(), 1);
    }

    void roundTrip_data()
    {
        QTest::addColumn<int>("count");
        QTest::newRow("empty") << 0;
        QTest::newRow("partial") << 7;
        QTest::newRow("exact-multiple") << 6;
    }

    void roundTrip()
    {
        QFETCH(int, count);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("index.cache"));
        QVector<SearchRecord> original;
        for (int i = 0; i < count; ++i) original.push_back(record(i));
        QVERIFY(IndexCache::write(path, signature(), original, {3}));
        QVector<SearchRecord> streamed;
        CacheSignature stored;
        QVERIFY(IndexCache::forEachRecord(path, &stored,
            [&streamed](SearchRecord r) { streamed.push_back(std::move(r)); }, &signatureValue));
        QCOMPARE(stored, signature());
        QCOMPARE(streamed.size(), original.size());
        for (int i = 0; i < count; ++i) equal(streamed[i], original[i]);
        const auto loaded = IndexCache::read(path);
        QVERIFY(loaded.ok);
        QCOMPARE(loaded.signature, signature());
        QCOMPARE(loaded.records.size(), original.size());
        for (int i = 0; i < count; ++i) equal(loaded.records[i], original[i]);
    }

    // Large records close a chunk by size before the record count is reached,
    // so no chunk grows toward the reader's per-chunk limit.
    void largeRecordsSplitBySize()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("large.cache"));
        QVector<SearchRecord> original;
        for (int i = 0; i < 3; ++i) {
            SearchRecord r = record(i);
            r.title = QString(3'000'000, QChar(u'a' + i));
            r.normTitle = r.title;
            original.push_back(r);
        }
        QVERIFY(IndexCache::write(path, signature(), original));
        // Each record serializes to about 12 MB, so the first chunk must close
        // after one record although the default count limit is far higher.
        QDataStream s(bytes(path));
        s.setVersion(QDataStream::Qt_6_0);
        quint32 magic, version, fold, firstCount;
        qint32 schema;
        QString id, dbPath;
        qint64 revision;
        quint64 total;
        s >> magic >> version >> fold >> schema >> id >> dbPath >> revision >> total >> firstCount;
        QCOMPARE(s.status(), QDataStream::Ok);
        QCOMPARE(total, quint64(3));
        QCOMPARE(firstCount, quint32(1));
        const auto loaded = IndexCache::read(path);
        QVERIFY(loaded.ok);
        QCOMPARE(loaded.records.size(), original.size());
        for (qsizetype i = 0; i < original.size(); ++i) equal(loaded.records[i], original[i]);
    }

    // A record that fits a chunk alone but not beside the previous one starts
    // a new chunk; only a record larger than a whole chunk is refused.
    void recordsSplitBeforeOverflow()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("split.cache"));
        const IndexCache::ChunkLimits limits{4096, 100'000, 100'000};
        QVector<SearchRecord> records{record(0), record(1)};
        records[0].title = QString(20'000, QLatin1Char('a'));
        records[1].title = QString(35'000, QLatin1Char('b'));
        QVERIFY(IndexCache::write(path, signature(), records, limits));
        const QVector<quint64> sizes = chunkRawSizes(path);
        QCOMPARE(sizes.size(), 2);
        for (const quint64 size : sizes) QVERIFY(size <= limits.maxBytes);
        const auto loaded = IndexCache::read(path);
        QVERIFY(loaded.ok);
        QCOMPARE(loaded.records.size(), records.size());
        for (qsizetype i = 0; i < records.size(); ++i) equal(loaded.records[i], records[i]);

        records.push_back(record(2));
        records.back().title = QString(60'000, QLatin1Char('c'));
        QVERIFY(!IndexCache::write(path, signature(), records, limits));
    }

    // The header's identity strings are bounded before they are decoded.
    void oversizedHeaderStringRejected()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("header.cache"));
        CacheSignature sig = signature();
        sig.databaseId = QString(40'000, QLatin1Char('x'));
        QVERIFY(!IndexCache::write(path, sig, {record(0)}));

        QByteArray data;
        {
            QDataStream out(&data, QIODevice::WriteOnly);
            out.setVersion(QDataStream::Qt_6_0);
            out << quint32(0x4D5A5349) << sig.formatVersion << sig.foldVersion << sig.schemaVersion
                << sig.databaseId << sig.databasePath << sig.contentRevision
                << quint64(0) << quint32(0);
        }
        QVERIFY(replace(path, data));
        QVERIFY(!IndexCache::read(path).ok);
    }

    void corruption_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char *name : {"truncated-chunk", "oversized-raw", "wrong-total",
                                 "missing-end", "trailing", "format-2"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void corruption()
    {
        QFETCH(QString, kind);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("index.cache"));
        QVector<SearchRecord> records;
        for (int i = 0; i < 5; ++i) records.push_back(record(i));
        QVERIFY(IndexCache::write(path, signature(), records, {2}));
        QByteArray data = bytes(path);
        QVERIFY(!data.isEmpty());
        QDataStream s(data);
        s.setVersion(QDataStream::Qt_6_0);
        quint32 magic, version, fold;
        qint32 schema;
        QString id, dbPath;
        qint64 revision;
        quint64 total;
        s >> magic >> version >> fold >> schema >> id >> dbPath >> revision;
        const qsizetype totalOffset = s.device()->pos();
        s >> total;
        const qsizetype chunkOffset = s.device()->pos();
        quint32 count;
        s >> count;
        const qsizetype rawOffset = s.device()->pos();
        if (kind == QStringLiteral("truncated-chunk")) data.truncate(data.size() / 2);
        else if (kind == QStringLiteral("oversized-raw")) {
            QDataStream out(&data, QIODevice::ReadWrite);
            out.setVersion(QDataStream::Qt_6_0);
            out.device()->seek(rawOffset);
            out << quint64(65ull * 1024 * 1024);
        } else if (kind == QStringLiteral("wrong-total")) {
            QDataStream out(&data, QIODevice::ReadWrite);
            out.setVersion(QDataStream::Qt_6_0);
            out.device()->seek(totalOffset);
            out << quint64(total + 1);
        } else if (kind == QStringLiteral("missing-end")) data.chop(sizeof(quint32));
        else if (kind == QStringLiteral("trailing")) data.append('x');
        else if (kind == QStringLiteral("format-2")) {
            QDataStream out(&data, QIODevice::ReadWrite);
            out.setVersion(QDataStream::Qt_6_0);
            out.device()->seek(sizeof(quint32));
            out << quint32(2);
        }
        QVERIFY(chunkOffset > totalOffset);
        QVERIFY(replace(path, data));
        QVERIFY(!IndexCache::forEachRecord(path, nullptr, [](SearchRecord) {}));
        QVERIFY(!IndexCache::read(path).ok);
    }

private:
    const CacheSignature signatureValue = signature();
};

QTEST_GUILESS_MAIN(TestIndexCache)
#include "test_index_cache.moc"
