#include "scanner/TagRating.h"

#include <QString>
#include <QStringList>

#include <cmath>

#include <taglib/tstring.h>
#include <taglib/tstringlist.h>

namespace {

QString toQString(const TagLib::String &value)
{
    return QString::fromStdString(value.to8Bit(true));
}

QString firstProperty(const TagLib::PropertyMap &properties, const QStringList &keys)
{
    for (const QString &key : keys) {
        const TagLib::String tagKey(key.toStdString(), TagLib::String::UTF8);
        const auto values = properties[tagKey];
        if (!values.isEmpty()) {
            const QString value = toQString(values.front()).trimmed();
            if (!value.isEmpty()) {
                return value;
            }
        }
    }
    return {};
}

} // namespace

TagRatingReadResult readRating(const TagLib::PropertyMap &properties)
{
    TagRatingReadResult result;

    const QString rating = firstProperty(properties, {QStringLiteral("RATING")});
    if (!rating.isEmpty()) {
        bool ok = false;
        const int parsed = rating.toInt(&ok);
        if (ok) {
            result.rating0To100 = Rating::normalized0To100(parsed);
            result.source = Rating::Source::MusicBeeCompatible;
            return result;
        }
    }

    const QString fmps = firstProperty(properties, {QStringLiteral("FMPS_RATING")});
    if (!fmps.isEmpty()) {
        bool ok = false;
        const double parsed = fmps.toDouble(&ok);
        if (ok) {
            result.rating0To100 = Rating::normalized0To100(static_cast<int>(std::lround(parsed * 100.0)));
            result.source = Rating::Source::VorbisRating;
        }
    }

    return result;
}

void setMusicBeeRating(TagLib::PropertyMap &properties, int rating0To100)
{
    const int normalized = Rating::normalized0To100(rating0To100);
    properties.replace(TagLib::String("RATING", TagLib::String::UTF8),
                       TagLib::StringList(TagLib::String(QString::number(normalized).toStdString(), TagLib::String::UTF8)));
}
