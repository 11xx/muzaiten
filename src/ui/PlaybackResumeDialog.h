#pragma once

#include <QDialog>

class QCheckBox;

class PlaybackResumeDialog final : public QDialog {
    Q_OBJECT

public:
    explicit PlaybackResumeDialog(QWidget *parent = nullptr);

    void setSavePositionEnabled(bool enabled);
    bool savePositionEnabled() const;
    void setRestorePlaybackStateEnabled(bool enabled);
    bool restorePlaybackStateEnabled() const;

private:
    QCheckBox *m_savePosition = nullptr;
    QCheckBox *m_restorePlaybackState = nullptr;
};
