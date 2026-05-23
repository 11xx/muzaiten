#pragma once

#include "core/Track.h"

#include <QObject>
#include <QString>
#include <QVector>

class LibraryScanner final : public QObject {
    Q_OBJECT

public:
    explicit LibraryScanner(QObject *parent = nullptr);

    QVector<Track> scan(const QString &rootPath) const;

private:
    static bool isSupportedAudioFile(const QString &path);
};

