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

// Audio-only extensions. MP4-family audio enters through m4a/m4b; bare .mp4 is
// excluded because nothing downstream tells an audio stream from a video one,
// so admitting it would file movies as library tracks.
const QSet<QString> &LibraryScanner::supportedAudioExtensions()
{
    static const QSet<QString> extensions = {
        QStringLiteral("aac"),
        QStringLiteral("aif"),
        QStringLiteral("aifc"),
        QStringLiteral("aiff"),
        QStringLiteral("ape"),
        QStringLiteral("dff"),
        QStringLiteral("dsdiff"),
        QStringLiteral("dsf"),
        QStringLiteral("flac"),
        QStringLiteral("m4a"),
        QStringLiteral("m4b"),
        QStringLiteral("mka"),
        QStringLiteral("mp2"),
        QStringLiteral("mp3"),
        QStringLiteral("mpc"),
        QStringLiteral("oga"),
        QStringLiteral("ogg"),
        QStringLiteral("ogx"),
        QStringLiteral("opus"),
        QStringLiteral("tta"),
        QStringLiteral("wav"),
        QStringLiteral("wma"),
        QStringLiteral("wv"),
    };
    return extensions;
}

bool LibraryScanner::isSupportedAudioFile(const QString &path)
{
    return supportedAudioExtensions().contains(QFileInfo(path).suffix().toLower());
}

