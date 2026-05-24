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
    void setListenBrainzEnabled(bool enabled);
    void setPlaying(bool playing);
    void setPosition(qint64 positionMs, qint64 durationMs);

protected:
    void resizeEvent(QResizeEvent *event) override;

signals:
    void openLibraryRequested();
    void linkRootsRequested();
    void listenBrainzEnabledChanged(bool enabled);
    void listenBrainzTokenRequested();
    void previousRequested();
    void playPauseRequested();
    void nextRequested();
    void stopRequested();
    void seekRequested(qint64 positionMs);
    void volumeChanged(int volume0To100);
    void currentTrackRatingChanged(int rating0To100);

private:
    class QToolButton *m_menuButton = nullptr;
    class QToolButton *m_playPause = nullptr;
    class QAction *m_listenBrainzEnabled = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_subtitle = nullptr;
    QLabel *m_nowPlaying = nullptr;
    QWidget *m_rating = nullptr;
    QLabel *m_elapsed = nullptr;
    QLabel *m_duration = nullptr;
    QSlider *m_progress = nullptr;
    bool m_hasTrack = false;
};
