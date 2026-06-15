#pragma once

#include <QJsonArray>
#include <QString>
#include <QVector>
#include <Qt>

struct Track;

// Pure (widget-free) model for the track-information panel: the catalogue of
// fields and metadata items, their defaults, JSON (de)serialization, and the
// predicates that decide what text a track renders. Lives apart from the widgets
// so it can be unit-tested and reused by the settings dialog and the panel alike.
namespace trackinfo {

// A primary line in the panel (title/artist/album/date/metadata/file).
struct TrackInfoField {
    QString key;
    QString label;
    bool visible = true;
    int opacity = 50;
    int sizeDelta = 0;
};

// One technical-metadata token that can appear on the "<Metadata>" line.
struct TrackInfoMetadataSpec {
    QString key;
    QString label;
    bool defaultVisible = true;
    QString defaultMode;
    // Threshold above which the value counts as "notable" (mode == "notable"),
    // expressed in the unit shown to the user: kHz, bits, or channel count. Zero
    // means the item has no configurable notable threshold.
    int defaultNotableMin = 0;
};

QVector<TrackInfoField> defaultTrackInfoFields();
QVector<TrackInfoMetadataSpec> availableTrackInfoMetadataItems();

QString metadataLabel(const QString &key);
QString metadataDefaultMode(const QString &key);
int metadataDefaultNotableMin(const QString &key);
bool metadataNotableConfigurable(const QString &key);
bool isKnownMetadataItem(const QString &key);

QJsonArray defaultTrackInfoMetadataItems();
QJsonArray normalizedMetadataItems(const QJsonArray &source);

QString metadataValueText(const Track &track, const QString &key);
bool metadataItemPassesMode(const Track &track, const QString &key, const QString &mode, int notableMin);
QString metadataText(const Track &track, const QJsonArray &items, const QString &separator, int spacing);

QString displayDate(const Track &track);
Qt::Alignment trackInfoAlignment(const QString &alignment);
QString separatorPresetLabel(const QString &separator);

} // namespace trackinfo
