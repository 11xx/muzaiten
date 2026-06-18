#include "scanner/ArtworkCache.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QVariant>

#include <taglib/fileref.h>
#include <taglib/tlist.h>
#include <taglib/tvariant.h>

#include <algorithm>

namespace {

constexpr int kSourceFolder = 0;
constexpr int kSourceEmbedded = 1;
constexpr int kMinArtSize = 128;
constexpr int kMaxArtSize = 4096;

QStringList folderArtCandidates()
{
    return {
        QStringLiteral("cover.jpg"),  QStringLiteral("Cover.jpg"),  QStringLiteral("cover.jpeg"),
        QStringLiteral("Cover.jpeg"), QStringLiteral("cover.png"),  QStringLiteral("Cover.png"),
        QStringLiteral("folder.jpg"), QStringLiteral("Folder.jpg"), QStringLiteral("folder.png"),
        QStringLiteral("Folder.png"), QStringLiteral("front.jpg"),  QStringLiteral("Front.jpg"),
        QStringLiteral("front.jpeg"), QStringLiteral("Front.jpeg"), QStringLiteral("front.png"),
        QStringLiteral("Front.png"),  QStringLiteral("album.jpg"),  QStringLiteral("Album.jpg"),
        QStringLiteral("artwork.jpg"), QStringLiteral("Artwork.jpg"),
    };
}

const QSet<QString> &supportedImageExtensions()
{
    static const QSet<QString> extensions = {
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("png"),
        QStringLiteral("webp"),
        QStringLiteral("bmp"),
        QStringLiteral("gif"),
        QStringLiteral("tif"),
        QStringLiteral("tiff"),
    };
    return extensions;
}

QString normalizedArtName(QString text)
{
    text = QFileInfo(text).completeBaseName().toLower();
    static const QRegularExpression bracketed(QStringLiteral("[\\[(][^\\])]*[\\])]"));
    text.remove(bracketed);
    static const QRegularExpression separators(QStringLiteral("[^\\p{L}\\p{N}]+"));
    text.replace(separators, QStringLiteral(" "));
    static const QRegularExpression whitespace(QStringLiteral("\\s{2,}"));
    text.replace(whitespace, QStringLiteral(" "));
    return text.trimmed();
}

bool isSupportedImageFile(const QFileInfo &info)
{
    return info.isFile() && info.isReadable() && supportedImageExtensions().contains(info.suffix().toLower());
}

int artCandidateScore(const QFileInfo &info, const QString &albumHint)
{
    const QString name = normalizedArtName(info.fileName());
    if (name.isEmpty()) {
        return 0;
    }

    int score = 1; // any image in the album dir is a last-resort fallback
    if (name == QStringLiteral("cover") || name == QStringLiteral("folder") || name == QStringLiteral("front")) {
        score = 100;
    } else if (name.startsWith(QStringLiteral("cover")) || name.startsWith(QStringLiteral("front"))) {
        score = 90;
    } else if (name.contains(QStringLiteral("cover")) || name.contains(QStringLiteral("front"))
               || name.contains(QStringLiteral("folder")) || name.contains(QStringLiteral("album"))
               || name.contains(QStringLiteral("artwork"))) {
        score = 75;
    }
    if (!albumHint.isEmpty() && (name == albumHint || name.contains(albumHint) || albumHint.contains(name))) {
        score += 20;
    }
    if (name.contains(QStringLiteral("back")) || name.contains(QStringLiteral("booklet"))
        || name.contains(QStringLiteral("obi")) || name.contains(QStringLiteral("insert"))
        || name.contains(QStringLiteral("scan"))) {
        score -= 25;
    }
    return score;
}

QString bestImageInDirectory(const QDir &dir, const QString &albumHint)
{
    QFileInfo best;
    int bestScore = 0;
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (!isSupportedImageFile(entry)) {
            continue;
        }
        const int score = artCandidateScore(entry, albumHint);
        if (score > bestScore || (score == bestScore && best.isFile() && entry.fileName() < best.fileName())) {
            best = entry;
            bestScore = score;
        }
    }
    return bestScore > 0 ? best.absoluteFilePath() : QString();
}

