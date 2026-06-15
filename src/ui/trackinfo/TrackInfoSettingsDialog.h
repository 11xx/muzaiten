#pragma once

#include <QJsonObject>

class QWidget;

namespace trackinfo {

struct TrackInfoDialogResult {
    bool accepted = false;
    QJsonObject settings;     // edited panel settings (valid when accepted)
    QJsonObject dialogState;  // window size + column widths (always captured)
};

// Run the modal track-information customization dialog, seeded from `current`
// (a TrackInfoPanel::settings() blob, including its saved trackInfoDialog state).
// Returns the edited settings plus the dialog's own UI state to persist.
TrackInfoDialogResult runTrackInfoSettingsDialog(QWidget *parent, const QJsonObject &current);

} // namespace trackinfo
