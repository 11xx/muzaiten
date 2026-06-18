#include "scanner/ArtworkCache.h"

#include <utility>

ArtworkCache::ArtworkCache(QString dbPath, int artSize, QObject *parent)
    : QObject(parent),
      m_dbPath(std::move(dbPath)),
      m_artSize(artSize)
{
}

ArtworkCache::~ArtworkCache() = default;

void ArtworkCache::requestArtwork(const QString &, const QString &, const QString &, quint64)
{
}

void ArtworkCache::setArtSize(int artSize)
{
    m_artSize.store(artSize);
}

void ArtworkCache::initialize()
{
}

void ArtworkCache::shutdown()
{
}

void ArtworkCache::releaseCacheMemory()
{
}

void ArtworkCache::handleRequest(const QString &, const QString &, const QString &, quint64)
{
}
