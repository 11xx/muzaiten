#pragma once

#include <QWidget>

class QLabel;
class QSlider;

class PlayerBar final : public QWidget {
    Q_OBJECT

public:
    explicit PlayerBar(QWidget *parent = nullptr);

    void setTrackText(const QString &text);
    void setTrackInfo(const QString &title, const QString &subtitle, int rating0To100);
    void setTrackInfoPaneVisible(bool visible);
    void setListenBrainzEnabled(bool enabled);
    void setLastFmEnabled(bool enabled);
    void setListUnsupportedFiles(bool show);
    void setExplorerOptionsVisible(bool visible);
    void setCompactMenu(bool compact);
    void setPlaying(bool playing);
    void setPosition(qint64 positionMs, qint64 durationMs);
    void setVolume(int volume0To100);

protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

signals:
    void openLibraryRequested();
    void sourceDirectoriesRequested();
    void scanEnabledSourcesRequested();
    void forceRescanRequested();
    void removeMissingTracksRequested();
    void syncCurrentTrackRatingTagsRequested();
    void syncCurrentArtistRatingTagsRequested();
    void syncAllSavedRatingTagsRequested();
    void retryPendingRatingTagsRequested();
    void playbackProfileRequested();
    void playbackResumeRequested();
    void currentTrackLibraryRequested();
    void linkRootsRequested();
    void mpdSourceRequested();
    void mpdImportRequested();
    void compactMenuChanged(bool compact);
    void listUnsupportedFilesChanged(bool show);
    void trackInfoPaneVisibleChanged(bool visible);
    void trackInfoPaneSettingsRequested();
    void albumArtResolutionRequested();
    void listenBrainzEnabledChanged(bool enabled);
    void listenBrainzTokenRequested();
    void lastFmEnabledChanged(bool enabled);
    void lastFmSettingsRequested();
    void previousRequested();
    void playPauseRequested();
    void nextRequested();
    void stopRequested();
    void seekRequested(qint64 positionMs);
    void volumeChanged(int volume0To100);
    void currentTrackRatingChanged(int rating0To100);

private:
    void restyleMenuBar();
    void updateShuffleIcon();

    class QToolButton *m_menuButton = nullptr;
    class QToolButton *m_volumeButton = nullptr;
    QWidget *m_menuStrip = nullptr;
    class QMenuBar *m_menuBar = nullptr;
    class QToolButton *m_playPause = nullptr;
    class QToolButton *m_shuffle = nullptr;
    class QAction *m_compactMenu = nullptr;
    class QAction *m_listUnsupportedFiles = nullptr;
    class QAction *m_trackInfoPaneVisible = nullptr;
    class QAction *m_listenBrainzEnabled = nullptr;
    class QAction *m_lastFmEnabled = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_subtitle = nullptr;
    QLabel *m_nowPlaying = nullptr;
    QWidget *m_rating = nullptr;
    QLabel *m_elapsed = nullptr;
    QLabel *m_duration = nullptr;
    QSlider *m_progress = nullptr;
    bool m_hasTrack = false;
};
