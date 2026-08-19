#pragma once

#include "core/Track.h"
#include "reco/TrackScorer.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QRandomGenerator;

// A radio recommendation session: turns a scored candidate pool into a
// constraint-sequenced stream of picks. Seeded Start Radio sessions keep the
// seed's genres as a mood anchor; anchorless sessions start without a seed and
// let notePlayed() build the mood solely from the rolling listening context.
// Sequencing is the hard part — a deterministic top-1 queue feels dead — so
// picks are a weighted-random draw among the top-scoring candidates, subject to
// hard constraints (artist/album throttling, no repeats) that are enforced
// before scoring rather than as score terms. Not a QObject: PlayerCore's
// provider closure calls it on the main thread. Not thread-safe.
class RadioSession final {
public:
    enum class ContextMode { PermanentAnchor, MovingContext };

    struct PickReason {
        QString path;
        QList<TrackScorer::Component> components;
    };

    // The seed candidate anchors the mood: its genres are always part of the
    // rolling genre window (stoplisted placeholder genres are filtered out
    // first — see GenreTags::informative). `genreIdf` is the library-wide
    // folded-genre -> IDF map (see TrackScorer::SeedContext::genreIdf); an
    // empty map makes every genre score as IDF 0. `nowSecs` fixes "now" for
    // recency scoring (injected so tests are deterministic). `rng` defaults to
    // the global generator; tests pass a privately-seeded one for reproducible
    // picks.
    RadioSession(QVector<TrackScorer::Candidate> pool,
                 QHash<QString, TrackScorer::Affinity> affinities,
                 QHash<QString, double> genreIdf,
                 TrackScorer::Candidate seed,
                 int exploration0To100,
                 qint64 nowSecs,
                 QRandomGenerator *rng = nullptr,
                 TrackScorer::Weights weights = TrackScorer::defaultWeights(),
                 QHash<qint64, QVector<float>> embeddingsByGroup = {},
                 TrackScorer::RadioSessionDecay sessionDecay = TrackScorer::defaultSessionDecay());

    // Anchorless ambient-radio mode: no fixed seed, so rollingGenres() starts
    // empty and becomes the last few notePlayed() tracks.
    RadioSession(QVector<TrackScorer::Candidate> pool,
                 QHash<QString, TrackScorer::Affinity> affinities,
                 QHash<QString, double> genreIdf,
                 int exploration0To100,
                 qint64 nowSecs,
                 QRandomGenerator *rng = nullptr,
                 TrackScorer::Weights weights = TrackScorer::defaultWeights(),
                 QHash<qint64, QVector<float>> embeddingsByGroup = {},
                 TrackScorer::RadioSessionDecay sessionDecay = TrackScorer::defaultSessionDecay());

    RadioSession(QVector<TrackScorer::Candidate> pool,
                 QHash<QString, TrackScorer::Affinity> affinities,
                 QHash<QString, double> genreIdf,
                 QVector<TrackScorer::Candidate> anchors,
                 ContextMode contextMode,
                 int exploration0To100,
                 qint64 nowSecs,
                 QRandomGenerator *rng = nullptr,
                 TrackScorer::Weights weights = TrackScorer::defaultWeights(),
                 QHash<qint64, QVector<float>> embeddingsByGroup = {},
                 TrackScorer::RadioSessionDecay sessionDecay = TrackScorer::defaultSessionDecay());

    // Up to `count` picks scored against the CURRENT rolling context, resolved to
    // full Tracks via `resolveTrack`. `excludePaths` (typically the live queue)
    // and the session's own already-picked/played paths are hard-excluded.
    QVector<Track> nextTracks(int count, const QSet<QString> &excludePaths,
                              const std::function<Track(const QString &path)> &resolveTrack);

    // Batch generation can score on a worker using path-only placeholder
    // Tracks, then resolve the selected paths to the preferred playable copies
    // on the GUI thread. Teach the completed session about that substitution so
    // explanations, repeat prevention, and later notePlayed() calls follow the
    // path that actually entered the queue.
    void aliasResolvedPath(const QString &candidatePath, const QString &resolvedPath);

    // Reconcile queued generated identities without confirming any survivor;
    // returns whether the ordered pending list changed.
    bool retainPendingPaths(const QStringList &orderedPaths);

    // Feed every track that actually becomes current while radio is active
    // (the seed, radio picks, and user-queued interruptions). Advances the
    // rolling context: last-3 artists, the last-3-played genre window, and the
    // per-session album counts.
    void notePlayed(const Track &track);

    // Live-updates the exploration knob (e.g. the player-bar "Adventurous" boost
    // or a persisted-setting change). SeedContext::exploration0To100 is rebuilt
    // fresh from m_exploration inside nextTracks() on every call, so this takes
    // effect starting with the NEXT pick — never retroactively rescoring picks
    // already handed out.
    void setExploration(int exploration0To100);
    void setWeights(TrackScorer::Weights weights);
    void setSessionDecay(TrackScorer::RadioSessionDecay decay);

    // Terse, data-driven explanation for a pick made this session (component
    // names + rounded contributions). Empty when the path was never picked here.
    QString reasonFor(const QString &path) const;

