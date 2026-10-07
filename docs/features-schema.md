# features.sqlite Schema

`features.sqlite` is written by the `muzaiten-features` C++ binary built with the
Qt application. The app and `muzaitenctl` treat this database as read-only.

Schema version: `5`

```sql
meta(key TEXT PRIMARY KEY, value TEXT);

files(
    path TEXT PRIMARY KEY,
    mtime INTEGER NOT NULL,
    size INTEGER NOT NULL,
    duration_ms INTEGER,
    decode_hash TEXT,
    chromaprint_fp BLOB,
    content_group_id INTEGER,
    analyzed_at INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'ok'
);

-- Group membership is read per radio pick and in bulk while grouping;
-- without this the lookup is a full scan of files.
CREATE INDEX idx_files_content_group ON files(content_group_id);

content_groups(id INTEGER PRIMARY KEY AUTOINCREMENT);

features(
    content_group_id INTEGER PRIMARY KEY,
    tempo_bpm REAL,
    loudness_lufs REAL,
    loudness_std_db REAL,
    spectral_centroid_mean_hz REAL,
    spectral_centroid_std_hz REAL,
    spectral_flatness_mean REAL,
    zero_crossing_rate REAL,
    onset_rate_hz REAL,
    energy REAL,
    extractor TEXT NOT NULL,
    version TEXT NOT NULL
);

file_features(
    path TEXT PRIMARY KEY,
    tempo_bpm REAL,
    loudness_lufs REAL,
    loudness_std_db REAL,
    spectral_centroid_mean_hz REAL,
    spectral_centroid_std_hz REAL,
    spectral_flatness_mean REAL,
    zero_crossing_rate REAL,
    onset_rate_hz REAL,
    energy REAL,
    extractor TEXT NOT NULL,
    version TEXT NOT NULL
);

semantic_generations(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    capability TEXT NOT NULL,
    model TEXT NOT NULL,
    checkpoint_sha256 TEXT NOT NULL,
    feature_revision TEXT NOT NULL,
    vector_dim INTEGER NOT NULL,
    provider_path TEXT,
    provider_version TEXT,
    created_at INTEGER NOT NULL,
    completed_at INTEGER,
    active INTEGER NOT NULL DEFAULT 0
);

embeddings(
    content_group_id INTEGER NOT NULL,
    generation_id INTEGER NOT NULL,
    dim INTEGER NOT NULL,
    vector BLOB NOT NULL,
    PRIMARY KEY(content_group_id, generation_id)
);

track_neighbors(
    content_group_id INTEGER NOT NULL,
    neighbor_group_id INTEGER NOT NULL,
    rank INTEGER NOT NULL,
    cosine REAL NOT NULL,
    generation_id INTEGER NOT NULL,
    algorithm_revision TEXT NOT NULL,
    top_k INTEGER NOT NULL,
    PRIMARY KEY(content_group_id, generation_id, rank)
);
```

The native orchestrator creates the complete schema. The optional provider
writes only generation-scoped embeddings and neighbors while the native parent
holds the feature-store lock.

## Meta Keys

- `schema_version`: `5`.
- `indexer_version`: `cpp`.
- `dsp_version`: the analyzer version recorded when the indexer last opened the
  store. It is diagnostic; app-side freshness is decided from each feature
  row's `version` against the version expected by the running build.
- `created_at`: Unix epoch seconds when the database was first initialized.
- `last_scan_finished_at`: Unix epoch seconds for the last successful full
  audio analysis scan.
- `last_scan_elapsed_secs`: wall time for that scan.
- `last_scan_scanned`, `last_scan_skipped`, `last_scan_failed`: file counts
  from the last successful full scan.
- `last_scan_mean_ms_per_track`: elapsed milliseconds per scanned track.
- `last_scan_power`: `background`, `balanced`, or `turbo`.

Schema v1/v2 files are upgraded in place by `muzaiten-features refresh`; their old
bliss-era `features` shape is dropped and recreated because no shipped database
contains authoritative rows in that shape. Schema v3/v4 scalar rows are
preserved. The v5 migration assigns a uniform known CLAP corpus to its current
generation; mixed or unknown semantic rows become inactive and require refresh.

## Identity Columns

- `decode_hash`: SHA-256 over the canonical `ffmpeg` decode:
  `f32le`, mono, 22050 Hz. This is exact decoded-audio identity for this
  canonical path.