bool isLikelyArtDirectory(const QFileInfo &info)
{
    if (!info.isDir() || !info.isReadable() || info.isSymLink()) {
        return false;
    }
    const QString name = normalizedArtName(info.fileName());
    return name == QStringLiteral("cover") || name == QStringLiteral("covers")
        || name == QStringLiteral("art") || name == QStringLiteral("artwork")
        || name == QStringLiteral("scan") || name == QStringLiteral("scans")
        || name == QStringLiteral("image") || name == QStringLiteral("images")
        || name == QStringLiteral("booklet") || name == QStringLiteral("booklets")
        || name.startsWith(QStringLiteral("cover ")) || name.startsWith(QStringLiteral("covers "));
}

QString folderArtPath(const QString &directory)
{
    if (directory.isEmpty()) {
        return {};
    }
    const QDir dir(directory);
    for (const QString &name : folderArtCandidates()) {
        const QFileInfo candidate(dir.filePath(name));
        if (candidate.isFile() && candidate.isReadable()) {
            return candidate.absoluteFilePath();
        }
    }
    const QString albumHint = normalizedArtName(dir.dirName());
    const QString direct = bestImageInDirectory(dir, albumHint);
    if (!direct.isEmpty()) {
        return direct;
    }
    const QFileInfoList subdirs = dir.entryInfoList(QDir::Dirs | QDir::Readable | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &subdir : subdirs) {
        if (!isLikelyArtDirectory(subdir)) {
            continue;
        }
        const QString nested = bestImageInDirectory(QDir(subdir.absoluteFilePath()), albumHint);
        if (!nested.isEmpty()) {
            return nested;
        }
    }
    return {};
}

