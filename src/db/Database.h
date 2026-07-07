#pragma once

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QVector>

#include <QList>

#include <tuple>

#include <QHash>
#include <QPair>
#include <QSet>
#include <QStringList>

#include <memory>

#include "core/Album.h"
#include "core/Artist.h"
#include "core/MetadataBlob.h"
#include "core/ScanRoot.h"
#include "core/Track.h"
#include "core/TrackFingerprint.h"
#include "fs/LinkRoot.h"
#include "mpd/MpdTrack.h"
#include "search/SearchRecord.h"

class Database;

// One library track offered to the radio recommender (Stage 1). Deliberately
// lighter than Track: only the fields TrackScorer needs, plus the raw genre and
// rating pieces the caller folds/resolves. `year` is parsed from the date in
// C++; `effectiveRating0To100` mirrors the pending-write rating overlay used by
// the full track loaders; `genresFolded` is every folded genre of the track.
struct RadioCandidateRow {
    QString path;
    QString artistName;
    QString title;
    QString albumArtistName;
    QString albumTitle;
    QString mbRecordingId;
    QString releaseGroupId;
    QStringList genresFolded;
    int year = 0;                   // 0 = unknown
    int effectiveRating0To100 = -1; // -1 = unrated
    bool hasUserRating = false;
};

// Streaming cursor over the search row set: local tracks first, then MPD
// tracks, folded into their norm fields as they are pulled. Lets the search
// worker build the in-memory index incrementally (fzf-from-a-pipe style)
// instead of blocking on a full read of the library before the first query can
// run. Obtain one from Database::beginTrackSearchStream() and pump nextBatch()
// until it returns false.
class TrackSearchCursor final {
public:
    TrackSearchCursor(const TrackSearchCursor &) = delete;
    TrackSearchCursor &operator=(const TrackSearchCursor &) = delete;

    // Pull up to `maxRows` more folded records into `out` (cleared first).
    // Returns false once the stream is fully drained (with `out` then empty).
    bool nextBatch(int maxRows, QVector<Search::SearchRecord> &out);

private:
    friend class Database;
    TrackSearchCursor(const QSqlDatabase &db, QString localSql, QString mpdSql);
    void advancePhase();

    enum class Phase { Local, Mpd, Done };
    QSqlQuery m_query;
    QString   m_localSql;
    QString   m_mpdSql;
    Phase     m_phase = Phase::Local;
    bool      m_execed = false;
    // Intern pool for the high-repetition fields, shared across both phases so
    // dedup spans the whole library.
    QHash<QString, QString> m_pool;
};

class Database final {
public:
    enum class TrackFlag {
        NeverRadio,
        NoLearn,
    };

    struct TrackRatingSnapshot {
        bool found = false;
        bool hasUserRating = false;
        int userRating0To100 = -1;
        int effectiveRating0To100 = -1;
        QString mbRecordingId;
    };

    struct RadioWeightProfile {
        QString name;
        QString weightsJson;
        QString updatedAt;
    };

    explicit Database(QString connectionName);
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool open(const QString &path);
    bool migrate();
    QString lastError() const;
    void releaseCacheMemory();
    void restoreCacheMemory();

