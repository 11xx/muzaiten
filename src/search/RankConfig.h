#pragma once

// User-configurable "ranking": an ordered list of criteria that re-sort matched
// search results (front-end), plus a list of exclusion rules (engine filter).
//
// Higher in the list = stronger. Relevance is itself a criterion, so the user
// can place quality/library-order above or below it.

#include "core/MusicSort.h"
#include "search/Exclusion.h"

#include <QString>
#include <QVector>

class QJsonObject;

namespace Search {

enum class RankKind {
    Relevance,           // the engine's match score
    AudioQuality,        // lossless / sample-rate / bitrate composite
    PreferredDirectory,  // boost results whose path starts with `param`
    LibraryOrder,        // full MusicSort chain from a primary field
    MusicField,          // a single MusicSort field
};

struct RankRule {
    RankKind                 kind = RankKind::Relevance;
    MusicSort::SortField     field = MusicSort::SortField::AlbumArtist; // for LibraryOrder/MusicField
    QString                  param;        // path prefix for PreferredDirectory
    MusicSort::SortDirection dir = MusicSort::SortDirection::Descending;
    bool                     enabled = true;
};

struct RankConfig {
    QVector<RankRule>   rules;
    QVector<ExcludeRule> excludes;

    bool isEmpty() const { return rules.isEmpty() && excludes.isEmpty(); }

    // The built-in default: Relevance, then Audio quality, then library order.
    static RankConfig defaultConfig();

    QJsonObject toJson() const;
    static RankConfig fromJson(const QJsonObject &obj);
    static RankConfig fromJsonString(const QString &json); // falls back to defaultConfig()
    QString toJsonString() const;

    // String<->enum helpers (also used by the dialog combos).
    static QString rankKindToString(RankKind k);
    static RankKind rankKindFromString(const QString &s, RankKind fallback);
};

} // namespace Search
