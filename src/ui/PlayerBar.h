#pragma once

#include "player/PlayerCore.h"

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
    void setScrobbleBacklogCounts(int lastFmPending, int listenBrainzPending);
    // running: show live status text + Cancel, disable the start actions.
    // idle: hide status unless statusText carries a last-outcome message,
    // hide Cancel, re-enable the start actions. lbResumable relabels the
    // ListenBrainz action to "Resume…" when a prior import was interrupted.
    void setBackfillStatus(bool running, const QString &statusText, bool lbResumable);
    void setAudioAnalysisRunStatus(bool running, const QString &statusText);
    void setListUnsupportedFiles(bool show);
    // 0 = Background, 1 = Balanced, 2 = Turbo (matches ScanPipeline::Profile order).
    void setScanProfile(int profile);
    void setShowGuessedPlaceholders(bool show);
    void setExplorerOptionsVisible(bool visible);
    void setQueueViewLayoutActive(bool active);
    void setPlaylistViewActionsActive(bool active);
    // Disabled while the queue is mirroring a playlist: merging a saved queue
    // there is almost never intended (it would silently append to the playlist).
    void setMergeSavedQueueEnabled(bool enabled);
    void setAlbumArt(const QString &imagePath);
    void setAlbumArt(const QImage &image);
    void setCompactMenu(bool compact);
    void setAlwaysShowTray(bool enabled);
    // Native DSD can temporarily take a card away from PipeWire. Surface the
    // release command only while muzaiten owns such a takeover.
    void setReleaseDeviceVisible(bool visible);
    void setPlaying(bool playing);
    void setPosition(qint64 positionMs, qint64 durationMs);
    void setVolume(int volume0To100);
    void setVolumeControlEnabled(bool enabled);
    // Reflect externally-driven mode state (persisted settings, MPRIS) on the
    // transport buttons without re-emitting a change request.
    void setRepeatMode(RepeatMode mode);
    void setShuffleMode(ShuffleMode mode);
    // Hidden while no radio session is running; shown (checked) while one is
    // active. Driven by PlayerCore::radioActiveChanged via MainWindow.
    void setRadioActive(bool active);
    // Reflects AppCore::radioAdventurous() on the right-click menu's checkable
    // action; synced by MainWindow on radioMenuAboutToShow. Signal-blocked while
    // set so this never re-emits radioAdventurousChanged.
    void setRadioAdventurous(bool on);
    // Cycle the modes as a left-click would; lets keyboard shortcuts share the
    // exact same behaviour as the buttons.
    void cycleRepeatMode();
    void cycleShuffleMode();
    void scheduleThemeRefresh();

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
    void releaseDeviceRequested();
    // The Playback menu is opening — owners refresh the "Release device" item's
    // availability (it depends on live device-hold state).
    void playbackMenuAboutToShow();
    void currentTrackLibraryRequested();
    void currentTrackFileRequested();
    void linkRootsRequested();
    void mpdSourceRequested();
    void mpdImportRequested();
    void listeningHistoryRequested();
    void scrobblersMenuAboutToShow();
    void lastFmBacklogClearRequested();
    void listenBrainzBacklogClearRequested();
    void backfillStartRequested(QString service);
    void backfillCancelRequested();
    void compactMenuChanged(bool compact);
    void alwaysShowTrayChanged(bool enabled);
    void listUnsupportedFilesChanged(bool show);
    void trackInfoPaneVisibleChanged(bool visible);
    void trackInfoPaneSettingsRequested();
    void albumArtResolutionRequested();
    void playlistMetadataDisplayRequested();
    void searchRankingRequested();
    void memoryReclaimRequested();
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
    void queueSavedLimitRequested();
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
    void repeatModeChangeRequested(RepeatMode mode);
    void shuffleModeChangeRequested(ShuffleMode mode);
    void audioAnalysisStartRequested();
    void audioAnalysisCancelRequested();
    void libraryShuffleSettingsRequested();
    void radioShuffleSettingsRequested();
    void rediscoveryMixRequested();
    void deepCutsMixRequested();
    void stopRadioRequested();
    // The radio button's right-click menu is opening; MainWindow refreshes the
    // "Adventurous" checked state (it depends on live AppCore state) via
    // setRadioAdventurous before it's shown.
    void radioMenuAboutToShow();
    void radioAdventurousChanged(bool on);
    void radioExplorationSettingsRequested();
    void radioBatchSizeSettingsRequested();
    void scoringWeightsRequested();
    void genreCurationRequested();
    void analysisStatusRequested();
    void duplicateCopiesRequested();
    // Menu-bar Radio entries; seeded from whatever is currently playing, so
    // the owner validates there IS a current track and reports otherwise.
    void startRadioFromCurrentRequested();
    void startArtistRadioFromCurrentRequested();
    void aboutRequested();

private:
    void refreshTheme();
    void restyleFrame(bool force = false);
    void restyleMenuBar(bool force = false);
    void updateTransportIcons();
    void updateShuffleIcon();
    void updateRepeatIcon();
    void updateRadioIcon();
    bool shouldHoldTransitionPosition(qint64 positionMs, qint64 durationMs);

    class QToolButton *m_menuButton = nullptr;
    class QToolButton *m_volumeButton = nullptr;
    QWidget *m_menuStrip = nullptr;
    class QMenuBar *m_menuBar = nullptr;
    class QToolButton *m_previous = nullptr;
    class QToolButton *m_playPause = nullptr;
    class QToolButton *m_next = nullptr;
    class QToolButton *m_shuffle = nullptr;
    class QToolButton *m_repeat = nullptr;
    class QToolButton *m_radio = nullptr;
    class QMenu *m_radioMenu = nullptr;
    class QAction *m_radioAdventurousAction = nullptr;
    // Menu-bar mirrors of the radio indicator's session controls; kept in
    // sync with m_radioAdventurousAction by setRadioAdventurous.
    class QAction *m_radioBarAdventurousAction = nullptr;
    class QAction *m_stopRadioAction = nullptr;
    RepeatMode m_repeatMode = RepeatMode::Off;
    ShuffleMode m_shuffleMode = ShuffleMode::Off;
    bool m_radioActive = false;
    class QAction *m_compactMenu = nullptr;
    class QAction *m_alwaysShowTray = nullptr;
    class QAction *m_listUnsupportedFiles = nullptr;
    class QAction *m_scanProfileActions[3] = {nullptr, nullptr, nullptr};
    class QAction *m_showGuessedPlaceholders = nullptr;
    class QAction *m_trackInfoPaneVisible = nullptr;
    class QAction *m_listenBrainzEnabled = nullptr;
    class QAction *m_lastFmEnabled = nullptr;
    class QAction *m_scrobbleOffline = nullptr;
    class QAction *m_clearLastFmBacklog = nullptr;
    class QAction *m_clearListenBrainzBacklog = nullptr;
    class QAction *m_backfillStatusAction = nullptr;
    class QAction *m_importListenBrainzAction = nullptr;
    class QAction *m_syncLastFmAction = nullptr;
    class QAction *m_cancelBackfillAction = nullptr;
    class QAction *m_audioAnalysisRunStatusAction = nullptr;
    class QAction *m_analyzeAudioAction = nullptr;
    class QAction *m_cancelAudioAnalysisAction = nullptr;
    class QAction *m_mergeSavedQueueAction = nullptr;
    class QAction *m_releaseDeviceAction = nullptr;
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
    bool m_isPlaying = false;
    bool m_usingArtFallback = true;
    bool m_themeRefreshPending = false;
    bool m_restylingFrame = false;
    bool m_restylingMenuBar = false;
};
