#pragma once

#include <QString>

class Rating final {
public:
    enum class Source {
        None,
        MusicBeeCompatible,
        VorbisRating,
        Id3Popularimeter,
        Mp4Rate,
        Unknown,
    };

    static constexpr int unset = -1;

    static int normalized0To100(int value);
    static QString displayText(int value0To100);
    static bool isValidStoredValue(int value);
};

