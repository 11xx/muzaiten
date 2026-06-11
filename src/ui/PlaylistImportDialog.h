#pragma once

#include "playlist/PlaylistImportWorker.h"

#include <QDialog>
#include <QString>
#include <QVector>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QThread;
class YouTubePlaylistFetcher;

// Import dialog for the playlist view: paste a tracklist (or load an
// m3u/m3u8/csv/txt file), preview how each line resolves against the library,
// then commit. Matching runs on a PlaylistImportWorker thread; the preview
// shows the matched/multi/pending breakdown before anything is written, so a
// bad import can be cancelled without touching the playlist.
class PlaylistImportDialog final : public QDialog {
    Q_OBJECT
public:
    PlaylistImportDialog(const QString &dbPath, const QString &playlistName, QWidget *parent = nullptr);
    ~PlaylistImportDialog() override;

    // Valid after the dialog is accepted: one row per source line.
    QVector<PlaylistImportMatch> results() const { return m_results; }

private slots:
    void loadFile();
    void fetchUrl();
    void runMatch();
    void onProgress(int done, int total);
    void onFinished(QVector<PlaylistImportMatch> results);
    void onError(const QString &message);

private:
    void ensureWorker();
    void rebuildPreview();
    void updateSummary();

    QString m_dbPath;
    QLineEdit *m_urlBox = nullptr;
    QPushButton *m_fetchButton = nullptr;
    YouTubePlaylistFetcher *m_fetcher = nullptr;
    QPlainTextEdit *m_input = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_matchButton = nullptr;
    QPushButton *m_addButton = nullptr;
    QTableWidget *m_preview = nullptr;

    QThread *m_workerThread = nullptr;
    PlaylistImportWorker *m_worker = nullptr;

    QVector<PlaylistImportMatch> m_results;
    bool m_matching = false;
};
