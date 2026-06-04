#pragma once

#include "playback/PlaybackTypes.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLineEdit;
class QSpinBox;

class PlaybackProfileDialog final : public QDialog {
    Q_OBJECT

public:
    explicit PlaybackProfileDialog(QWidget *parent = nullptr);

    void setProfile(const PlaybackProfile &profile);
    PlaybackProfile profile() const;

private:
    void updateModeVisibility();

    QFormLayout *m_form = nullptr;
    QComboBox *m_mode = nullptr;
    QComboBox *m_sink = nullptr;
    QComboBox *m_deviceCombo = nullptr;
    QCheckBox *m_softwareVolume = nullptr;
    QCheckBox *m_allowResample = nullptr;
    QCheckBox *m_releaseSinkOnPause = nullptr;
    QSpinBox *m_readAheadMb = nullptr;
};
