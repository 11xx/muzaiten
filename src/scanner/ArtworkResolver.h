#pragma once

#include <QString>

struct ArtworkResult {
    enum class Source {
        None,
        FolderFile,
        Embedded,
        Placeholder,
    };

    Source source = Source::None;
    QString sourcePath;
    QString cachePath;
};

class ArtworkResolver final {
public:
    explicit ArtworkResolver(QString cacheRoot);

    ArtworkResult resolveForDirectory(const QString &directoryPath) const;
    QString cacheRoot() const;

private:
    QString m_cacheRoot;
};