- `chromaprint_fp`: raw Chromaprint fingerprint integers for the first 120 s,
  stored as little-endian signed 32-bit values and kept compatible with
  `fpcalc -raw -json -length 120` output.
- `content_group_id`: assigned by the indexer after grouping.
- `status`: `ok`, `decode_failed`, or `fp_failed`.

## Grouping

Grouping uses these two identity relations after changed files are analyzed:

1. Files with identical `decode_hash` are unioned into the same group.
2. Files whose durations differ by at most 2000 ms are compared by Chromaprint
   bit-error rate over offsets `-3..=3` frames. Pairs with minimum BER `< 0.15`
   are unioned.

AcoustID lookup is intentionally out of scope.

The first scan computes those relations across every successful file. Normal
incremental scans instead treat each unaffected existing group as an already
known connected component and compare only new or changed files plus all
members of a changed file's previous group. This preserves exact split, merge,
and stable-group-id behavior without repeating Chromaprint comparisons between
unchanged groups. A full regroup remains the recovery path when successful
ungrouped rows predate the current process, such as after cancellation between
file analysis and grouping. The incremental path keeps lightweight metadata and
exact hashes globally visible but hydrates Chromaprint blobs only for affected
tracks and candidates within their duration windows; a large affected set
automatically switches back to one ordered blob scan.

## Per-file Feature Rows

`file_features` is the durable scalar result for each successfully analyzed
path. The file phase already decodes a changed file once for its SHA-256
identity, Chromaprint, and `Dsp::analyze`; schema v4 persists that analysis at
the same time instead of keeping it only in a process-local cache. Rows use the
same scalar columns, extractor, DSP version, and NULL semantics as group rows.
A row with NULL optional scalars is therefore a known result, not a request to
decode the file again.

The cache is strictly versioned. A row is copyable only when
`file_features.version` matches the DSP version expected by the running
indexer. Changed paths overwrite their row after successful analysis; failed
files are never representatives, and orphan rows are swept when their `files`
row is removed. The path key intentionally matches the lexicographic
representative policy and the indexer's incremental path/mtime/size identity.
A rename is analyzed once under its new path.

## Group Feature Rows

`features` contains one row per content group. The representative is the
lexicographically first successfully analyzed path in the group, matching the
embedder representative policy. After grouping, the feature phase copies a
fresh representative `file_features` row into `features` without touching the
audio. If that per-file row is missing or stale, the existing parallel fallback
decodes and analyzes the representative once, writes both rows, and thereby
backfills the copy path for a retry or later refresh. Files analyzed in the
same scan already have their per-file row, so they are never decoded a second
time merely to create the group row.

Group rows persist across rescans: a group that keeps its id keeps its feature
row as long as the row's `version` matches the current `Dsp::kDspVersion`.
Feature-only refreshes also skip Chromaprint regrouping when no successful file
is awaiting a group id. The steady-state phase is therefore O(stale groups)
SQLite copying; decode is reserved for genuinely missing or invalidated
per-file results.

The app and radio serve scalar rows only when `features.version` matches the
DSP version expected by the running build. This strict row-level check also
covers an untouched old store after an application upgrade and mixed-version
stores produced by canceling and resuming a refresh. A missing or empty
`meta.dsp_version` is shown as unknown but does not override per-row freshness:
matching rows remain readable and non-matching rows remain stale.

The active scalar version is `muzaiten-dsp-v2`. It replaces the 2048-point
complex-double STFT with Muzaiten's allocation-free fixed-size real-float FFT,
while retaining double-precision power values and downstream reductions.
Measured v1-to-v2 comparisons kept tempo, loudness, loudness spread, onset
rate, zero-crossing rate, and energy exact on the deterministic oracle and an
isolated 6.3-hour corpus containing DSD64/128/256 and 96/192 kHz 24-bit FLAC.
Only spectral fields moved: at most `0.0001 Hz` under the committed centroid
gate and `1e-9` under the flatness gate. Existing v1 rows are intentionally
stale after upgrading and are refreshed through the normal resumable
**Writing features** phase; identity hashes, Chromaprint, and content groups
do not change.

## Scan JSON

`muzaiten-features refresh --json` includes the stable count fields
`scanned`, `skipped`, `failed`, `groups`, and `featured_groups`, plus:

- `featured_fresh`: existing feature rows whose `version` matches this build.
- `featured_stale`: existing feature rows awaiting re-analysis because their
  `version` does not match this build. Missing group rows are pending work but
  are not included in either existing-row count.