    bool beginTransaction();
    bool commitTransaction();
    // Brackets a scan/ingest run so upsertTrack can memoize artist/album ids
    // (append-only during a scan) instead of re-querying them per track. The
    // caches are cleared on begin and end; outside a session no caching happens.
    void beginScanSession();
    void endScanSession();
    bool upsertTrack(const Track &track);
    // Fast first pass: insert path-derived placeholder rows (metadata_scanned=0)
    // for newly-enumerated files, without touching artists/albums. ON CONFLICT DO
    // NOTHING so it never clobbers an already-scanned row or resets its flag.
    bool insertEnumeratedPlaceholders(const QVector<Track> &tracks);
    // path -> {file_mtime, file_size, metadata_scanned} for tracks under
    // rootPrefix, for the incremental-rescan diff. Empty rootPrefix returns all
    // tracks. metadata_scanned lets the diff re-queue enumerated-only placeholders.
    QHash<QString, TrackFingerprint> trackFingerprints(const QString &rootPrefix = {}) const;
    // Paths of enumerated-only placeholder rows (metadata_scanned=0) awaiting a tag
    // read, optionally scoped to one directory and/or capped, for the lazy fill.
    QStringList enumeratedOnlyPaths(const QString &parentDir = {}, int limit = 0) const;
    // Count of the pending metadata-fill backlog (metadata_scanned=0), one indexed
    // COUNT — for progress display and "queued for fill" messages.
    int enumeratedOnlyCount() const;
    // When true, the artist/album/track browse also includes path-guessed
    // placeholders (metadata_scanned=0 with a guessed album_artist_name); off keeps
    // the browse real-data only. Drives the "show guessed metadata" setting.
    void setGuessedPlaceholdersVisible(bool visible) { m_showGuessedPlaceholders = visible; }
    // Flags the given track paths as missing (file no longer present).
    int markTracksMissing(const QStringList &paths);
    QStringList missingTrackPaths() const;
    int removeMissingTracks();
    int missingTrackCount() const;
    MetadataBlob::FullMetadata fullMetadata(const QString &path) const;
    TrackRatingSnapshot trackRatingSnapshot(const QString &trackPath) const;
    bool setUserTrackRating(const QString &trackPath, int rating0To100);
    bool clearUserTrackRating(const QString &trackPath);
    bool setTrackFlag(const QString &trackPath, TrackFlag flag, bool on);
    bool trackFlag(const QString &trackPath, TrackFlag flag) const;
    QSet<QString> flaggedPaths(TrackFlag flag) const;
    bool setPendingTrackRatingWrite(const QString &trackPath, int rating0To100, const QString &status, const QString &lastError = {});
    bool clearPendingTrackRatingWrite(const QString &trackPath);
    QVector<Track> tracksWithUserRatings() const;
    QVector<Track> tracksWithPendingRatingWrites() const;
    bool updateScannedTrackRating(const QString &trackPath, int rating0To100, Rating::Source source, qint64 fileSize, qint64 fileMtime);
    bool setUserAlbumRating(const QString &albumArtistName, const QString &albumTitle, int rating0To100);
    bool clearUserAlbumRating(const QString &albumArtistName, const QString &albumTitle);
    // Display-form genres for a library track (see track_genres, Schema v10),
    // ordered by genre_folded; empty when the track is unknown or has no
    // genre tag.
    QStringList genresForTrack(const QString &path) const;
    // Folded-genre -> distinct-track count across the whole library (the
    // radio engine's IDF denominator per genre). The vocabulary is small
    // (~400 entries) so this is cheap to call once and cache per session.
    // `taggedTrackTotal`, if non-null, receives the count of distinct tracks
    // carrying at least one genre (the IDF numerator base).
    QHash<QString, int> genreTrackCounts(int *taggedTrackTotal = nullptr) const;
    // Folded-genre -> distinct-track count for one album artist. Ordered
    // deterministically by descending count, then folded genre, for artist
    // radio seed aggregation.
    QVector<QPair<QString, int>> genreCountsForArtist(const QString &albumArtist) const;
    // Deterministic display samples for human genre curation reports.
    QStringList sampleArtistsForGenre(const QString &folded, int limit = 3) const;
    QHash<QString, QString> genreAliases() const;
    bool setGenreAlias(const QString &alias, const QString &canonical);
    bool removeGenreAlias(const QString &alias);
    QSet<QString> ignoredRadioGenres() const;
    bool setRadioGenreIgnored(const QString &genreFolded, bool ignored);
    QVector<RadioWeightProfile> radioWeightProfiles() const;
    QString radioWeightProfile(const QString &name) const;
    bool saveRadioWeightProfile(const QString &name, const QString &weightsJson);
    bool removeRadioWeightProfile(const QString &name);
    QString contentGroupPin(qint64 groupId) const;
    QHash<qint64, QString> contentGroupPins() const;
    bool setContentGroupPin(qint64 groupId, const QString &path);
    bool removeContentGroupPin(qint64 groupId);
    QString setting(const QString &key, const QString &fallback = {}) const;
    bool setSetting(const QString &key, const QString &value);
    bool removeSetting(const QString &key);
    QVector<LinkRoot> linkRoots() const;
    bool saveLinkRoot(const LinkRoot &linkRoot);
    bool removeLinkRoot(int id);
    QVector<ScanRoot> scanRoots() const;
    QVector<ScanRoot> enabledScanRoots() const;
    QVector<ScanRoot> enabledLibraryRoots() const;
    bool saveScanRoot(const ScanRoot &root);
    bool removeScanRoot(int id);
    bool setScanRootLastScanned(int id, const QString &lastError = {});
    Track trackForPath(const QString &path) const;
    // Lightweight (path, artist_name, title, musicbrainz_recording_id) rows for
    // every non-missing library track, for building the scrobbler-backfill match
    // index without loading full Track objects.
    QList<std::tuple<QString, QString, QString, QString>> trackMatchRows() const;
    // Fill the rich scanned columns (audio props, totals, sort names, rating
    // source, track-level MusicBrainz ids) that the lighter loaders omit.
    void enrichTrackForStatus(Track &track) const;
    // Case-insensitive substring search over title/artist/album/filename for
    // external consumers (IPC); the in-app Search view uses the fzf engine.
    QVector<Track> searchTracksLike(const QString &text, int limit = 50) const;
    // Up to `count` random non-missing library tracks for library-wide shuffle,
    // skipping any path in `excludePaths` (typically the current queue).
    QVector<Track> randomTracks(int count, const QSet<QString> &excludePaths = {}) const;
    // Radio candidate generation (Stage 1). Non-missing, metadata-scanned local
    // tracks that share ANY of `foldedGenres` (track_genres join). Each row
    // carries the track's full folded genre set, not just the matched ones.
    QVector<RadioCandidateRow> radioCandidates(const QStringList &foldedGenres, int limit = 2000) const;
    // Random-sample fallback for seeds with no genres to match on (same ORDER BY
    // RANDOM() idiom as randomTracks). Genres are still returned when present.
    QVector<RadioCandidateRow> radioFallbackCandidates(int limit = 2000) const;
    // Targeted radio candidate rows for specific local library paths. Used by
    // semantic neighbor augmentation after the feature store has picked group
    // neighbors and the app has resolved each group to a preferred copy path.
    QVector<RadioCandidateRow> radioCandidatesForPaths(const QStringList &paths) const;
    QVector<Track> tracksForDirectory(const QString &directory) const;
    QStringList localLibraryDirectories(const QString &parentDirectory = {}) const;
    qint64 upsertMediaSource(const QString &kind, const QString &name, const QString &rootHint, const QString &configPath);
    bool clearMpdTracksForSource(qint64 sourceId);
    bool upsertMpdTrack(qint64 sourceId, const MpdTrack &track);
    int mpdTrackCount(qint64 sourceId) const;
    qint64 mpdSourceId() const;
    QVector<Artist> mpdAlbumArtists() const;
    QVector<Album> mpdAlbumsForArtist(const QString &albumArtist, const QString &musicDirectory) const;
    QVector<Track> mpdTracksForArtist(const QString &albumArtist, const QString &musicDirectory, const QString &albumTitleFilter = {}) const;
    // Narrow to several albums in a single query (album-sort order). An empty
    // list returns the whole artist; empty entries are ignored.
    QVector<Track> mpdTracksForArtist(const QString &albumArtist, const QString &musicDirectory, const QStringList &albumTitleFilters) const;
    QVector<Artist> albumArtists() const;
    QVector<Album> albumsForArtist(const QString &albumArtist) const;
    QVector<Track> tracksForArtist(const QString &albumArtist, const QString &albumTitleFilter = {}) const;
    QVector<Track> tracksForArtist(const QString &albumArtist, const QStringList &albumTitleFilters) const;
    // Lightweight row set for building the search index — all non-missing local
    // tracks, folded (CJK romaji + sort-reading enrichment) for matching.
    QVector<Search::SearchRecord> allTracksForSearch() const;
    // Lightweight row set for MPD-imported tracks.
    QVector<Search::SearchRecord> allMpdTracksForSearch() const;
    // Streaming alternative to the two functions above: returns a cursor that
    // yields the same records (local then MPD) in batches, so the search index
    // can be filled incrementally instead of in one blocking read.
    std::unique_ptr<TrackSearchCursor> beginTrackSearchStream() const;

    // Cheap aggregates that summarize the search row set, for the on-disk search
    // cache's staleness check (recomputed on each load and compared to what the
    // cache stored). Coarse by design: catches added/removed/retagged tracks and
    // root changes, not rating-only edits (the GUI's background refresh covers
    // those). One COUNT/MAX per source plus the enabled-roots hash.
    struct SearchRowSummary {
        qint64  localCount = 0;
        qint64  localMaxMtime = 0;
        qint64  mpdCount = 0;
        quint64 rootsHash = 0;
    };
    SearchRowSummary searchRowSummary() const;

private:
    qint64 upsertArtist(const QString &name, const QString &sortName = {});
    qint64 upsertAlbum(const Track &track, qint64 albumArtistId);

    QString m_connectionName;
    QSqlDatabase m_db;
    QString m_lastError;

    // Per-scan-session id memoization (see beginScanSession). Only consulted
    // while m_scanSession is true; artists/albums are append-only during a scan
    // so a resolved id stays valid until the session ends.
    bool m_scanSession = false;
    QHash<QString, qint64> m_artistIdCache;
    QHash<QString, qint64> m_albumIdCache;
    bool m_showGuessedPlaceholders = false;
};
