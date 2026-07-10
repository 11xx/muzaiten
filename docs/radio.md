# Radio — the offline recommendation engine

Everything here runs locally against your own library and listening
history. There is no cloud, no account, and nothing leaves your machine.

## Starting a session

- **Start Radio** from any track's context menu (library, queue, playlists,
  explorers, search results), from `Radio > Start radio from current track`,
  or `muzaitenctl start-radio <path>`. The seed's genres, era, and sound set
  the session's initial direction.
- **Artist radio** from the artist sidebar's context menu or
  `Radio > Start artist radio`.
- **Radio shuffle** is the ambient variant: the shuffle button cycles to a
  taste-aware mode that uses the radio engine for library pulls without a
  visible session. Its pull chance is configurable at
  `Radio > Radio shuffle percent…` (default 80%).
- **Mixes**: `Radio > Play Rediscovery mix` (loved-but-forgotten) and
  `Play Deep cuts mix` (rarely surfaced album tracks).

Starting radio snapshots the current queue first, so "Restore saved queue…"
can undo it. While a session runs, a radio indicator appears in the player
bar — click to stop (the queue is kept), right-click for session options.

## How picks are scored

Every pick blends: shared **genres** (alias-canonicalized, weighted by
rarity so `shoegaze` counts more than `rock`), **era** proximity,
**ratings**, **listening history** (local plays + imported scrobbles,
pooled across duplicate copies), a **novelty** bonus for the unheard, and
penalties for recent plays, high skip rates, and repeating artists. With
audio analysis built (below), three content-aware components join in:
**tempo** and **energy** proximity to the session's rolling sonic context,
and **audio** similarity by CLAP embedding against the last few played
tracks.

Hover a radio pick in the queue to see exactly why it was chosen — a
human-readable summary plus the numeric component breakdown.

Sessions throttle artists and albums, never repeat a song (MusicBrainz
recording identity, falling back to folded artist+title), and resolve
duplicate copies to the best available quality (see duplicate pinning
below).

## Steering a session

- **Exploration** (`Radio > Exploration…`, 0–100) controls how far picks
  may stray from the seed's mood; **Adventurous (this session)** is a
  one-session boost.
- **Never play on radio** and **Don't learn from this** are per-track
  taste flags on every track context menu. The first excludes a track from
  radio permanently; the second keeps a track's listening history out of
  the engine's taste signals (guilty-pleasure mode).
- **Batch size** (`Radio > Radio batch size…`) sets how many picks queue
  ahead at a time.

## Tuning the scoring

`Radio > Scoring weights…` edits every scorer weight with validated
ranges, manages named profiles, and can **suggest a profile learned from
your own listening**: after enough radio history (~200 picks with some
early skips), a small local model fits your skip behavior and proposes
per-component adjustments. Suggestions are never auto-applied — they save
as a `learned-YYYYMMDD` profile you can inspect and apply explicitly.

The same operations exist client-side:

```sh
muzaitenctl radio-weights get | set '<json>' | save <name> | apply <name> | list
muzaitenctl radio-learn --dry-run
```

Weight changes take effect on the next session.

## Genre curation

Scanned genre tags are matched through a folded, alias-canonicalized
vocabulary. `Radio > Genre curation…` edits aliases (`clássica` →
`classical`), radio-ignored genres (`soundtrack`), and shows the full
vocabulary with track counts for context. CLI equivalents: `genre-alias`,
`radio-genre`, `genre-report`.

## Audio analysis (content-aware tier)

`Library > Audio analysis > Analyze library audio` runs the bundled
`muzaiten-index` over your library with live progress: one canonical decode
per file computes exact audio identity, Chromaprint content groups
(duplicate detection across formats/codecs), and clean-room DSP scalars
(tempo, loudness, energy, brightness) into `features.sqlite`. Progress
moves through file analysis, grouping, then a **Writing features** phase
that refreshes only missing or stale group feature rows (with its own n/m,
rate, and ETA). Stopping mid-run keeps completed work; the next analysis
resumes remaining files and stale groups. Terminal equivalent:

```sh
muzaiten-index scan --library ~/.local/share/muzaiten/library.sqlite \
  --features ~/.local/share/muzaiten/features.sqlite --json --progress
```

`Library > Audio analysis > Analysis status…` (or
`muzaitenctl features-status`) reports coverage. `Duplicate copies…` (or
`duplicate-groups` / `pin-copy` / `unpin-copy`) inspects detected duplicate
groups and pins which copy radio should prefer.

The current `muzaiten-dsp-v2` analyzer makes full-track analysis substantially
cheaper with a first-party fixed-size real FFT. The v2 comparison kept the
tempo and energy values used by radio exactly stable across the synthetic
oracle and an isolated DSF/high-resolution corpus, including a deliberate
85/170 BPM near-octave scorer gate, so radio's tempo and energy falloff
constants are unchanged. Upgrading marks v1 scalar rows stale until the normal
resumable feature phase refreshes them; stale rows never enter scoring.

**CLAP embeddings** (audio similarity + free-text search) are computed by
the optional Python tool in `tools/embedder` — kept out of the app because
it downloads a ~2 GB model on first use:

```sh
cd tools/embedder
uv run muzaiten-embed scan --features ~/.local/share/muzaiten/features.sqlite
uv run muzaiten-embed neighbors --features ~/.local/share/muzaiten/features.sqlite
```

`scan` (and `query`) accept `--device auto|cuda|cpu` (default `auto`) and log
the chosen device at startup; an explicit `--device cuda` fails rather than
silently falling back to CPU. See `tools/embedder/README.md` for details.

With embeddings present, radio pools get augmented with sonic neighbors
(tag-poor tracks surface because they *sound* right) and free-text semantic
search works:

```sh
muzaitenctl semantic-search "melancholic shoegaze" --limit 10
```

The full `features.sqlite` layout — identity, scalar, and embedding tables,
with units and caveats — is documented in
[features-schema.md](features-schema.md).

## Telemetry and privacy

The engine records its own picks and your play/skip outcomes into the local
history database purely as training data for the weight suggestions above.
Live scoring never reads that telemetry; nothing is transmitted anywhere.
