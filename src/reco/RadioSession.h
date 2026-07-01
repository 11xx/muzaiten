#pragma once

#include "core/Track.h"
#include "reco/TrackScorer.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QRandomGenerator;

// A single Start Radio session: turns a scored candidate pool into a
// constraint-sequenced stream of picks. Sequencing is the hard part — a
// deterministic top-1 queue feels dead — so picks are a weighted-random draw
// among the top-scoring candidates, subject to hard constraints (artist/album
// throttling, no repeats) that are enforced before scoring rather than as score
// terms. Not a QObject: PlayerCore's provider closure calls it on the main
// thread. Not thread-safe.
class RadioSession final {
public:
    // The seed candidate anchors the mood: its genres are always part of the
    // rolling genre window. `nowSecs` fixes "now" for recency scoring (injected
    // so tests are deterministic). `rng` defaults to the global generator; tests
    // pass a privately-seeded one for reproducible picks.
    RadioSession(QVector<TrackScorer::Candidate> pool,
                 QHash<QString, TrackScorer::Affinity> affinities,
                 TrackScorer::Candidate seed,
                 int exploration0To100,
                 qint64 nowSecs,
                 QRandomGenerator *rng = nullptr);

    // Up to `count` picks scored against the CURRENT rolling context, resolved to
    // full Tracks via `resolveTrack`. `excludePaths` (typically the live queue)
    // and the session's own already-picked/played paths are hard-excluded.
    QVector<Track> nextTracks(int count, const QSet<QString> &excludePaths,
                              const std::function<Track(const QString &path)> &resolveTrack);

    // Feed every track that actually becomes current while radio is active
    // (the seed, radio picks, and user-queued interruptions). Advances the
    // rolling context: last-3 artists, the last-3-played genre window, and the
    // per-session album counts.
    void notePlayed(const Track &track);

    // Terse, data-driven explanation for a pick made this session (component
    // names + rounded contributions). Empty when the path was never picked here.
    QString reasonFor(const QString &path) const;

private:
    // Rolling genre window: seed genres unioned with the last few played tracks'
    // genres — the seed anchors the mood, the window lets it drift.
    QStringList rollingGenres() const;
    // Score-ordered eligible candidates for the current context, honoring the hard
    // constraints (excludePaths + the per-batch recent-artist list).
    void recordPick(const TrackScorer::Candidate &candidate, const TrackScorer::Scored &scored);

    QVector<TrackScorer::Candidate> m_pool;
    QHash<QString, TrackScorer::Affinity> m_affinities;
    QHash<QString, TrackScorer::Candidate> m_byPath; // pool + seed, for notePlayed lookups
    TrackScorer::Candidate m_seed;
    int m_exploration = 30;
    qint64 m_nowSecs = 0;
    QRandomGenerator *m_rng = nullptr;

    QStringList m_recentArtists;              // last 3 played/picked (folded, consecutive-deduped)
    QList<QStringList> m_playedGenres;        // last 3 played tracks' folded genres
    QHash<QString, int> m_albumCounts;        // albumKey -> tracks committed this session
    QSet<QString> m_usedPaths;                // never pick/repeat a path twice
    QHash<QString, QList<TrackScorer::Component>> m_pickReasons;
};
