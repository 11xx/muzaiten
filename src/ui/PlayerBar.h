#pragma once

#include <QWidget>

class QLabel;
class QSlider;

class PlayerBar final : public QWidget {
    Q_OBJECT

public:
    explicit PlayerBar(QWidget *parent = nullptr);

    void setTrackText(const QString &text);
    void setPlaying(bool playing);
    void setPosition(qint64 positionMs, qint64 durationMs);

signals:
    void playPauseRequested();
    void stopRequested();
    void seekRequested(qint64 positionMs);

private:
    class QToolButton *m_playPause = nullptr;
    QLabel *m_nowPlaying = nullptr;
    QLabel *m_elapsed = nullptr;
    QLabel *m_duration = nullptr;
    QSlider *m_progress = nullptr;
    bool m_hasTrack = false;
};
