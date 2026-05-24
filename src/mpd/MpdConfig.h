#pragma once

#include <QString>
#include <QStringList>

struct MpdConfig {
    QString path;
    QString musicDirectory;
    QString playlistDirectory;
    QString dbFile;
    QString stickerFile;
    QString metadataToUse;
};

class MpdConfigParser final {
public:
    static MpdConfig parseText(const QString &text, const QString &path = {});
    static MpdConfig parseFile(const QString &path, QString *error = nullptr);
    static QStringList defaultConfigCandidates();

private:
    static bool parseLine(const QString &line, QString *key, QString *value);
};