- `elapsed_secs`: scan wall time.
- `power`: effective analysis power (`background`, `balanced`, or `turbo`).
- `jobs`: effective worker count after `--power` and `--jobs` resolution.
- `canceled`: true when SIGTERM/SIGINT requested a cooperative stop.
- `timings`: per-stage `stage_wait`, `decode`, `hash`, `dsp`, and `fp`
  aggregates with `total_ms`, `mean_ms`, `p50_ms`, and `p95_ms`. `stage_wait`
  is the time a decode slot spent waiting for its file's staged bytes; it is
  zero throughout when `--staging-bytes` is off.
- `staging`: `budget_bytes` (the effective `--staging-bytes` value, 0 when
  off) and `fallback_extensions`, the lowercase extensions whose staged decode
  failed and were read from their path for the rest of the run.

NULL means the extractor could not report that value for the representative.
Near-silence has NULL loudness and energy; a flat or empty onset envelope
(true silence) has NULL tempo.

Known tempo caveats:

- Tempo is quantized. `estimateBpm` picks an integer autocorrelation lag over
  an onset envelope sampled at `kSampleRateHz / kHop`, so every value is
  `2583.98 / lag` for some integer lag, and only a few dozen values are
  achievable at all. Near 120 BPM consecutive lags are ~5.7 BPM apart, a ±2.8
  BPM quantization floor; near 172 BPM the step is ~11 BPM. Never treat a small
  BPM difference as meaningful, and never compare tempo for exact equality.
- The global estimate may fold to the half or double octave toward the edges
  of the common range (a 180 BPM track can report 90) — the standard
  limitation of autocorrelation tempo estimation under a 120 BPM-centered
  prior; octave-equivalent values should be treated as matching.
- Non-rhythmic but non-silent material (drones, noise, field recordings)
  does NOT get NULL: the prior pulls the estimate toward ~120 BPM, so its
  `tempo_bpm` is unreliable rather than absent.

A salience gate forcing that last class to NULL is deliberately not
implemented. Corpus measurement does not support it: the cohort sitting on the
prior has a *higher* onset rate and *lower* spectral flatness than the rest of
the library, so it is more rhythmic rather than less, and the population such a
gate would target is a fraction of a percent of a library, about half of which
does not sit on the prior at all. A gate tuned to fire there would mislabel
roughly as much material as it corrected.

- `tempo_bpm`: global tempo estimate in beats per minute.
- `loudness_lufs`: BS.1770-style integrated loudness in LUFS.
- `loudness_std_db`: standard deviation of gated 400 ms block loudness, in dB;
  this is a simple dynamics proxy, not EBU Tech 3342 LRA.
- `spectral_centroid_mean_hz`: mean spectral centroid in Hz.
- `spectral_centroid_std_hz`: standard deviation of spectral centroid in Hz.
- `spectral_flatness_mean`: mean spectral flatness over non-silent frames,
  unitless `0..1`.
- `zero_crossing_rate`: fraction of adjacent canonical samples that cross zero,
  unitless `0..1`.
- `onset_rate_hz`: detected onsets per second.
- `energy`: unit-scale perceived-intensity blend versioned by `dsp_version`.
- `extractor`: `muzaiten-dsp`.
- `version`: the `dsp_version` used for this row.

Caveats:

- Tempo is quantized to the integer-lag grid; small BPM differences are not
  meaningful.
- Tempo may fold to a half/double octave at extreme BPM.
- Tempo on non-rhythmic material is unreliable rather than NULL.

## Local feature-pack format

`muzaiten-features pack export|import FILE` exchanges existing scalar
analysis between feature stores without copying a database. Version 1 is UTF-8 NDJSON: one header object
on the first line, followed by exactly the declared number of record objects.
Every line, including the last, ends in a newline. The header contains exactly:

```json
{"format":"muzaiten-features-pack","version":1,"records":1}
```

Each record contains exactly `decode_hash`, `extractor`, `version` and
`scalars`. `decode_hash` is 64 lowercase SHA-256 hex digits for the source
group's representative, its lexicographically first successful path. The
other three fields come from that file's own `file_features` row, so a record
always describes the audio its hash names. `extractor` and `version`
are nonempty ASCII tokens of at most 128 characters using letters, digits,
hyphens, underscores and periods. The `scalars` object requires all nine
columns: `tempo_bpm`, `loudness_lufs`, `loudness_std_db`,
`spectral_centroid_mean_hz`, `spectral_centroid_std_hz`,
`spectral_flatness_mean`, `zero_crossing_rate`, `onset_rate_hz` and `energy`.
Each value is a finite JSON number or `null`, preserving SQL NULL semantics.
Numbers except loudness are nonnegative; flatness, zero-crossing rate and
energy are at most 1. Units and interpretation match the group rows above.

