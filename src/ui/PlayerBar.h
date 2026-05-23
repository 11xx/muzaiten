#pragma once

#include <QWidget>

class QLabel;
class QSlider;

class PlayerBar final : public QWidget {
    Q_OBJECT

public:
    explicit PlayerBar(QWidget *parent = nullptr);

private:
    QLabel *m_nowPlaying = nullptr;
    QLabel *m_elapsed = nullptr;
    QLabel *m_duration = nullptr;
    QSlider *m_progress = nullptr;
};

