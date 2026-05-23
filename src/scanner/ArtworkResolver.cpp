#include "scanner/ArtworkResolver.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>

namespace {

QStringList candidateNames()
{
    return {
        QStringLiteral("cover.jpg"),
        QStringLiteral("Cover.jpg"),
        QStringLiteral("cover.jpeg"),
        QStringLiteral("Cover.jpeg"),
        QStringLiteral("cover.png"),
        QStringLiteral("Cover.png"),
        QStringLiteral("folder.jpg"),
        QStringLiteral("Folder.jpg"),
        QStringLiteral("folder.png"),
        QStringLiteral("Folder.png"),
        QStringLiteral("front.jpg"),
        QStringLiteral("Front.jpg"),
        QStringLiteral("front.png"),
        QStringLiteral("Front.png"),
    };
}

QString cacheNameFor(const QFileInfo &source)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(source.absoluteFilePath().toUtf8());
    hash.addData(QByteArray::number(source.lastModified().toSecsSinceEpoch()));
    hash.addData(QByteArray::number(source.size()));
    return QString::fromLatin1(hash.result().toHex()) + QStringLiteral(".jpg");
}

} // namespace

ArtworkResolver::ArtworkResolver(QString cacheRoot)
    : m_cacheRoot(std::move(cacheRoot))
{
}

ArtworkResult ArtworkResolver::resolveForDirectory(const QString &directoryPath) const
{
    const QDir dir(directoryPath);
    for (const QString &name : candidateNames()) {
        const QFileInfo source(dir.filePath(name));
        if (!source.isFile() || !source.isReadable()) {
            continue;
        }

        QDir().mkpath(m_cacheRoot);
        const QString cachePath = QDir(m_cacheRoot).filePath(cacheNameFor(source));
        if (!QFileInfo::exists(cachePath)) {
            const QImage image(source.absoluteFilePath());
            if (!image.isNull()) {
                QImage square(320, 320, QImage::Format_ARGB32);
                square.fill(Qt::transparent);
                const QImage scaled = image.scaled(320, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                QPainter painter(&square);
                painter.drawImage((320 - scaled.width()) / 2, (320 - scaled.height()) / 2, scaled);
                painter.end();
                square.save(cachePath, "PNG");
            }
        }

        return {ArtworkResult::Source::FolderFile, source.absoluteFilePath(), cachePath};
    }

    return {ArtworkResult::Source::None, {}, {}};
}

QString ArtworkResolver::cacheRoot() const
{
    return m_cacheRoot;
}