QString cacheKeyFor(const QString &path, char kind, int artSize)
{
    const QFileInfo info(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(info.absoluteFilePath().toUtf8());
    hash.addData(QByteArray::number(info.lastModified().toSecsSinceEpoch()));
    hash.addData(QByteArray::number(info.size()));
    hash.addData(QByteArray(1, kind));
    hash.addData(QByteArray::number(artSize)); // size is part of the identity
    return QString::fromLatin1(hash.result().toHex());
}

QImage squareArt(const QImage &source, int artSize)
{
    if (source.isNull()) {
        return {};
    }
    QImage square(artSize, artSize, QImage::Format_ARGB32);
    square.fill(Qt::transparent);
    const QImage scaled = source.scaled(artSize, artSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter painter(&square);
    painter.drawImage((artSize - scaled.width()) / 2, (artSize - scaled.height()) / 2, scaled);
    painter.end();
    return square;
}

QByteArray toPng(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

// Decode through QImageReader, pre-scaling so neither dimension exceeds maxDim.
// size() reads only the header, so an oversized (decompression-bomb) image is
// never fully decoded to native resolution just to be downscaled to the cached
// art size moments later.
QImage decodeBounded(QImageReader &reader, int maxDim)
{
    const QSize native = reader.size();
    if (native.isValid() && (native.width() > maxDim || native.height() > maxDim)) {
        reader.setScaledSize(native.scaled(maxDim, maxDim, Qt::KeepAspectRatio));
    }
    return reader.read();
}

QImage folderArtImage(const QString &path, int maxDim)
{
    QImageReader reader(path);
    return decodeBounded(reader, maxDim);
}

QImage embeddedArt(const QString &filePath, int maxDim)
{
    if (filePath.isEmpty()) {
        return {};
    }
    TagLib::FileRef file(filePath.toUtf8().constData(), false);
    if (file.isNull()) {
        return {};
    }
    const auto pictures = file.complexProperties("PICTURE");
    if (pictures.isEmpty()) {
        return {};
    }
    const TagLib::VariantMap picture = pictures.front();
    const TagLib::ByteVector data = picture.value("data").value<TagLib::ByteVector>();
    if (data.isEmpty()) {
        return {};
    }
    QByteArray bytes(data.data(), static_cast<qsizetype>(data.size()));
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    return decodeBounded(reader, maxDim);
}

} // namespace

ArtworkCache::ArtworkCache(QString dbPath, int artSize, QObject *parent)
    : QObject(parent)
    , m_dbPath(std::move(dbPath))
    , m_connectionName(QStringLiteral("artwork-cache-%1").arg(reinterpret_cast<quintptr>(this)))
    , m_artSize(std::clamp(artSize, kMinArtSize, kMaxArtSize))
{
    m_thread = new QThread(this);
    moveToThread(m_thread);
    connect(m_thread, &QThread::started, this, &ArtworkCache::initialize);
    m_thread->start();
}

ArtworkCache::~ArtworkCache()
{
    if (m_thread != nullptr) {
        if (m_thread->isRunning()) {
            // A QSqlDatabase connection may only be touched from the thread that
            // created it (here, the worker via initialize()). Close and release
            // the handle on that thread while its event loop is still running,
            // before we stop it.
            QMetaObject::invokeMethod(this, "shutdown", Qt::BlockingQueuedConnection);
        }
        m_thread->quit();
        m_thread->wait(5000);
    }
    // Safe from any thread now that no QSqlDatabase object references the
    // connection (shutdown() reset m_db on the worker thread).
    QSqlDatabase::removeDatabase(m_connectionName);
}

void ArtworkCache::shutdown()
{
    if (m_db.isValid()) {
        m_db.close();
    }
    m_db = QSqlDatabase();
}

void ArtworkCache::initialize()
{
    QFileInfo info(m_dbPath);
    QDir().mkpath(info.absolutePath());

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(m_dbPath);
    if (!m_db.open()) {
        return;
    }

    QSqlQuery query(m_db);
    query.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    query.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    query.exec(QStringLiteral("PRAGMA busy_timeout=5000"));
    query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS artwork_blobs (cache_key TEXT PRIMARY KEY, source INTEGER NOT NULL, "
        "width INTEGER, height INTEGER, format INTEGER NOT NULL DEFAULT 0, data BLOB NOT NULL, updated_at TEXT NOT NULL)"));
}

void ArtworkCache::releaseCacheMemory()
{
    if (!m_db.isOpen()) {
        return;
    }

    QSqlQuery query(m_db);
    query.exec(QStringLiteral("PRAGMA shrink_memory"));
}

void ArtworkCache::requestArtwork(const QString &token, const QString &directory, const QString &filePath, quint64 generation)
{
    QMetaObject::invokeMethod(this, "handleRequest", Qt::QueuedConnection,
                              Q_ARG(QString, token), Q_ARG(QString, directory),
                              Q_ARG(QString, filePath), Q_ARG(quint64, generation));
}

void ArtworkCache::setArtSize(int artSize)
{
    m_artSize.store(std::clamp(artSize, kMinArtSize, kMaxArtSize));
}

void ArtworkCache::handleRequest(const QString &token, const QString &directory, const QString &filePath, quint64 generation)
{
    const int artSize = m_artSize.load();

    // Folder art first (preferred, shared across an album's tracks).
    const QString folder = folderArtPath(directory);
    if (!folder.isEmpty()) {
        const QString key = cacheKeyFor(folder, 'f', artSize);
        QImage cached = lookupBlob(key);
        if (cached.isNull()) {
            cached = squareArt(folderArtImage(folder, artSize), artSize);
            if (!cached.isNull()) {
                storeBlob(key, kSourceFolder, cached, toPng(cached));
            }
        }
        if (!cached.isNull()) {
            emit artworkReady(token, cached, generation);
            return;
        }
    }

    // Embedded art fallback (lazy; only on navigation, never during scans).
    if (!filePath.isEmpty()) {
        const QString key = cacheKeyFor(filePath, 'e', artSize);
        QImage cached = lookupBlob(key);
        if (cached.isNull()) {
            cached = squareArt(embeddedArt(filePath, artSize), artSize);
            if (!cached.isNull()) {
                storeBlob(key, kSourceEmbedded, cached, toPng(cached));
            }
        }
        if (!cached.isNull()) {
            emit artworkReady(token, cached, generation);
            return;
        }
    }

    emit artworkMissing(token, generation);
}

QImage ArtworkCache::lookupBlob(const QString &cacheKey) const
{
    if (!m_db.isOpen()) {
        return {};
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT data FROM artwork_blobs WHERE cache_key = ?"));
    query.addBindValue(cacheKey);
    if (query.exec() && query.next()) {
        return QImage::fromData(query.value(0).toByteArray(), "PNG");
    }
    return {};
}

void ArtworkCache::storeBlob(const QString &cacheKey, int source, const QImage &image, const QByteArray &png)
{
    if (!m_db.isOpen() || png.isEmpty()) {
        return;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO artwork_blobs(cache_key, source, width, height, format, data, updated_at) "
        "VALUES(?, ?, ?, ?, 0, ?, datetime('now')) "
        "ON CONFLICT(cache_key) DO UPDATE SET source=excluded.source, width=excluded.width, "
        "height=excluded.height, data=excluded.data, updated_at=excluded.updated_at"));
    query.addBindValue(cacheKey);
    query.addBindValue(source);
    query.addBindValue(image.width());
    query.addBindValue(image.height());
    query.addBindValue(png);
    query.exec();
}
