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

#include <optional>

namespace {

// Kill the fetch only after yt-dlp has produced no output for this long: a
// large but progressing playlist must not be guillotined mid-stream, while a
// genuinely hung process still dies.
constexpr int kIdleTimeoutMs = 60000;

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

// Map one flat-playlist entry object to an ImportEntry, or nullopt to skip it
// (deleted/private/untitled). Shared by the -J test parser and the -j stream so
// both apply identical title/artist/duration rules.
std::optional<PlaylistImport::ImportEntry> entryFromObject(const QJsonObject &obj)
{
    const QString title = obj.value(QStringLiteral("title")).toString().trimmed();
    if (title.isEmpty() || title == QStringLiteral("[Deleted video]")
        || title == QStringLiteral("[Private video]")) {
        return std::nullopt;
    }

    // Plain-YouTube video titles are often "Artist - Title"; YT Music titles are
    // bare, with the artist on the (auto-generated) channel.
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
    return entry;
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
        if (auto entry = entryFromObject(value.toObject())) {
            entries.append(*entry);
        }
    }
    return entries;
}

void YouTubePlaylistFetcher::readLines()
{
    if (m_process == nullptr) {
        return;
    }
    if (m_idleTimer != nullptr) {
        m_idleTimer->start();  // restart the inactivity countdown on fresh output
    }
    m_stdoutBuffer += m_process->readAllStandardOutput();

    // -j emits one JSON object per line; parse only the complete ones and leave
    // any partial trailing line in the buffer for the next chunk.
    qsizetype newline;
    while ((newline = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newline);
        m_stdoutBuffer.remove(0, newline + 1);
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        if (obj.isEmpty()) {
            continue;
        }
        if (m_playlistTitle.isEmpty()) {
            m_playlistTitle = obj.value(QStringLiteral("playlist_title")).toString();
        }
        if (auto entry = entryFromObject(obj)) {
            m_entries.append(*entry);
        }
    }
    emit progress(static_cast<int>(m_entries.size()));
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

    m_stdoutBuffer.clear();
    m_entries.clear();
    m_playlistTitle.clear();

    m_process = new QProcess(this);
    m_process->setProgram(tool);
    // -j (newline-delimited) + --lazy-playlist stream entries as they are
    // discovered, so the UI can show progress and memory stays flat on large
    // playlists. --ignore-config keeps the fetch deterministic and independent
    // of the user's personal ~/.config/yt-dlp/config.
    m_process->setArguments({QStringLiteral("--flat-playlist"),
                             QStringLiteral("--lazy-playlist"),
                             QStringLiteral("--ignore-config"),
                             QStringLiteral("--no-warnings"),
                             QStringLiteral("-j"),
                             url.trimmed()});

    m_idleTimer = new QTimer(m_process);
    m_idleTimer->setSingleShot(true);
    m_idleTimer->setInterval(kIdleTimeoutMs);
    connect(m_idleTimer, &QTimer::timeout, m_process, &QProcess::kill);

    connect(m_process, &QProcess::readyReadStandardOutput, this,
            &YouTubePlaylistFetcher::readLines);

    connect(m_process, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus status) {
                if (m_process == nullptr) {
                    return;  // errorOccurred() already cleaned up
                }
                readLines();  // drain any tail output before tearing down
                QProcess *process = m_process;
                m_process = nullptr;
                m_idleTimer = nullptr;  // child of process; deleted with it below
                const QString stderrText =
                    QString::fromUtf8(process->readAllStandardError()).trimmed();
                process->deleteLater();
                if (status != QProcess::NormalExit || exitCode != 0) {
                    emit error(stderrText.isEmpty()
                                   ? QStringLiteral("yt-dlp failed (exit %1).").arg(exitCode)
                                   : stderrText.section(QLatin1Char('\n'), -1));
                    return;
                }
                if (m_entries.isEmpty()) {
                    emit error(QStringLiteral("No tracks found in the playlist."));
                    return;
                }
                emit finished(m_entries, m_playlistTitle);
            });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (m_process == nullptr) {
            return;  // finished() already handled it
        }
        QProcess *process = m_process;
        m_process = nullptr;
        m_idleTimer = nullptr;
        process->deleteLater();
        emit error(QStringLiteral("Failed to run yt-dlp: %1").arg(process->errorString()));
    });

    m_idleTimer->start();
    m_process->start();
}
