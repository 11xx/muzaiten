#include "playlist/import/YouTubeImport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {

constexpr int kFetchTimeoutMs = 60000;

QString ytDlpPath()
{
    return QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
}

// YT Music auto-generated channels are named "Artist - Topic".
QString cleanChannelName(QString name)
{
    if (name.endsWith(QStringLiteral(" - Topic"))) {
        name.chop(QStringLiteral(" - Topic").size());
    }
    return name.trimmed();
}

} // namespace

YouTubePlaylistFetcher::YouTubePlaylistFetcher(QObject *parent)
    : QObject(parent)
{
}

bool YouTubePlaylistFetcher::toolAvailable()
{
    return !ytDlpPath().isEmpty();
}

bool YouTubePlaylistFetcher::looksLikePlaylistUrl(const QString &url)
{
    const QUrl parsed(url.trimmed());
    if (!parsed.isValid()) {
        return false;
    }
    const QString host = parsed.host().toLower();
    if (!host.endsWith(QStringLiteral("youtube.com")) && host != QStringLiteral("youtu.be")) {
        return false;
    }
    return QUrlQuery(parsed).hasQueryItem(QStringLiteral("list"))
        || parsed.path().startsWith(QStringLiteral("/playlist"));
}

QVector<PlaylistImport::ImportEntry> YouTubePlaylistFetcher::entriesFromJson(
    const QByteArray &json, QString *playlistTitle)
{
    QVector<PlaylistImport::ImportEntry> entries;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (playlistTitle != nullptr) {
        *playlistTitle = root.value(QStringLiteral("title")).toString();
    }
    const QJsonArray jsonEntries = root.value(QStringLiteral("entries")).toArray();
    for (const QJsonValue &value : jsonEntries) {
        const QJsonObject obj = value.toObject();
        const QString title = obj.value(QStringLiteral("title")).toString().trimmed();
        if (title.isEmpty() || title == QStringLiteral("[Deleted video]")
            || title == QStringLiteral("[Private video]")) {
            continue;
        }

        // Plain-YouTube video titles are often "Artist - Title"; YT Music
        // titles are bare, with the artist on the (auto-generated) channel.
        PlaylistImport::ImportEntry entry = PlaylistImport::parseLine(title);
        if (entry.artist.isEmpty()) {
            QString channel = obj.value(QStringLiteral("channel")).toString();
            if (channel.isEmpty()) {
                channel = obj.value(QStringLiteral("uploader")).toString();
            }
            entry.artist = cleanChannelName(channel);
        }

        const double durationSecs = obj.value(QStringLiteral("duration")).toDouble();
        if (durationSecs > 0) {
            entry.durationMs = static_cast<qint64>(durationSecs * 1000.0);
        }
        entry.externalId = obj.value(QStringLiteral("id")).toString();
        entries.append(entry);
    }
    return entries;
}

void YouTubePlaylistFetcher::fetch(const QString &url)
{
    if (m_process != nullptr) {
        emit error(QStringLiteral("A fetch is already running."));
        return;
    }
    const QString tool = ytDlpPath();
    if (tool.isEmpty()) {
        emit error(QStringLiteral("yt-dlp not found in PATH."));
        return;
    }

    m_process = new QProcess(this);
    m_process->setProgram(tool);
    m_process->setArguments({QStringLiteral("--flat-playlist"),
                             QStringLiteral("--no-warnings"),
                             QStringLiteral("-J"),
                             url.trimmed()});

    auto *timeout = new QTimer(m_process);
    timeout->setSingleShot(true);
    timeout->setInterval(kFetchTimeoutMs);
    connect(timeout, &QTimer::timeout, m_process, &QProcess::kill);

    connect(m_process, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus status) {
                QProcess *process = m_process;
                m_process = nullptr;
                process->deleteLater();
                if (status != QProcess::NormalExit || exitCode != 0) {
                    const QString stderrText =
                        QString::fromUtf8(process->readAllStandardError()).trimmed();
                    emit error(stderrText.isEmpty()
                                   ? QStringLiteral("yt-dlp failed (exit %1).").arg(exitCode)
                                   : stderrText.section(QLatin1Char('\n'), -1));
                    return;
                }
                QString playlistTitle;
                const auto entries = entriesFromJson(process->readAllStandardOutput(),
                                                     &playlistTitle);
                if (entries.isEmpty()) {
                    emit error(QStringLiteral("No tracks found in the playlist."));
                    return;
                }
                emit finished(entries, playlistTitle);
            });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (m_process == nullptr) {
            return;  // finished() already handled it
        }
        QProcess *process = m_process;
        m_process = nullptr;
        process->deleteLater();
        emit error(QStringLiteral("Failed to run yt-dlp: %1").arg(process->errorString()));
    });

    timeout->start();
    m_process->start();
}
