#pragma once

#include "playback/PlaybackTypes.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;

class PlaybackProfileDialog final : public QDialog {
    Q_OBJECT

public:
    explicit PlaybackProfileDialog(QWidget *parent = nullptr);

    void setProfile(const PlaybackProfile &profile);
    PlaybackProfile profile() const;

private:
    QComboBox *m_mode = nullptr;
    QComboBox *m_sink = nullptr;
    QLineEdit *m_device = nullptr;
    QCheckBox *m_softwareVolume = nullptr;
    QCheckBox *m_allowResample = nullptr;
};
