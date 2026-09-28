#include "search/IndexCache.h"

#include "app/AppPaths.h"
#include "db/Database.h"
#include "db/Schema.h"
#include "search/fold/Fold.h"

#include <QByteArray>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSaveFile>

#include <zstd.h>

#include <algorithm>
#include <limits>
#include <optional>

namespace Search::IndexCache {

namespace {

constexpr quint32 kMagic = 0x4D5A5349; // "MZSI"
constexpr int     kZstdLevel = 3;
constexpr auto    kDataStreamVersion = QDataStream::Qt_6_0;

// Records are stored in independently compressed chunks, so neither side ever
// holds more than one chunk's payload. A chunk closes at a record count or at
// kChunkTargetBytes, whichever comes first; the reader refuses any chunk that
// claims more than kMaxChunkBytes or kMaxChunkRecords, so a corrupt or
// tampered file cannot request an absurd allocation.
constexpr quint64 kChunkTargetBytes = 4ull * 1024 * 1024;
constexpr quint64 kMaxChunkBytes = 64ull * 1024 * 1024;
constexpr quint32 kMaxChunkRecords = 65536;

QByteArray zstdCompress(const QByteArray &raw)
{
    const size_t bound = ZSTD_compressBound(static_cast<size_t>(raw.size()));
    if (bound == 0 || bound > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    QByteArray out(static_cast<int>(bound), Qt::Uninitialized);
    const size_t written = ZSTD_compress(out.data(), bound, raw.constData(),
                                         static_cast<size_t>(raw.size()), kZstdLevel);
    if (ZSTD_isError(written)) {
        return {};
    }
    out.resize(static_cast<int>(written));
    return out;
}

QByteArray zstdDecompress(const QByteArray &blob, quint64 rawSize)
{
    if (blob.isEmpty() || rawSize == 0 || rawSize > kMaxChunkBytes) {
        return {};
    }
    QByteArray out(static_cast<int>(rawSize), Qt::Uninitialized);
    const size_t written = ZSTD_decompress(out.data(), static_cast<size_t>(rawSize),
                                           blob.constData(), static_cast<size_t>(blob.size()));
    if (ZSTD_isError(written) || written != static_cast<size_t>(rawSize)) {
        return {};
    }
    return out;
}

void writeSignature(QDataStream &s, const CacheSignature &sig)
{
    s << sig.formatVersion << sig.foldVersion << sig.schemaVersion
      << sig.databaseId << sig.databasePath << sig.contentRevision;
}

bool readSignature(QDataStream &s, CacheSignature &sig)
{
    s >> sig.formatVersion;
    if (sig.formatVersion != kFormatVersion) return false;
    s >> sig.foldVersion >> sig.schemaVersion >> sig.databaseId >> sig.databasePath >> sig.contentRevision;
    return s.status() == QDataStream::Ok && sig.valid();
}

void writeRecord(QDataStream &s, const SearchRecord &r)
{
    s << r.title << r.artistName << r.albumArtistName << r.albumTitle << r.date
      << r.filename << r.path
      << r.normTitle << r.normArtist << r.normAlbumArtist << r.normAlbum
      << r.normFilename << r.normPath
      << r.durationMs << static_cast<qint32>(r.rating0To100)
      << static_cast<qint32>(r.sampleRateHz) << static_cast<qint32>(r.bitrateKbps)
      << static_cast<qint32>(r.channels) << static_cast<qint32>(r.bitDepth) << r.codec
      << static_cast<qint32>(r.trackNumber) << static_cast<qint32>(r.discNumber)
      << r.fileMtime << r.fileSize << static_cast<quint8>(r.source);
}

QString readString(QDataStream &s)
{
    QString v;
    s >> v;
    return v;
}

// Re-intern the high-repetition fields through `pool` (one buffer per distinct
// value), mirroring how the live build dedups them — otherwise every cached row
// would hold its own copy of the artist/album strings.
SearchRecord readRecord(QDataStream &s, QHash<QString, QString> &pool)
{
    const auto intern = [&pool](const QString &v) {
        if (v.isEmpty()) {
            return v;
        }
        const auto it = pool.constFind(v);
        if (it != pool.constEnd()) {
            return it.value();
        }
        pool.insert(v, v);
        return v;
    };

    SearchRecord r;
    s >> r.title;
    r.artistName = intern(readString(s));
    r.albumArtistName = intern(readString(s));
    r.albumTitle = intern(readString(s));
    r.date = intern(readString(s));
    s >> r.filename >> r.path >> r.normTitle;
    r.normArtist = intern(readString(s));
    r.normAlbumArtist = intern(readString(s));
    r.normAlbum = intern(readString(s));
    s >> r.normFilename >> r.normPath;

    qint32 rating = 0, sampleRate = 0, bitrate = 0, channels = 0, bitDepth = 0, track = 0, disc = 0;
    quint8 source = 0;
    s >> r.durationMs >> rating >> sampleRate >> bitrate >> channels >> bitDepth;
    r.codec = intern(readString(s));
    s >> track >> disc >> r.fileMtime >> r.fileSize >> source;
    r.rating0To100 = rating;
    r.sampleRateHz = sampleRate;
    r.bitrateKbps = bitrate;
    r.channels = channels;
    r.bitDepth = bitDepth;
    r.trackNumber = track;
    r.discNumber = disc;
    r.source = source == static_cast<quint8>(TrackSource::Mpd) ? TrackSource::Mpd : TrackSource::Local;
    return r;
}

} // namespace

QString defaultPath()
{
    return QDir(AppPaths::cacheDir()).filePath(QStringLiteral("search-index.cache"));
}

CacheSignature currentSignature(const Database &db)
{
    const auto state = db.searchContentState();
    CacheSignature sig;
    sig.formatVersion  = kFormatVersion;
    sig.foldVersion    = Fold::cacheSignature();
    sig.schemaVersion  = Schema::currentVersion;
    sig.databaseId = state.databaseId;
    sig.databasePath = state.databasePath;
    sig.contentRevision = state.revision;
    return sig;
}

QString mismatchReason(const CacheSignature &cached, const CacheSignature &current)
{
    if (!current.valid()) return QStringLiteral("invalid-source");
    if (!cached.valid()) return QStringLiteral("invalid-cache");
    if (cached.formatVersion != current.formatVersion) return QStringLiteral("format-version");
    if (cached.foldVersion != current.foldVersion) return QStringLiteral("fold-version");
    if (cached.schemaVersion != current.schemaVersion) return QStringLiteral("schema-version");
    if (cached.databaseId != current.databaseId) return QStringLiteral("database-identity");
    if (cached.databasePath != current.databasePath) return QStringLiteral("database-path");
    if (cached.contentRevision != current.contentRevision) return QStringLiteral("content-revision");
    return QStringLiteral("hit");
}

bool write(const QString &path, const CacheSignature &signature,
           const QVector<SearchRecord> &records, quint32 chunkRecords)
{
    if (!signature.valid() || signature.formatVersion != kFormatVersion
        || chunkRecords == 0 || chunkRecords > kMaxChunkRecords) return false;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream s(&file);
    s.setVersion(kDataStreamVersion);
    s << kMagic;
    writeSignature(s, signature);
    s << static_cast<quint64>(records.size());

    QByteArray raw;
    std::optional<QDataStream> chunk;
    quint32 count = 0;
    const auto flush = [&] {
        if (count == 0) return true;
        chunk.reset();
        const QByteArray compressed = zstdCompress(raw);
        if (compressed.isEmpty()) return false;
        s << count << static_cast<quint64>(raw.size()) << static_cast<quint64>(compressed.size());
        if (s.status() != QDataStream::Ok
            || s.writeRawData(compressed.constData(), compressed.size()) != compressed.size()) {
            return false;
        }
        raw.clear();
        count = 0;
        return true;
    };
    for (const SearchRecord &record : records) {
        if (!chunk) {
            chunk.emplace(&raw, QIODevice::WriteOnly);
            chunk->setVersion(kDataStreamVersion);
        }
        writeRecord(*chunk, record);
        ++count;
        // A single record larger than a whole chunk may hold is not cacheable.
        if (chunk->status() != QDataStream::Ok || static_cast<quint64>(raw.size()) > kMaxChunkBytes) {
            file.cancelWriting();
            return false;
        }
        if ((count == chunkRecords || static_cast<quint64>(raw.size()) >= kChunkTargetBytes) && !flush()) {
            file.cancelWriting();
            return false;
        }
    }
    if (!flush()) {
        file.cancelWriting();
        return false;
    }
    s << quint32(0);
    if (s.status() != QDataStream::Ok) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

bool forEachRecord(const QString &path, CacheSignature *outSignature,
                   const std::function<void(SearchRecord)> &sink, const CacheSignature *expected)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QDataStream s(&file);
    s.setVersion(kDataStreamVersion);

    quint32 magic = 0;
    s >> magic;
    if (magic != kMagic) return false;
    CacheSignature signature;
    const bool valid = readSignature(s, signature);
    if (outSignature != nullptr) *outSignature = signature;
    if (!valid || (expected != nullptr && signature != *expected)) return false;

    quint64 total = 0;
    s >> total;
    if (s.status() != QDataStream::Ok) return false;
    quint64 seen = 0;
    QHash<QString, QString> pool;
    while (true) {
        quint32 count = 0;
        s >> count;
        if (s.status() != QDataStream::Ok) return false;
        if (count == 0) return seen == total && s.atEnd();
        quint64 rawSize = 0, compressedSize = 0;
        s >> rawSize >> compressedSize;
        if (s.status() != QDataStream::Ok || count > kMaxChunkRecords
            || count > total - seen || rawSize == 0 || rawSize > kMaxChunkBytes
            || compressedSize == 0 || compressedSize > ZSTD_compressBound(kMaxChunkBytes)
            || compressedSize > static_cast<quint64>(file.bytesAvailable())) return false;
        QByteArray compressed(static_cast<qsizetype>(compressedSize), Qt::Uninitialized);
        if (s.readRawData(compressed.data(), compressed.size()) != compressed.size()) return false;
        const QByteArray raw = zstdDecompress(compressed, rawSize);
        if (raw.isEmpty()) return false;
        QDataStream chunk(raw);
        chunk.setVersion(kDataStreamVersion);
        for (quint32 i = 0; i < count; ++i) {
            SearchRecord rec = readRecord(chunk, pool);
            if (chunk.status() != QDataStream::Ok) return false;
            sink(std::move(rec));
        }
        if (!chunk.atEnd()) return false;
        seen += count;
    }
}

Loaded read(const QString &path)
{
    Loaded result;
    QVector<SearchRecord> records;
    CacheSignature signature;
    const bool ok = forEachRecord(path, &signature,
                                  [&records](SearchRecord rec) { records.push_back(std::move(rec)); });
    if (!ok) {
        return result;
    }
    result.ok = true;
    result.signature = signature;
    result.records = std::move(records);
    return result;
}

} // namespace Search::IndexCache
