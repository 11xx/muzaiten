#pragma once

#include <QString>

#include <functional>
#include <vector>

struct Track;
struct Album;

// Centralized metadata sorting + logical grouping.
//
// A sort is a single user-chosen *primary* SortField plus an automatic,
// curated chain of *tiebreaker* fields so that items sharing the primary value
// fall into a sensible secondary order (grouping) instead of an arbitrary one.
// The chains live in one place (tiebreakChainFor) and are shared by every view.
//
// Direction semantics:
//  - SortDirection flips only the primary comparison by default.
//  - reverseGroups (only meaningful with Descending) also flips the tiebreakers.
namespace MusicSort {

enum class SortField {
    Year,         // originalDate -> date, parsed to a year
    Title,
    AlbumTitle,
    AlbumArtist,
    Artist,
    DiscNumber,
    TrackNumber,
    Rating,       // effectiveRating0To100
    Duration,
    TrackCount,   // album-only
    DateAdded,    // file mtime (track) / MAX(file mtime) (album)
    FileName,
    FileSize,
};

enum class SortDirection {
    Ascending,
    Descending,
};

// Three-way comparisons (-1 / 0 / +1). Fields not applicable to a type return 0
// so they are transparently skipped within a tiebreaker chain.
int compareField(SortField field, const Track &a, const Track &b);
int compareField(SortField field, const Album &a, const Album &b);

// The curated fallback chain for a given primary field (excludes the primary).
const std::vector<SortField> &tiebreakChainFor(SortField primary);

QString sortFieldToString(SortField field);
SortField sortFieldFromString(const QString &value, SortField fallback);

// Builds a strict-weak-ordering comparator for std::sort / std::stable_sort.
template <class T>
std::function<bool(const T &, const T &)>
makeComparator(SortField primary, SortDirection dir, bool reverseGroups)
{
    return [primary, dir, reverseGroups](const T &a, const T &b) {
        int c = compareField(primary, a, b);
        if (dir == SortDirection::Descending) {
            c = -c;
        }
        if (c != 0) {
            return c < 0;
        }
        for (SortField field : tiebreakChainFor(primary)) {
            int t = compareField(field, a, b);
            if (reverseGroups) {
                t = -t;
            }
            if (t != 0) {
                return t < 0;
            }
        }
        return false; // full tie: stable_sort preserves input order
    };
}

} // namespace MusicSort
