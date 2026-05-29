#include "scanner/LibraryScanner.h"

#include "scanner/TagReader.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QSet>

LibraryScanner::LibraryScanner(QObject *parent)
    : QObject(parent)
{
}

QVector<Track> LibraryScanner::scan(const QString &rootPath) const
{
    QVector<Track> tracks;
    const QFileInfo root(rootPath);
    if (!root.isDir() || root.isSymLink()) {
        return tracks;
    }

    TagReader reader;
    QDirIterator iterator(root.absoluteFilePath(), QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info(path);
        if (info.isSymLink() || !isSupportedAudioFile(path)) {
            continue;
        }
        tracks.push_back(reader.read(path));
    }
    return tracks;
}

const QSet<QString> &LibraryScanner::supportedAudioExtensions()
{
    static const QSet<QString> extensions = {
        QStringLiteral("flac"),
        QStringLiteral("mp3"),
        QStringLiteral("m4a"),
        QStringLiteral("ogg"),
        QStringLiteral("opus"),
        QStringLiteral("wav"),
        QStringLiteral("wv"),
        QStringLiteral("ape"),
    };
    return extensions;
}

bool LibraryScanner::isSupportedAudioFile(const QString &path)
{
    return supportedAudioExtensions().contains(QFileInfo(path).suffix().toLower());
}

