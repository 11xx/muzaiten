#include "mpd/MpdConfig.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

namespace {

QString stripComment(const QString &line)
{
    bool inQuote = false;
    QChar quote;
    for (qsizetype index = 0; index < line.size(); ++index) {
        const QChar ch = line.at(index);
        if ((ch == QLatin1Char('"') || ch == QLatin1Char('\'')) && (index == 0 || line.at(index - 1) != QLatin1Char('\\'))) {
            if (!inQuote) {
                inQuote = true;
                quote = ch;
            } else if (quote == ch) {
                inQuote = false;
            }
            continue;
        }
        if (ch == QLatin1Char('#') && !inQuote) {
            return line.left(index);
        }
    }
    return line;
}

QString unquote(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2) {
        const QChar first = value.front();
        const QChar last = value.back();
        if ((first == QLatin1Char('"') && last == QLatin1Char('"')) || (first == QLatin1Char('\'') && last == QLatin1Char('\''))) {
            value = value.mid(1, value.size() - 2);
        }
    }
    value.replace(QStringLiteral("\\\""), QStringLiteral("\""));
    value.replace(QStringLiteral("\\'"), QStringLiteral("'"));
    value.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
    return value;
}

void applyValue(MpdConfig *config, const QString &key, const QString &value)
{
    if (key == QStringLiteral("music_directory")) {
        config->musicDirectory = value;
    } else if (key == QStringLiteral("playlist_directory")) {
        config->playlistDirectory = value;
    } else if (key == QStringLiteral("db_file")) {
        config->dbFile = value;
    } else if (key == QStringLiteral("sticker_file")) {
        config->stickerFile = value;
    } else if (key == QStringLiteral("metadata_to_use")) {
        config->metadataToUse = value;
    }
}

} // namespace

MpdConfig MpdConfigParser::parseText(const QString &text, const QString &path)
{
    MpdConfig config;
    config.path = path;

    QTextStream stream(const_cast<QString *>(&text), QIODevice::ReadOnly);
    while (!stream.atEnd()) {
        QString key;
        QString value;
        if (parseLine(stream.readLine(), &key, &value)) {
            applyValue(&config, key, value);
        }
    }

    return config;
}

MpdConfig MpdConfigParser::parseFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        MpdConfig config;
        config.path = path;
        return config;
    }

    const QString text = QString::fromUtf8(file.readAll());
    return parseText(text, path);
}

QStringList MpdConfigParser::defaultConfigCandidates()
{
    QStringList candidates;
    const QString xdgConfig = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (!xdgConfig.isEmpty()) {
        candidates.push_back(QDir(xdgConfig).filePath(QStringLiteral("mpd/mpd.conf")));
    }
    candidates.push_back(QDir::home().filePath(QStringLiteral(".config/mpd/mpd.conf")));
    candidates.push_back(QStringLiteral("/etc/mpd.conf"));
    candidates.removeDuplicates();
    return candidates;
}

bool MpdConfigParser::parseLine(const QString &line, QString *key, QString *value)
{
    const QString trimmed = stripComment(line).trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('{')) || trimmed.startsWith(QLatin1Char('}'))) {
        return false;
    }

    qsizetype split = -1;
    for (qsizetype index = 0; index < trimmed.size(); ++index) {
        if (trimmed.at(index).isSpace()) {
            split = index;
            break;
        }
    }
    if (split < 0) {
        return false;
    }

    const QString parsedKey = trimmed.left(split).trimmed();
    const QString parsedValue = unquote(trimmed.mid(split + 1));
    if (parsedKey.isEmpty() || parsedValue.isEmpty()) {
        return false;
    }

    *key = parsedKey;
    *value = parsedValue;
    return true;
}