    // Stored scorer components for a pick made this session. Empty when unknown.
    QList<TrackScorer::Component> reasonComponentsFor(const QString &path) const;

    // Pick explanations in generation order. Deliberately live-only: restored
    // sessions resume sequencing but do not resurrect pre-restart explanations.
    QVector<PickReason> pickReasons() const;

    // Constraint-only session persistence. Pick reasons are deliberately not
    // included: restored pre-restart rows can continue sequencing correctly, but
    // only new picks have freshly computed explanations.
    QJsonObject constraintState() const;

    // Counts every change to state a later batch would be scored against: the
    // owned RNG, the anchor cursor, recorded picks, resolved-path aliases and
    // retained pending paths.
    //
    // A batch can mutate all of that and still return nothing, when every
    // candidate it chose fails to resolve. Callers that decide whether an
    // in-flight batch's snapshot is now stale must compare this rather than
    // whether picks came back, or a queue-silent mutation goes unnoticed.
    quint64 mutationGeneration() const { return m_mutationGeneration; }
    void restoreConstraintState(const QJsonObject &state);

    // Pure classifier for AppCore's radio re-roll heuristic: true when a play
    // ended before crossing the scrobble threshold (half the track's duration,
    // capped at 4 minutes) -- the same rule ListenTracker and
    // ListenHistoryStore::trackAffinities use to separate "listened" from
    // "rejected". Callers are responsible for checking outcome == "skipped" and
    // source == "radio" themselves; this only judges the timing.
    static bool isEarlySkip(qint64 playedMs, qint64 durationMs);

private:
    struct PlayedScalars {
        double tempoBpm = -1.0;
        double energy = -1.0;
    };

    struct ConfirmedContextEntry {
        QString path;
        double weight = 0.0;
    };

    TrackScorer::SeedContext movingContext(const TrackScorer::Candidate *anchor) const;
    TrackScorer::SeedContext permanentMultiContext(const TrackScorer::Candidate *anchor,
                                                   const QStringList &recentArtists) const;
    const TrackScorer::Candidate *nextAnchor();
    void consumeAnchor();
    QStringList pendingArtists() const;
    bool anchorExcluded(const TrackScorer::Candidate &candidate) const;
    double drawRandomDouble();
    quint64 nextOwnedRandom();
    quint64 ownedBounded(quint64 bound);

    // Rolling genre window: seed genres unioned with the last few played tracks'
    // genres — the seed anchors the mood, the window lets it drift.
    QStringList rollingGenres() const;
    double rollingTempoBpm() const;
    double rollingEnergy() const;
    QVector<float> rollingAudioCentroid() const;
    // Score-ordered eligible candidates for the current context, honoring the hard
    // constraints (excludePaths + the per-batch recent-artist list).
    void recordPick(const TrackScorer::Candidate &candidate, const TrackScorer::Scored &scored,
                    const QString &resolvedPath);

    quint64 m_mutationGeneration = 0;
    ContextMode m_contextMode = ContextMode::PermanentAnchor;
    bool m_vectorAnchorSession = false;
    QVector<TrackScorer::Candidate> m_anchors;
    QSet<QString> m_anchorPaths;
    QSet<QString> m_anchorSongKeys;
    QVector<ConfirmedContextEntry> m_confirmedContext;
    QHash<QString, QString> m_contextSourcePaths;
    QStringList m_pendingPaths;
    QVector<int> m_anchorRoundOrder;
    int m_anchorCursor = 0;
    quint64 m_ownedRngState = 0;
    bool m_usesOwnedRng = false;

    QVector<TrackScorer::Candidate> m_pool;
    QHash<QString, TrackScorer::Affinity> m_affinities;
    QHash<QString, TrackScorer::Candidate> m_byPath; // pool + anchors, for notePlayed lookups
    QHash<QString, double> m_genreIdf;
    QHash<qint64, QVector<float>> m_embeddingsByGroup;
    TrackScorer::Candidate m_seed;
    TrackScorer::Weights m_weights;
    TrackScorer::RadioSessionDecay m_sessionDecay = TrackScorer::defaultSessionDecay();
    int m_generatedPickCount = 0;
    int m_exploration = 30;
    qint64 m_nowSecs = 0;
    QRandomGenerator *m_rng = nullptr;

    QStringList m_recentArtists;              // last 3 played/picked (folded, consecutive-deduped)
    QList<QStringList> m_playedGenres;        // last 3 played tracks' folded genres
    QList<PlayedScalars> m_playedScalars;      // last 3 played tracks' known acoustic scalars
    QList<qint64> m_playedContentGroups;       // last 3 played tracks' feature groups (-1 = unknown)
    QHash<QString, int> m_albumCounts;        // albumKey -> tracks committed this session
    QSet<QString> m_usedPaths;                // never pick/repeat a path twice
    QSet<QString> m_usedSongKeys;             // never pick/repeat a song twice through duplicate files
    QHash<QString, QList<TrackScorer::Component>> m_pickReasons;
    QStringList m_pickReasonOrder;
};
