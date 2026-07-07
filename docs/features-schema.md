# features.sqlite Schema

`features.sqlite` is written by the `muzaiten-index` C++ binary built with the
Qt application. The app and `muzaitenctl` treat this database as read-only.

Schema version: `3`

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

embeddings(
    content_group_id INTEGER PRIMARY KEY,
    model TEXT NOT NULL,
    version TEXT NOT NULL,
    dim INTEGER NOT NULL,
    vector BLOB NOT NULL
);

track_neighbors(
    content_group_id INTEGER NOT NULL,
    neighbor_group_id INTEGER NOT NULL,
    rank INTEGER NOT NULL,
    cosine REAL NOT NULL,
    PRIMARY KEY(content_group_id, rank)
);
```

The embedding tables are created by `tools/embedder`; fresh `muzaiten-index`
databases may not contain them until the embedder runs.

## Meta Keys

- `schema_version`: `3`.
- `indexer_version`: `cpp`.
- `dsp_version`: the value of `Dsp::kDspVersion`, currently
  `muzaiten-dsp-v1`.
- `created_at`: Unix epoch seconds when the database was first initialized.

Schema v1/v2 files are upgraded in place by `muzaiten-index scan`; the old
`features` table is dropped and recreated because no shipped database contains
authoritative scalar rows from the blocked bliss plan.

## Identity Columns

- `decode_hash`: SHA-256 over the canonical `ffmpeg` decode:
  `f32le`, mono, 22050 Hz. This is exact decoded-audio identity for this
  canonical path.
- `chromaprint_fp`: raw `fpcalc -raw -json -length 120` fingerprint integers,
  stored as little-endian signed 32-bit values.
- `content_group_id`: assigned by the indexer after grouping.
- `status`: `ok`, `decode_failed`, or `fp_failed`.

## Grouping

Grouping is recomputed after changed files are analyzed:

1. Files with identical `decode_hash` are unioned into the same group.
2. Files whose durations differ by at most 2000 ms are compared by Chromaprint
   bit-error rate over offsets `-3..=3` frames. Pairs with minimum BER `< 0.15`
   are unioned.

AcoustID lookup is intentionally out of scope.

## Feature Rows

`features` contains one row per content group. The representative is the
lexicographically first successfully analyzed path in the group, matching the
embedder representative policy. Each scan buffers the canonical PCM once for a
changed file, feeds it to both the SHA-256 identity hash and `Dsp::analyze`, and
writes scalar rows after grouping.

NULL means the extractor could not report that value for the representative.
For example, near-silence has NULL loudness and energy; non-rhythmic material
usually has NULL tempo.

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

- Tempo may fold to a half/double octave at extreme BPM.
- Tempo on non-rhythmic material is unreliable pending a salience gate backed by
  real-corpus evidence.

## Embedding Rows

`tools/embedder` adds CLAP embeddings and cosine neighbors. It upgrades
schema-1 databases to schema 2, accepts schema 3 without downgrading it, and is
not linked into the Qt application.

`embeddings` contains one row per content group for the active model:

- `content_group_id`: the content group being represented.
- `model`: model family identifier, currently `laion-clap-music-audioset`.
- `version`: checkpoint filename, currently
  `music_audioset_epoch_15_esc_90.14.pt`.
- `dim`: number of `float32` values in `vector`.
- `vector`: little-endian `float32` values, L2-normalized before storage.

The embedder uses the lexicographically first successfully analyzed path in a
content group as the representative audio file. Model weights are downloaded
outside the repository into the user cache and verified by SHA-256 before use.

## Neighbor Rows

`track_neighbors` contains precomputed cosine nearest neighbors over normalized
embedding vectors. Rows are regenerated as a full table:

- `content_group_id`: source group.
- `neighbor_group_id`: neighboring group.
- `rank`: one-based rank within the source group's neighbors.
- `cosine`: cosine similarity from the vector dot product.

The embedder stores the top 100 neighbors per source group by default and breaks
equal-score ties by `neighbor_group_id` for deterministic output.