Files are limited to 64 MiB, 100,000 records and 16 KiB per line excluding
the newline. Unknown or missing keys at every level, unknown format versions,
duplicate keys (including escaped spellings), duplicate hashes, invalid
UTF-8, malformed or truncated lines, invalid values and record-count
disagreements are rejected before opening the store. A header count detects
truncation at a complete record boundary.

Export emits one record per content group whose representative has a current
`muzaiten-dsp` row in `file_features`. It never exports the group row: a group
keeps its id and row while its membership changes, so that row can hold an
earlier representative's analysis. Groups whose representative row is missing
or stale are counted as skipped, as are rows whose hash or values break the
record rules above; those are also counted as `invalid`. Export never invokes
extraction. Ambiguous representative
hashes require repairing the identity store with refresh before export.
Import requires an existing schema-5 store and performs no schema migration.
Refresh identity first so the recipient's hashes describe its current audio.
Only exact representative hashes with extractor `muzaiten-dsp` and the
running binary's `Dsp::kDspVersion` are compatible. A nonrepresentative
member's matching hash, similar fingerprint or matching MusicBrainz ID
cannot authorize adoption. Ambiguous destination hashes are mismatched.

The native writer holds `features.sqlite.lock` for preview, export and
import. Valid imports write group rows in one transaction. Existing rows,
including stale rows, require `--replace`. Import does not modify identity,
per-file caches, semantic generations, neighbors or scan metadata. Readers
and refresh judge imported versions by their ordinary freshness rules.

The CLI report includes `format`, `version`, `operation`, `dry_run`,
`replace`, `contents`, `records`, `adopted`, `skipped`, `mismatched`,
`incompatible` and `existing`. For import, every incoming record is adopted,
skipped because a local row already exists, or mismatched because its
extractor/version or representative hash does not qualify. `incompatible`
is the extractor/version subset of `mismatched`; `existing` is the
existing-row subset of `skipped`. `adopted` means would adopt during dry-run.
Import also reports `destination_groups`, `destination_unmatched_groups`,
`destination_files` and `destination_unmatched_files`. Unmatched groups have
no compatible incoming representative record; unmatched files have no exact
hash in a compatible incoming record, regardless of group membership.
Export adds `exported` (would export during dry-run) and `invalid`, and uses
`skipped` for groups without an exportable representative row; `invalid` is
the subset of `skipped` whose local hash or values break the record rules.

Version 1 has no optional identifier or embedding section. It excludes paths,
filenames, file sizes, mtimes, analysis times, internal content-group IDs,
history, application state, ratings and timestamps. Taste/history is absent,
but hashes identify recordings; the pack is not anonymous library data.
Embedding exchange requires an enforceable exact model, checkpoint SHA-256,
feature revision and dimension match, together with verified source terms.

## Embedding Rows

`muzaiten-features-clap` supplies CLAP embeddings and cosine neighbors through
the native orchestrator. Only one semantic capability generation is active in
v1. Activating a different checkpoint/revision/dimension hides old rows
immediately; partial new coverage is therefore truthful and resumable.

`embeddings` contains one row per content group and generation:

- `content_group_id`: the content group being represented.
- `generation_id`: provenance row containing capability, model, checkpoint
  SHA-256, stable feature revision, vector dimension, provider diagnostics,
  timestamps, and active state.
- `dim`: number of `float32` values in `vector`.
- `vector`: little-endian `float32` values, L2-normalized before storage.

The provider uses the lexicographically first successfully analyzed path in a
content group as representative audio. Model download is explicit, cached
outside the repository, checksum-verified, and atomic.

## Neighbor Rows

`track_neighbors` contains precomputed cosine nearest neighbors over normalized
embedding vectors. Rows are regenerated as a full table:

- `content_group_id`: source group.
- `neighbor_group_id`: neighboring group.
- `rank`: one-based rank within the source group's neighbors.
- `cosine`: cosine similarity from the vector dot product.
- `generation_id`: embedding generation used for both endpoints.
- `algorithm_revision` and `top_k`: provenance required by readers; stale or
  insufficient neighbor sets are rejected.

The embedder stores the top 100 neighbors per source group by default and breaks
equal-score ties by `neighbor_group_id` for deterministic output.
