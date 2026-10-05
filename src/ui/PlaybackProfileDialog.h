#pragma once

#include "playback/PlaybackBackend.h"
#include "playback/PlaybackTypes.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class PlaybackProfileDialog final : public QDialog {
    Q_OBJECT

public:
    explicit PlaybackProfileDialog(QWidget *parent = nullptr);

    // Shared endpoints to offer, and whether the backend can route to one at
    // all. Call before setProfile() so the saved choice can be matched.
    void setSharedEndpoints(const QVector<OutputEndpoint> &endpoints, bool supported);
    // Live description of what the output is doing; may change while open.
    void setOutputStatus(const OutputStatus &status);
    void setProfile(const PlaybackProfile &profile);
    PlaybackProfile profile() const;
    QString releasedDeviceHw() const { return m_releasedDeviceHw; }
    int releasedSinkNodeId() const { return m_releasedSinkNodeId; }

private:
    // The two modes are mutually-exclusive checkable group boxes: activating one
    // unchecks (and so greys out) the other, with exactly one always active.
    bool bitPerfectActive() const;
    void setModeActive(bool bitPerfect);
    // Repopulate the device combo, colouring cards PipeWire is currently
    // holding so a busy device is visible at a glance.
    void populateDevices();
    // Refresh the takeover status line/button for the selected device.
    void refreshDeviceStatus();
    // Take over (free) or release the selected card via AudioDeviceControl.
    void toggleSelectedDeviceTakeover();
    // Select |name| in the output list, adding it as unavailable if absent.
    void selectEndpoint(const QString &name, const QString &label);
    void refreshEndpointControls();

    QGroupBox *m_sharedGroup = nullptr;
    QGroupBox *m_bitPerfectGroup = nullptr;
    QComboBox *m_sink = nullptr;
    QComboBox *m_endpointCombo = nullptr;
    QCheckBox *m_endpointFallback = nullptr;
    QLabel *m_outputStatus = nullptr;
    bool m_endpointsSupported = true;
    QComboBox *m_deviceCombo = nullptr;
    QLabel *m_deviceStatus = nullptr;
    QPushButton *m_deviceAction = nullptr;
    QCheckBox *m_softwareVolume = nullptr;
    QCheckBox *m_allowResample = nullptr;
    QCheckBox *m_releaseSinkOnPause = nullptr;
    QCheckBox *m_autoReleaseDevice = nullptr;
    QSpinBox *m_autoReleaseTimeout = nullptr;
    QSpinBox *m_readAheadMb = nullptr;
    QString m_releasedDeviceHw;
    int m_releasedSinkNodeId = -1;
};
