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
    // Value carried by the item's "Only if ..." condition (mode == valueMode of
    // its MetadataCondition), in the unit shown to the user (kHz / bits / channels
    // / MB / ms). Zero when the item has no value-bearing condition.
    int defaultCondValue = 0;
};

// The widget kind the dialog shows to edit an item's "Only if ..." threshold.
enum class ConditionEditorKind {
    None,      // self-contained mode (lossy / lossless) — no value to edit
    IntSpin,   // a plain integer (kHz / bits / channels / MB)
    Duration,  // a clock value (m:ss / h:mm:ss), stored as milliseconds
};

struct MetadataModeOption {
    QString token;  // stored "mode" value
    QString label;  // dropdown text
};

// Describes the single value-bearing "Only if ..." mode of an item (if any).
struct MetadataCondition {
    QString valueMode;  // the mode token that carries condValue ("" = none editable)
    ConditionEditorKind editor = ConditionEditorKind::None;
    int defaultValue = 0;
    int minValue = 0;
    int maxValue = 0;
    QString prompt;  // e.g. "Sample rate above"
    QString suffix;  // e.g. " kHz"
};

QVector<TrackInfoField> defaultTrackInfoFields();
QVector<TrackInfoMetadataSpec> availableTrackInfoMetadataItems();

QString metadataLabel(const QString &key);
QString metadataDefaultMode(const QString &key);
int metadataDefaultCondValue(const QString &key);
bool isKnownMetadataItem(const QString &key);

// The modes this item offers (contextual — only what actually applies to it).
QVector<MetadataModeOption> metadataModeOptions(const QString &key);
// The item's value-bearing condition (editor kind, range, prompt, suffix).
MetadataCondition metadataCondition(const QString &key);

bool isLosslessFormat(const Track &track);

QJsonArray defaultTrackInfoMetadataItems();
QJsonArray normalizedMetadataItems(const QJsonArray &source);

QString metadataValueText(const Track &track, const QString &key);
bool metadataItemPassesMode(const Track &track, const QString &key, const QString &mode, int condValue);
QString metadataText(const Track &track, const QJsonArray &items, const QString &separator, int spacing);

QString displayDate(const Track &track);
Qt::Alignment trackInfoAlignment(const QString &alignment);
QString separatorPresetLabel(const QString &separator);

} // namespace trackinfo
