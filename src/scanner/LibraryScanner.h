#pragma once

#include "core/Track.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

class LibraryScanner final : public QObject {
    Q_OBJECT

public:
    explicit LibraryScanner(QObject *parent = nullptr);

    QVector<Track> scan(const QString &rootPath) const;
    static bool isSupportedAudioFile(const QString &path);
    // Lowercased audio file suffixes (without the dot). Exposed so the parallel
    // walker can classify entries without constructing a QFileInfo per file.
    static const QSet<QString> &supportedAudioExtensions();

private:
};
