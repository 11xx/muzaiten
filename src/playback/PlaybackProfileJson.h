#pragma once

#include "playback/PlaybackTypes.h"

#include <QString>

// The saved form of a PlaybackProfile (the playback.profile setting). Fields
// missing from older saves take their defaults or, for the cross-mode memory,
// the active mode's values.
PlaybackProfile playbackProfileFromJson(const QString &json);
QString playbackProfileToJson(const PlaybackProfile &profile);
