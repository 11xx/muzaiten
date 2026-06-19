#pragma once

#include <QString>

struct Track;

// Display-string helpers coupled to Track: the title/artist/year fallbacks the
// track-list views (queue, library table, file explorer) all need. Kept in one
// place so every view resolves them identically — e.g. a blank title always
// falls back to the filename base, never to an empty cell in one view and the
// path in another.
namespace trackdisplay {

// Track title, falling back to the filename's base name (then the raw filename or
// path) when the title tag is blank.
QString title(const Track &track);

// The artist to show: album artist when set, otherwise the track artist.
QString artist(const Track &track);

// Four-character display year, taken from originalDate, then date. Empty if both
// are blank. Returned as a string so callers can render it or .toInt() it.
QString year(const Track &track);

} // namespace trackdisplay
