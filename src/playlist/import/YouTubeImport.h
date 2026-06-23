#pragma once

// YouTube / YouTube Music playlist fetcher. Deliberately independent from
// paywalled transfer services: it shells out to yt-dlp (the de-facto
// maintained extractor) with --flat-playlist, which needs no API key or login
// for public/unlisted playlists and covers both youtube.com and
// music.youtube.com URLs. Output is streamed line-by-line (-j) so the UI can
// show progress and memory stays flat on large playlists. Network/tool code
// stays in this module; the result is a plain ImportEntry list feeding the same
// offline matcher as every other import source.

#include "playlist/PlaylistImport.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

class QProcess;
class QTimer;

class YouTubePlaylistFetcher final : public QObject {
    Q_OBJECT
public:
    explicit YouTubePlaylistFetcher(QObject *parent = nullptr);

    // True when yt-dlp is on PATH. The UI greys the fetch row out otherwise.
    static bool toolAvailable();
    // Accepts youtube.com/music.youtube.com playlist or watch?list= URLs.
    static bool looksLikePlaylistUrl(const QString &url);

    // Parse a yt-dlp --flat-playlist -J document (single wrapper object).
    // Exposed for tests; the live fetch streams the -j variant instead.
    static QVector<PlaylistImport::ImportEntry> entriesFromJson(const QByteArray &json,
                                                                QString *playlistTitle = nullptr);

    // Async, one fetch at a time; emits progress() as tracks stream in, then
    // finished() or error().
    void fetch(const QString &url);
    bool isFetching() const { return m_process != nullptr; }

signals:
    void progress(int count);
    void finished(QVector<PlaylistImport::ImportEntry> entries, QString playlistTitle);
    void error(QString message);

private:
    // Drain whatever complete JSON lines yt-dlp has emitted into m_entries.
    void readLines();

    QProcess *m_process = nullptr;
    QTimer *m_idleTimer = nullptr;
    QByteArray m_stdoutBuffer;
    QVector<PlaylistImport::ImportEntry> m_entries;
    QString m_playlistTitle;
};
