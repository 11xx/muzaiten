#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

// Pure, explainable per-track scoring for the rule-based radio engine (Stage 1).
// No Qt-SQL dependency: callers build the plain data structs from whatever store
// they like. Every score is the sum of named, signed components, all retained on
// the result so Stage 3 can explain a pick without recomputing anything.
namespace TrackScorer {

// One library track under consideration, reduced to the fields scoring needs.
struct Candidate {
    QString path;
    QString artistFolded;
    QString albumKey;              // folded "albumartist\nalbum"
    QStringList genresFolded;
    int year = 0;                  // 0 = unknown
    int effectiveRating0To100 = -1; // -1 = unrated
    bool hasUserRating = false;
};

// Aggregated listening history for one track (see ListenHistoryStore).
struct Affinity {
    int playEvents = 0;
    int finished = 0;
    int skipped = 0;
    qint64 lastPlayedAtSecs = 0;
    int listenCount = 0;           // local listens + imported listens
    int baselineMax = 0;           // max playcount baseline across services
};

// The rolling mood context a candidate is scored against.
struct SeedContext {
    QStringList genresFolded;          // seed + last few played tracks (folded)
    QSet<QString> recentArtistsFolded; // for the soft same-artist term (not the hard constraint)
    int year = 0;
    qint64 nowSecs = 0;
    int exploration0To100 = 30;        // conservative .. exploratory
};

struct Component {
    QString name;
    double value = 0.0;
};

struct Scored {
    QString path;
    double score = 0.0;
    QList<Component> components;    // retained for explainability (Stage 3)
};

Scored score(const Candidate &candidate, const Affinity &affinity, const SeedContext &seed);

} // namespace TrackScorer
