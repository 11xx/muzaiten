#pragma once

// On-disk cache of the folded search index.
//
// Building the in-memory index means reading every track from the library DB
// and folding its fields (CJK romanization, sort-reading enrichment) — the
// expensive part. This cache stores the already-folded SearchRecords so a warm
// start skips both the read and the fold. The streaming TrackSearchCursor is
// the builder (and the cold-start fallback); this is the fast path the GUI and
// muzaitenctl both load.
//
// Freshness uses a trigger-maintained source revision, database identity/path,
// and format/fold/schema versions. Signature reads do not scan library rows.
// Builders read the signature and records under one SQLite read snapshot.

#include "search/SearchRecord.h"

#include <QString>
#include <QVector>
#include <QtTypes>

#include <functional>

class Database;

namespace Search {

struct CacheSignature {
    quint32 formatVersion = 0;   // on-disk record layout (IndexCache::kFormatVersion)
    quint32 foldVersion = 0;     // fold pipeline/tables (Fold::kVersion)
    qint32  schemaVersion = 0;   // library DB schema (Schema::currentVersion)
    QString databaseId;
    QString databasePath;
    qint64 contentRevision = -1;

    bool valid() const { return !databaseId.isEmpty() && !databasePath.isEmpty() && contentRevision >= 0; }

    bool operator==(const CacheSignature &other) const = default;
};

namespace IndexCache {

// Layout version of the serialized record stream; bump on any field change.
inline constexpr quint32 kFormatVersion = 2;

// Default cache file under AppPaths::cacheDir().
QString defaultPath();

// The signature describing the DB's search row set as it stands now.
CacheSignature currentSignature(const Database &db);
QString mismatchReason(const CacheSignature &cached, const CacheSignature &current);

// Atomically write the folded records + signature, zstd-compressed. Returns
// false on any I/O or encoding error (caller falls back to a live build).
bool write(const QString &path, const CacheSignature &signature, const QVector<SearchRecord> &records);

struct Loaded {
    bool ok = false;            // false if missing / unreadable / corrupt
    CacheSignature signature;   // the signature the cache was written with
    QVector<SearchRecord> records;
};

// Read the cache. ok=false if the file is absent or fails integrity checks.
// Does NOT consult the database — the caller compares loaded.signature against
// currentSignature(db) to decide freshness.
Loaded read(const QString &path);

// Streaming read: invokes `sink` for each record as it is deserialized, so a
// consumer (e.g. the fzf picker) can start using rows immediately instead of
// waiting for the whole file. Returns false if the file is absent/corrupt
// (some records may already have been sunk on a mid-stream failure). When
// `outSignature` is non-null it receives the stored signature. `expected`, when
// provided, is checked on the same open file before any records are emitted.
bool forEachRecord(const QString &path, CacheSignature *outSignature,
                   const std::function<void(SearchRecord)> &sink,
                   const CacheSignature *expected = nullptr);

} // namespace IndexCache
} // namespace Search
