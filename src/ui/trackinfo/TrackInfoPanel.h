#pragma once

#include "core/Track.h"

#include <QFrame>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class QLabel;

// The "now playing" information pane shown in the right sidebar: a stack of
// configurable lines (title/artist/album/date/<metadata>/file) with per-field
// opacity/size/visibility, alignment, line spacing and overflow behaviour. Owns
// its own settings and its customization dialog; the host persists the settings
// blob via settings()/applySettings().
class TrackInfoPanel final : public QFrame {
    Q_OBJECT

public:
    explicit TrackInfoPanel(QWidget *parent = nullptr);

    void setTrack(const Track &track);

    // The serialized panel settings (trackInfo* keys + saved dialog UI state),
    // merged by the host into its combined view-settings blob.
    QJsonObject settings() const;
    void applySettings(const QJsonObject &root);
    void resetToDefaults();

    // Open the customization dialog; emits settingsChanged() if the user accepts.
    void configure(QWidget *dialogParent);

signals:
    void artistRequested(const QString &artistName);
    void albumRequested(const QString &artistName, const QString &albumTitle);
    void findFileRequested(const Track &track);
    void settingsChanged();

protected:
    void changeEvent(QEvent *event) override;

private:
    void updateTrackInfoLabels();
    void applyTrackInfoSettingsJson(const QJsonObject &root);
    QJsonArray trackInfoSettingsJson() const;
    QJsonArray trackInfoMetadataSettingsJson() const;
    void applyTrackInfoLayoutSpacing();
    void restyleTrackInfoLabels();
    QWidget *trackInfoLabelFromSender() const;
    void showTrackInfoLabelMenu(const QPoint &pos);

    QWidget *m_trackInfoTitle = nullptr;
    QWidget *m_trackInfoArtist = nullptr;
    QWidget *m_trackInfoAlbum = nullptr;
    QWidget *m_trackInfoYear = nullptr;
    QWidget *m_trackInfoFile = nullptr;
    QWidget *m_trackInfoProperties = nullptr;
    QWidget *m_trackInfoTopSpacer = nullptr;
    QWidget *m_trackInfoBottomSpacer = nullptr;
    QLabel *m_noTrackLabel = nullptr;
    Track m_currentTrack;
    QJsonArray m_trackInfoMetadataItems;
    QJsonObject m_trackInfoDialogState;
    QString m_trackInfoMetadataSeparator = QString::fromUtf8("\xc2\xb7");
    int m_trackInfoMetadataSpacing = 1;
    QString m_trackInfoAlignment = QStringLiteral("left");
    QString m_trackInfoLineSpacingMode = QStringLiteral("justify");
    int m_trackInfoLineSpacing = 1;
    QString m_trackInfoVerticalAlignment = QStringLiteral("top");
};
