#pragma once

// YouTube / YouTube Music playlist fetcher. Deliberately independent from
// paywalled transfer services: it shells out to yt-dlp (the de-facto
// maintained extractor) with --flat-playlist -J, which needs no API key or
// login for public/unlisted playlists and covers both youtube.com and
// music.youtube.com URLs. Network/tool code stays in this module; the result
// is a plain ImportEntry list feeding the same offline matcher as every other
// import source.

#include "playlist/PlaylistImport.h"

#include <QObject>
#include <QString>
#include <QVector>

class QProcess;

class YouTubePlaylistFetcher final : public QObject {
    Q_OBJECT
public:
    explicit YouTubePlaylistFetcher(QObject *parent = nullptr);

    // True when yt-dlp is on PATH. The UI greys the fetch row out otherwise.
    static bool toolAvailable();
    // Accepts youtube.com/music.youtube.com playlist or watch?list= URLs.
    static bool looksLikePlaylistUrl(const QString &url);

    // Parse a yt-dlp --flat-playlist -J document. Exposed for tests.
    static QVector<PlaylistImport::ImportEntry> entriesFromJson(const QByteArray &json,
                                                                QString *playlistTitle = nullptr);

    // Async, one fetch at a time; emits finished() or error().
    void fetch(const QString &url);
    bool isFetching() const { return m_process != nullptr; }

signals:
    void finished(QVector<PlaylistImport::ImportEntry> entries, QString playlistTitle);
    void error(QString message);

private:
    QProcess *m_process = nullptr;
};
