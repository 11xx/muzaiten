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

#include <limits>

namespace Search::IndexCache {

namespace {

constexpr quint32 kMagic = 0x4D5A5349; // "MZSI"
constexpr int     kZstdLevel = 3;
constexpr auto    kDataStreamVersion = QDataStream::Qt_6_0;

// Guard against a corrupt/tampered header claiming an absurd decoded size.
constexpr unsigned long long kMaxDecodedBytes = 1024ull * 1024 * 1024; // 1 GiB

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
    if (blob.isEmpty() || rawSize == 0 || rawSize > kMaxDecodedBytes) {
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
      << sig.localCount << sig.localMaxMtime << sig.mpdCount << sig.rootsHash;
}

void readSignature(QDataStream &s, CacheSignature &sig)
{
    s >> sig.formatVersion >> sig.foldVersion >> sig.schemaVersion
      >> sig.localCount >> sig.localMaxMtime >> sig.mpdCount >> sig.rootsHash;
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
    const Database::SearchRowSummary summary = db.searchRowSummary();
    CacheSignature sig;
    sig.formatVersion  = kFormatVersion;
    sig.foldVersion    = Fold::kVersion;
    sig.schemaVersion  = Schema::currentVersion;
    sig.localCount     = summary.localCount;
    sig.localMaxMtime  = summary.localMaxMtime;
    sig.mpdCount       = summary.mpdCount;
    sig.rootsHash      = summary.rootsHash;
    return sig;
}

bool write(const QString &path, const CacheSignature &signature, const QVector<SearchRecord> &records)
{
    // Serialize the records, then compress that payload.
    QByteArray payload;
    {
        QDataStream s(&payload, QIODevice::WriteOnly);
        s.setVersion(kDataStreamVersion);
        s << static_cast<quint64>(records.size());
        for (const SearchRecord &r : records) {
            writeRecord(s, r);
        }
    }
    const QByteArray compressed = zstdCompress(payload);
    if (compressed.isEmpty() && !payload.isEmpty()) {
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream s(&file);
    s.setVersion(kDataStreamVersion);
    s << kMagic;
    writeSignature(s, signature);
    s << static_cast<quint64>(payload.size()); // raw payload size for the decoder
    s << compressed;
    if (s.status() != QDataStream::Ok) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

bool forEachRecord(const QString &path, CacheSignature *outSignature,
                   const std::function<void(SearchRecord)> &sink)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QDataStream s(&file);
    s.setVersion(kDataStreamVersion);

    quint32 magic = 0;
    s >> magic;
    if (magic != kMagic) {
        return false;
    }
    CacheSignature signature;
    readSignature(s, signature);
    quint64 rawSize = 0;
    QByteArray compressed;
    s >> rawSize >> compressed;
    if (s.status() != QDataStream::Ok) {
        return false;
    }

    const QByteArray payload = zstdDecompress(compressed, rawSize);
    if (payload.isEmpty() && rawSize != 0) {
        return false;
    }

    QDataStream ps(payload);
    ps.setVersion(kDataStreamVersion);
    quint64 count = 0;
    ps >> count;
    if (count > kMaxDecodedBytes) { // sanity vs a corrupt count
        return false;
    }
    QHash<QString, QString> pool;
    for (quint64 i = 0; i < count; ++i) {
        SearchRecord rec = readRecord(ps, pool);
        if (ps.status() != QDataStream::Ok) {
            return false; // truncated/corrupt payload
        }
        sink(std::move(rec));
    }

    if (outSignature) {
        *outSignature = signature;
    }
    return true;
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
