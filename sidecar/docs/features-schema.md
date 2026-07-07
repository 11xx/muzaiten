# features.sqlite Schema

`features.sqlite` is written by the standalone `sidecar/indexer` Rust binary and
read by later muzaiten application slices. The app must treat this database as
read-only.

Schema version: `1`

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

content_groups(id INTEGER PRIMARY KEY AUTOINCREMENT);

features(
    content_group_id INTEGER PRIMARY KEY,
    bliss_vector BLOB NOT NULL,
    tempo_bpm REAL,
    loudness REAL,
    energy REAL,
    brightness REAL,
    extractor TEXT NOT NULL,
    version TEXT NOT NULL
);
```

## Meta Keys

- `schema_version`: `1`.
- `indexer_version`: the `muzaiten-index` crate version.
- `bliss_version`: currently `blocked_license_gpl_3_only`.
- `created_at`: Unix epoch seconds when the database was first initialized.

## Identity Columns

- `decode_hash`: SHA-256 over `ffmpeg` decoded canonical PCM
  (`f32le`, mono, 22050 Hz). This is exact decoded-audio identity for this
  canonical decode path.
- `chromaprint_fp`: raw `fpcalc -raw -json -length 120` fingerprint integers,
  stored as little-endian signed 32-bit values.
- `content_group_id`: assigned by the indexer after grouping.

## Grouping

Grouping is recomputed after each scan:

1. Files with identical `decode_hash` are unioned into the same group.
2. Files whose durations differ by at most 2000 ms are compared by Chromaprint
   bit-error rate over the overlapping prefix. The indexer checks offsets
   `-3..=3` frames and uses the minimum BER. Pairs with BER `< 0.15` are unioned.

AcoustID lookup is intentionally out of scope.

## Feature Rows

The `features` table is intentionally present but empty in this slice. The
planned bliss extraction tier is blocked because the current published Rust bliss
crates are GPL-3.0-only, while the stage plan permits only a permissively
licensed bliss library. No GPL bliss dependency is linked or vendored.

When a compatible extractor is selected, feature rows should be one per content
group, using the first analyzed group member as the representative unless a later
plan supersedes that policy.

The intended scalar mapping is still reserved as:

- `tempo_bpm`: extractor tempo dimension.
- `loudness`: extractor loudness dimension.
- `energy`: extractor energy descriptor.
- `brightness`: extractor brightness descriptor.

Because bliss extraction is not active, there is no authoritative bliss
index-to-name mapping in this schema version.
