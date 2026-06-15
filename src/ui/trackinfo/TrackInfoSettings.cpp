#include "ui/trackinfo/TrackInfoSettings.h"

#include "core/HumanQuantity.h"
#include "core/Track.h"

#include <QFileInfo>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
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
        {QStringLiteral("format"), QStringLiteral("Format"), true, QStringLiteral("always"), 0},
        {QStringLiteral("duration"), QStringLiteral("Duration"), true, QStringLiteral("always"), 300000},
        {QStringLiteral("size"), QStringLiteral("File size"), true, QStringLiteral("always"), 50},
        {QStringLiteral("sampleRate"), QStringLiteral("Sample rate"), true, QStringLiteral("notable"), 48},
        {QStringLiteral("bitDepth"), QStringLiteral("Bit depth"), true, QStringLiteral("notable"), 16},
        {QStringLiteral("channels"), QStringLiteral("Channels"), true, QStringLiteral("notable"), 2},
        {QStringLiteral("bitrate"), QStringLiteral("Bitrate"), true, QStringLiteral("lossy"), 0},
        {QStringLiteral("codec"), QStringLiteral("Codec"), false, QStringLiteral("always"), 0},
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

int metadataDefaultCondValue(const QString &key)
{
    for (const TrackInfoMetadataSpec &spec : availableTrackInfoMetadataItems()) {
        if (spec.key == key) {
            return spec.defaultCondValue;
        }
    }
    return 0;
}

QVector<MetadataModeOption> metadataModeOptions(const QString &key)
{
    const MetadataModeOption always{QStringLiteral("always"), QStringLiteral("Always")};
    if (key == QStringLiteral("format")) {
        return {always,
                {QStringLiteral("formatLossy"), QStringLiteral("Only if lossy")},
                {QStringLiteral("formatLossless"), QStringLiteral("Only if lossless")}};
    }
    if (key == QStringLiteral("duration")) {
        return {always, {QStringLiteral("durationOver"), QStringLiteral("Only if longer than")}};
    }
    if (key == QStringLiteral("size")) {
        return {always, {QStringLiteral("sizeOver"), QStringLiteral("Only if larger than")}};
    }
    if (key == QStringLiteral("sampleRate") || key == QStringLiteral("bitDepth") || key == QStringLiteral("channels")) {
        return {always, {QStringLiteral("notable"), QStringLiteral("Only if above")}};
    }
    if (key == QStringLiteral("bitrate")) {
        return {always, {QStringLiteral("lossy"), QStringLiteral("Only if lossy")}};
    }
    return {always};
}

MetadataCondition metadataCondition(const QString &key)
{
    if (key == QStringLiteral("sampleRate")) {
        return {QStringLiteral("notable"), ConditionEditorKind::IntSpin, 48, 1, 768, QStringLiteral("Sample rate above"), QStringLiteral(" kHz")};
    }
    if (key == QStringLiteral("bitDepth")) {
        return {QStringLiteral("notable"), ConditionEditorKind::IntSpin, 16, 1, 64, QStringLiteral("Bit depth above"), QStringLiteral("-bit")};
    }
    if (key == QStringLiteral("channels")) {
        return {QStringLiteral("notable"), ConditionEditorKind::IntSpin, 2, 1, 32, QStringLiteral("Channels above"), QStringLiteral(" ch")};
    }
    if (key == QStringLiteral("size")) {
        return {QStringLiteral("sizeOver"), ConditionEditorKind::IntSpin, 50, 1, 100000, QStringLiteral("Larger than"), QStringLiteral(" MB")};
    }
    if (key == QStringLiteral("duration")) {
        return {QStringLiteral("durationOver"), ConditionEditorKind::Duration, 300000, 0, 0, QStringLiteral("Longer than"), QString()};
    }
    return {};
}

bool isLosslessFormat(const Track &track)
{
    static const QSet<QString> lossless = {
        QStringLiteral("flac"), QStringLiteral("wav"), QStringLiteral("wave"), QStringLiteral("alac"),
        QStringLiteral("ape"), QStringLiteral("wv"), QStringLiteral("tta"), QStringLiteral("aiff"),
        QStringLiteral("aif"), QStringLiteral("aifc"), QStringLiteral("dsf"), QStringLiteral("dff"),
        QStringLiteral("shn"),
    };
    static const QSet<QString> lossy = {
        QStringLiteral("mp3"), QStringLiteral("aac"), QStringLiteral("ogg"), QStringLiteral("oga"),
        QStringLiteral("opus"), QStringLiteral("wma"), QStringLiteral("mpc"), QStringLiteral("ac3"),
        QStringLiteral("eac3"), QStringLiteral("dts"), QStringLiteral("amr"), QStringLiteral("ra"),
    };
    const QString ext = QFileInfo(track.path).suffix().toLower();
    if (lossless.contains(ext)) {
        return true;
    }
    if (lossy.contains(ext)) {
        return false;
    }
    // Ambiguous container (e.g. m4a): a known bit depth implies lossless.
    return track.bitDepth > 0;
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
        item.insert(QStringLiteral("condValue"), spec.defaultCondValue);
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
        // condValue replaced the older notableMin key; read the legacy name as a
        // fallback so existing saved settings keep their threshold.
        const int legacy = object.value(QStringLiteral("notableMin")).toInt(metadataDefaultCondValue(key));
        item.insert(QStringLiteral("condValue"), object.value(QStringLiteral("condValue")).toInt(legacy));
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
        item.insert(QStringLiteral("condValue"), spec.defaultCondValue);
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

bool metadataItemPassesMode(const Track &track, const QString &key, const QString &mode, int condValue)
{
    if (mode == QStringLiteral("notable")) {
        if (key == QStringLiteral("sampleRate")) {
            return track.sampleRateHz > condValue * 1000;
        }
        if (key == QStringLiteral("bitDepth")) {
            return track.bitDepth > condValue;
        }
        if (key == QStringLiteral("channels")) {
            return track.channels > condValue;
        }
    }
    if (mode == QStringLiteral("lossy")) {
        return track.bitrateKbps > 0 && track.bitDepth <= 0;
    }
    if (mode == QStringLiteral("durationOver")) {
        return track.durationMs > static_cast<qint64>(condValue);
    }
    if (mode == QStringLiteral("sizeOver")) {
        return track.fileSize > static_cast<qint64>(condValue) * 1000000;
    }
    if (mode == QStringLiteral("formatLossy")) {
        return !isLosslessFormat(track);
    }
    if (mode == QStringLiteral("formatLossless")) {
        return isLosslessFormat(track);
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
        const int legacy = item.value(QStringLiteral("notableMin")).toInt(metadataDefaultCondValue(key));
        const int condValue = item.value(QStringLiteral("condValue")).toInt(legacy);
        if (!metadataItemPassesMode(track, key, mode, condValue)) {
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
