#include "core/MetadataBlob.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <zstd.h>

#include <limits>

namespace {

constexpr int kCompressionLevel = 3;

constexpr auto kTagsKey = "tags";
constexpr auto kTechKey = "tech";
constexpr auto kBitrateKey = "bitrate";
constexpr auto kSampleRateKey = "sampleRate";
constexpr auto kChannelsKey = "channels";
constexpr auto kBitDepthKey = "bitDepth";
constexpr auto kCodecKey = "codec";

QByteArray toJson(const MetadataBlob::FullMetadata &metadata)
{
    QJsonObject tags;
    for (auto it = metadata.tags.constBegin(); it != metadata.tags.constEnd(); ++it) {
        QJsonArray values;
        for (const QString &value : it.value()) {
            values.append(value);
        }
        tags.insert(it.key(), values);
    }

    QJsonObject tech;
    tech.insert(QString::fromLatin1(kBitrateKey), metadata.bitrateKbps);
    tech.insert(QString::fromLatin1(kSampleRateKey), metadata.sampleRateHz);
    tech.insert(QString::fromLatin1(kChannelsKey), metadata.channels);
    tech.insert(QString::fromLatin1(kBitDepthKey), metadata.bitDepth);
    tech.insert(QString::fromLatin1(kCodecKey), metadata.codec);

    QJsonObject root;
    root.insert(QString::fromLatin1(kTagsKey), tags);
    root.insert(QString::fromLatin1(kTechKey), tech);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

MetadataBlob::FullMetadata fromJson(const QByteArray &json)
{
    MetadataBlob::FullMetadata metadata;
    const QJsonObject root = QJsonDocument::fromJson(json).object();

    const QJsonObject tags = root.value(QString::fromLatin1(kTagsKey)).toObject();
    for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) {
        QStringList values;
        const QJsonArray array = it.value().toArray();
        values.reserve(array.size());
        for (const QJsonValue &value : array) {
            values.append(value.toString());
        }
        metadata.tags.insert(it.key(), values);
    }

    const QJsonObject tech = root.value(QString::fromLatin1(kTechKey)).toObject();
    metadata.bitrateKbps = tech.value(QString::fromLatin1(kBitrateKey)).toInt();
    metadata.sampleRateHz = tech.value(QString::fromLatin1(kSampleRateKey)).toInt();
    metadata.channels = tech.value(QString::fromLatin1(kChannelsKey)).toInt();
    metadata.bitDepth = tech.value(QString::fromLatin1(kBitDepthKey)).toInt();
    metadata.codec = tech.value(QString::fromLatin1(kCodecKey)).toString();
    return metadata;
}

} // namespace

namespace MetadataBlob {

bool isEmpty(const FullMetadata &metadata)
{
    return metadata.tags.isEmpty() && metadata.bitrateKbps == 0 && metadata.sampleRateHz == 0
        && metadata.channels == 0 && metadata.bitDepth == 0 && metadata.codec.isEmpty();
}

Encoded encode(const FullMetadata &metadata)
{
    const QByteArray json = toJson(metadata);

    Encoded encoded;
    encoded.rawSize = json.size();

    const size_t bound = ZSTD_compressBound(static_cast<size_t>(json.size()));
    if (bound == 0 || bound > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    QByteArray compressed(static_cast<int>(bound), Qt::Uninitialized);
    const size_t written = ZSTD_compress(compressed.data(), bound, json.constData(),
                                         static_cast<size_t>(json.size()), kCompressionLevel);
    if (ZSTD_isError(written)) {
        return {};
    }
    compressed.resize(static_cast<int>(written));
    encoded.data = compressed;
    return encoded;
}

FullMetadata decode(const QByteArray &blob, qint64 rawSize)
{
    if (blob.isEmpty()) {
        return {};
    }

    // Upper bound on a single track's decoded metadata JSON. Tag metadata is
    // only kilobytes in practice; this guards a corrupt/tampered DB blob whose
    // frame header claims an implausible (or >INT_MAX) content size, which would
    // otherwise let ZSTD_decompress write past an under-allocated buffer.
    constexpr unsigned long long kMaxDecodedBytes = 64ull * 1024 * 1024;

    unsigned long long contentSize = ZSTD_getFrameContentSize(blob.constData(), static_cast<size_t>(blob.size()));
    if (contentSize == ZSTD_CONTENTSIZE_ERROR) {
        return {};
    }
    if (contentSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        contentSize = rawSize > 0 ? static_cast<unsigned long long>(rawSize) : 0;
    }
    if (contentSize == 0 || contentSize > kMaxDecodedBytes) {
        return {};
    }

    // Use a single value for both the allocation and the decompress capacity so
    // the two can never disagree (the source of the truncation/overflow risk).
    const int dstSize = static_cast<int>(contentSize);
    QByteArray json(dstSize, Qt::Uninitialized);
    const size_t written = ZSTD_decompress(json.data(), static_cast<size_t>(dstSize), blob.constData(), static_cast<size_t>(blob.size()));
    if (ZSTD_isError(written)) {
        return {};
    }
    json.resize(static_cast<int>(written));
    return fromJson(json);
}

} // namespace MetadataBlob
