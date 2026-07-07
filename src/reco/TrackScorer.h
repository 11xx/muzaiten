#pragma once

#include <QByteArray>
#include <QHash>
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
    qint64 contentGroupId = -1;     // -1 = no features.sqlite content group
    QString songKey;
    QString artistFolded;
    QString albumKey;              // folded "albumartist\nalbum"
    QStringList genresFolded;
    int year = 0;                  // 0 = unknown
    double tempoBpm = -1.0;         // -1 = unknown
    double energy = -1.0;           // -1 = unknown, expected unit-ish extractor scale
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
    // Folded genre -> IDF weight (log(taggedLibraryTracks / trackCount(genre))),
    // covering the full library vocabulary so rolling-context genres picked up
    // from played tracks (not just the seed's) resolve too. A genre absent from
    // this map (including an empty map altogether) scores as IDF 0 — it
    // contributes nothing to the genre component. See TrackScorer.cpp for how
    // this is turned into the genre score.
    QHash<QString, double> genreIdf;
    QSet<QString> recentArtistsFolded; // for the soft same-artist term (not the hard constraint)
    int year = 0;
    double contextTempoBpm = -1.0;
    double contextEnergy = -1.0;
    QVector<float> audioCentroid;       // L2-normalized; empty = unknown
    const QHash<qint64, QVector<float>> *embeddingsByGroup = nullptr;
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

// Runtime-tunable scorer weights. AppCore reads a JSON object from the
// library-DB setting `radio.scoringWeights`; omitted fields keep these defaults.
struct Weights {
    double genreWeight = 3.0;
    double genreIdfSaturation = 4.0;
    double genreCrowdingSoftLimit = 3.0;
    double eraWeight = 1.0;
    double eraSpanYears = 30.0;
    double tempoWeight = 0.4;
    double energyWeight = 0.6;
    double audioWeight = 1.2;
    double ratingWeight = 1.5;
    double userRatingBoost = 1.25;
    double historyWeight = 1.0;
    double historySaturation = 50.0;
    double noveltyWeight = 0.8;
    double noveltyZeroAt = 10.0;
    double recencyPenalty = -2.0;
    double recencyHalfLifeDays = 14.0;
    double skipPenalty = -2.5;
    double sameArtistPenalty = -0.6;
};

Weights defaultWeights();
Weights weightsFromJson(const QByteArray &json, QString *error = nullptr);
QByteArray weightsToJson(const Weights &weights);

Scored score(const Candidate &candidate, const Affinity &affinity, const SeedContext &seed);
Scored score(const Candidate &candidate, const Affinity &affinity, const SeedContext &seed,
             const Weights &weights);

} // namespace TrackScorer
