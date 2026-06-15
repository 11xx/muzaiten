#include "ui/trackinfo/TrackInfoSettings.h"

#include "core/HumanQuantity.h"
#include "core/Track.h"

#include <QFileInfo>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>

#include <algorithm>

namespace trackinfo {

namespace {

QString formatSampleRate(int sampleRateHz)
{
    if (sampleRateHz <= 0) {
        return {};
    }
    if (sampleRateHz % 1000 == 0) {
        return QStringLiteral("%1 kHz").arg(sampleRateHz / 1000);
    }
    return QStringLiteral("%1 kHz").arg(QString::number(sampleRateHz / 1000.0, 'f', 1));
}

QString metadataJoiner(const QString &separator, int spacing)
{
    const QString spaces(std::clamp(spacing, 0, 6), QLatin1Char(' '));
    return separator.isEmpty() ? spaces : spaces + separator + spaces;
}

} // namespace

QVector<TrackInfoField> defaultTrackInfoFields()
{
    return {
        {QStringLiteral("title"), QStringLiteral("Title"), true, 90, 1},
        {QStringLiteral("artist"), QStringLiteral("Artist"), true, 50, 0},
        {QStringLiteral("album"), QStringLiteral("Album"), true, 50, 0},
        {QStringLiteral("date"), QStringLiteral("Date"), true, 40, 0},
        {QStringLiteral("metadata"), QStringLiteral("Metadata"), true, 40, 0},
        {QStringLiteral("file"), QStringLiteral("File full path"), true, 35, -1},
    };
}

QVector<TrackInfoMetadataSpec> availableTrackInfoMetadataItems()
{
    return {
        {QStringLiteral("format"), QStringLiteral("Format"), true, QStringLiteral("always")},
        {QStringLiteral("duration"), QStringLiteral("Duration"), true, QStringLiteral("always")},
        {QStringLiteral("size"), QStringLiteral("File size"), true, QStringLiteral("always")},
        {QStringLiteral("sampleRate"), QStringLiteral("Sample rate"), true, QStringLiteral("notable"), 48},
        {QStringLiteral("bitDepth"), QStringLiteral("Bit depth"), true, QStringLiteral("notable"), 16},
        {QStringLiteral("channels"), QStringLiteral("Channels"), true, QStringLiteral("notable"), 2},
        {QStringLiteral("bitrate"), QStringLiteral("Bitrate"), true, QStringLiteral("lossy")},
        {QStringLiteral("codec"), QStringLiteral("Codec"), false, QStringLiteral("always")},
    };
}

QString metadataLabel(const QString &key)
{
    for (const TrackInfoMetadataSpec &spec : availableTrackInfoMetadataItems()) {
        if (spec.key == key) {
            return spec.label;
        }
    }
    return key;
}

QString metadataDefaultMode(const QString &key)
{
    for (const TrackInfoMetadataSpec &spec : availableTrackInfoMetadataItems()) {
        if (spec.key == key) {
            return spec.defaultMode;
        }
    }
    return QStringLiteral("always");
}

// Items whose "Only if notable" rule compares a measured value against a
// user-editable threshold. Other items have a fixed notable rule (or none).
bool metadataNotableConfigurable(const QString &key)
{
    return key == QStringLiteral("sampleRate")
        || key == QStringLiteral("bitDepth")
        || key == QStringLiteral("channels");
}

int metadataDefaultNotableMin(const QString &key)
{
    for (const TrackInfoMetadataSpec &spec : availableTrackInfoMetadataItems()) {
        if (spec.key == key) {
            return spec.defaultNotableMin;
        }
    }
    return 0;
}

bool isKnownMetadataItem(const QString &key)
{
    for (const TrackInfoMetadataSpec &spec : availableTrackInfoMetadataItems()) {
        if (spec.key == key) {
            return true;
        }
    }
    return false;
}

QJsonArray defaultTrackInfoMetadataItems()
{
    QJsonArray items;
    for (const TrackInfoMetadataSpec &spec : availableTrackInfoMetadataItems()) {
        QJsonObject item;
        item.insert(QStringLiteral("key"), spec.key);
        item.insert(QStringLiteral("visible"), spec.defaultVisible);
        item.insert(QStringLiteral("mode"), spec.defaultMode);
        item.insert(QStringLiteral("notableMin"), spec.defaultNotableMin);
        items.append(item);
    }
    return items;
}

QJsonArray normalizedMetadataItems(const QJsonArray &source)
{
    QJsonArray items;
    QStringList seen;
    for (const QJsonValue &value : source) {
        const QJsonObject object = value.toObject();
        const QString key = object.value(QStringLiteral("key")).toString();
        if (!isKnownMetadataItem(key) || seen.contains(key)) {
            continue;
        }
        QJsonObject item;
        item.insert(QStringLiteral("key"), key);
        item.insert(QStringLiteral("visible"), object.value(QStringLiteral("visible")).toBool(true));
        const QString mode = object.value(QStringLiteral("mode")).toString(metadataDefaultMode(key));
        item.insert(QStringLiteral("mode"), mode.isEmpty() ? metadataDefaultMode(key) : mode);
        item.insert(QStringLiteral("notableMin"),
                    object.value(QStringLiteral("notableMin")).toInt(metadataDefaultNotableMin(key)));
        items.append(item);
        seen.push_back(key);
    }
    for (const TrackInfoMetadataSpec &spec : availableTrackInfoMetadataItems()) {
        if (seen.contains(spec.key)) {
            continue;
        }
        QJsonObject item;
        item.insert(QStringLiteral("key"), spec.key);
        item.insert(QStringLiteral("visible"), spec.defaultVisible);
        item.insert(QStringLiteral("mode"), spec.defaultMode);
        item.insert(QStringLiteral("notableMin"), spec.defaultNotableMin);
        items.append(item);
    }
    return items;
}

QString metadataValueText(const Track &track, const QString &key)
{
    if (key == QStringLiteral("format")) {
        return QFileInfo(track.path).suffix().toUpper();
    }
    if (key == QStringLiteral("duration")) {
        return humanquantity::formatDuration(track.durationMs);
    }
    if (key == QStringLiteral("size")) {
        return humanquantity::formatSize(track.fileSize);
    }
    if (key == QStringLiteral("sampleRate")) {
        return formatSampleRate(track.sampleRateHz);
    }
    if (key == QStringLiteral("bitDepth") && track.bitDepth > 0) {
        return QStringLiteral("%1-bit").arg(track.bitDepth);
    }
    if (key == QStringLiteral("channels") && track.channels > 0) {
        return QStringLiteral("%1 ch").arg(track.channels);
    }
    if (key == QStringLiteral("bitrate") && track.bitrateKbps > 0) {
        return QStringLiteral("%1 kbps").arg(track.bitrateKbps);
    }
    if (key == QStringLiteral("codec")) {
        return track.codec.toUpper();
    }
    return {};
}

bool metadataItemPassesMode(const Track &track, const QString &key, const QString &mode, int notableMin)
{
    if (mode == QStringLiteral("notable")) {
        if (key == QStringLiteral("sampleRate")) {
            return track.sampleRateHz > notableMin * 1000;
        }
        if (key == QStringLiteral("bitDepth")) {
            return track.bitDepth > notableMin;
        }
        if (key == QStringLiteral("channels")) {
            return track.channels > notableMin;
        }
    }
    if (mode == QStringLiteral("lossy")) {
        return track.bitrateKbps > 0 && track.bitDepth <= 0;
    }
    return true;
}

QString metadataText(const Track &track,
                     const QJsonArray &items,
                     const QString &separator,
                     int spacing)
{
    QStringList parts;
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        if (!item.value(QStringLiteral("visible")).toBool(true)) {
            continue;
        }
        const QString key = item.value(QStringLiteral("key")).toString();
        const QString mode = item.value(QStringLiteral("mode")).toString(metadataDefaultMode(key));
        const int notableMin = item.value(QStringLiteral("notableMin")).toInt(metadataDefaultNotableMin(key));
        if (!metadataItemPassesMode(track, key, mode, notableMin)) {
            continue;
        }
        const QString text = metadataValueText(track, key).trimmed();
        if (!text.isEmpty()) {
            parts.push_back(text);
        }
    }
    return parts.join(metadataJoiner(separator, spacing));
}

QString displayDate(const Track &track)
{
    if (!track.originalDate.isEmpty()) {
        return track.originalDate;
    }
    return track.date;
}

Qt::Alignment trackInfoAlignment(const QString &alignment)
{
    if (alignment == QStringLiteral("center")) {
        return Qt::AlignHCenter;
    }
    if (alignment == QStringLiteral("right")) {
        return Qt::AlignRight;
    }
    return Qt::AlignLeft;
}

QString separatorPresetLabel(const QString &separator)
{
    if (separator.isEmpty()) {
        return QStringLiteral("None");
    }
    if (separator == QString::fromUtf8("\xc2\xb7")) {
        return QStringLiteral("Middle dot");
    }
    return QStringLiteral("Custom");
}

} // namespace trackinfo
