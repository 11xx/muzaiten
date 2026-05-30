#pragma once

#include <QImage>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

#include <QtTypes>

#include <atomic>

class QThread;

// Monolithic artwork cache backed by a single SQLite file (WAL), replacing the
// directory of cached image files. Resolution is asynchronous on a dedicated
// low-priority worker thread: cache hit -> folder art -> embedded art (TagLib
// PICTURE) -> miss. Results are reported via signals, tagged with the caller's
// generation so stale replies (after fast navigation) can be dropped. Never
// invoked during scans.
class ArtworkCache final : public QObject {
    Q_OBJECT

public:
    // artSize is the square pixel size cached covers are rendered to.
    explicit ArtworkCache(QString dbPath, int artSize, QObject *parent = nullptr);
    ~ArtworkCache() override;

    // Thread-safe. Resolves art for the given directory (folder art) and/or
    // filePath (embedded art) asynchronously; replies carry back token+generation.
    void requestArtwork(const QString &token, const QString &directory, const QString &filePath, quint64 generation);

    // Thread-safe. Changes the cached cover resolution. The size is part of each
    // cache key, so existing blobs at other sizes are simply re-rendered lazily.
    void setArtSize(int artSize);

signals:
    void artworkReady(QString token, QImage image, quint64 generation);
    void artworkMissing(QString token, quint64 generation);

private slots:
    void initialize();
    void handleRequest(const QString &token, const QString &directory, const QString &filePath, quint64 generation);

private:
    QImage lookupBlob(const QString &cacheKey) const;
    void storeBlob(const QString &cacheKey, int source, const QImage &image, const QByteArray &png);

    QString m_dbPath;
    QString m_connectionName;
    QThread *m_thread = nullptr;
    QSqlDatabase m_db;
    std::atomic<int> m_artSize;
};
