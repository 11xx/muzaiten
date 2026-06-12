#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QWidget>

class QLabel;
class QSlider;
class QImage;
class AlbumArtView;

class PlayerBar final : public QWidget {
    Q_OBJECT

public:
    explicit PlayerBar(QWidget *parent = nullptr);

    void setTrackText(const QString &text);
    void setTrackInfo(const QString &title, const QString &subtitle, int rating0To100);
    void setTrackInfoPaneVisible(bool visible);
    void setListenBrainzEnabled(bool enabled);
    void setLastFmEnabled(bool enabled);
    void setScrobbleOffline(bool offline);
    void setListUnsupportedFiles(bool show);
    // 0 = Background, 1 = Balanced, 2 = Turbo (matches ScanPipeline::Profile order).
    void setScanProfile(int profile);
    void setShowGuessedPlaceholders(bool show);
    void setExplorerOptionsVisible(bool visible);
    void setQueueViewLayoutActive(bool active);
    void setPlaylistViewActionsActive(bool active);
    void setAlbumArt(const QString &imagePath);
    void setAlbumArt(const QImage &image);
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
    void scanProfileChanged(int profile);
    void showGuessedPlaceholdersChanged(bool show);
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
    void searchRankingRequested();
    void keybindingsRequested();
    void resetPanelOrderRequested();
    void resetViewPreferencesRequested();
    void panelOrderRequested();
    void queueViewRequested();
    void queueClearRequested();
    void queueClearPlayNextPriorityRequested();
    void queueSaveAsRequested();
    void queueRestorePreviousRequested();
    void queueMergeSavedRequested();
    void playlistViewRequested();
    void playlistNewRequested();
    void playlistAddSongRequested();
    void playlistPlayRequested();
    void playlistPlayNextRequested();
    void playlistAddToQueueRequested();
    void playlistRenameRequested();
    void playlistExportRequested();
    void playlistDeleteRequested();
    void playlistMoveItemUpRequested();
    void playlistMoveItemDownRequested();
    void listenBrainzEnabledChanged(bool enabled);
    void listenBrainzTokenRequested();
    void lastFmEnabledChanged(bool enabled);
    void lastFmSettingsRequested();
    void scrobbleOfflineChanged(bool offline);
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
    bool shouldHoldTransitionPosition(qint64 positionMs, qint64 durationMs);

    class QToolButton *m_menuButton = nullptr;
    class QToolButton *m_volumeButton = nullptr;
    QWidget *m_menuStrip = nullptr;
    class QMenuBar *m_menuBar = nullptr;
    class QToolButton *m_playPause = nullptr;
    class QToolButton *m_shuffle = nullptr;
    class QAction *m_compactMenu = nullptr;
    class QAction *m_listUnsupportedFiles = nullptr;
    class QAction *m_scanProfileActions[3] = {nullptr, nullptr, nullptr};
    class QAction *m_showGuessedPlaceholders = nullptr;
    class QAction *m_trackInfoPaneVisible = nullptr;
    class QAction *m_listenBrainzEnabled = nullptr;
    class QAction *m_lastFmEnabled = nullptr;
    class QAction *m_scrobbleOffline = nullptr;
    QList<class QAction *> m_playlistViewActions;
    QLabel *m_title = nullptr;
    QLabel *m_subtitle = nullptr;
    QLabel *m_nowPlaying = nullptr;
    QWidget *m_rating = nullptr;
    AlbumArtView *m_albumArt = nullptr;
    QLabel *m_elapsed = nullptr;
    QLabel *m_duration = nullptr;
    QSlider *m_progress = nullptr;
    QElapsedTimer m_trackStartGuardTimer;
    qint64 m_lastProgressPositionMs = 0;
    qint64 m_lastProgressDurationMs = -1;
    bool m_trackStartGuardActive = false;
    bool m_hasTrack = false;
    bool m_usingArtFallback = true;
};
