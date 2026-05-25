#include "scanner/TagRatingWriter.h"

#include "scanner/TagRating.h"
#include "scanner/TagReader.h"

#include <taglib/fileref.h>

TagRatingWriteResult TagRatingWriter::writeRating(const QString &path, int rating0To100) const
{
    TagRatingWriteResult result;
    const int normalized = Rating::normalized0To100(rating0To100);
    if (!Rating::isValidStoredValue(normalized) || normalized < 0) {
        result.error = QStringLiteral("Invalid rating");
        return result;
    }

    TagLib::FileRef file(path.toUtf8().constData(), false);
    if (file.isNull() || file.file() == nullptr) {
        result.error = QStringLiteral("TagLib could not open file");
        return result;
    }

    TagLib::PropertyMap properties = file.file()->properties();
    const TagRatingReadResult existing = readRating(properties);
    if (existing.rating0To100 >= 0) {
        result.existingTagWon = true;
        result.fileRating0To100 = existing.rating0To100;
        return result;
    }

    setMusicBeeRating(properties, normalized);
    file.file()->setProperties(properties);
    if (!file.file()->save()) {
        result.error = QStringLiteral("TagLib could not save file");
        return result;
    }

    const Track reread = TagReader().read(path);
    if (reread.rating0To100 != normalized) {
        result.error = QStringLiteral("Written rating was not present after re-read");
        result.fileRating0To100 = reread.rating0To100;
        return result;
    }

    result.ok = true;
    result.fileRating0To100 = normalized;
    return result;
}
