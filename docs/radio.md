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
  visible session. It starts its rolling mood from the current library track,
  including the same genre, tempo, energy, sonic-neighbor, and CLAP audio
  context as Start Radio, then follows the rolling context as tracks play. Its
  pull chance is configurable at
  `Radio > Radio shuffle percent…` (default 80%).
- **Mixes**: `Radio > Play Rediscovery mix` (loved-but-forgotten) and
  `Play Deep cuts mix` (rarely surfaced album tracks).

Starting radio snapshots the current queue first, so "Restore saved queue…"
can undo it. While a session runs, a radio indicator appears in the player
bar — click to stop (the queue is kept), right-click for session options.
Starting from the track that is already playing keeps its playback position
and replaces only the queue around it. Starting from another track plays that
seed immediately. In both cases recommendations load in the background and an
indeterminate status-bar indicator remains visible until the new tail arrives.

## How picks are scored

Every pick blends **era** proximity, **ratings**, **listening history**
(local plays + imported scrobbles, pooled across duplicate copies), a
**novelty** bonus for the unheard, and penalties for recent plays, high skip
rates, and repeating artists. The first five radio picks use the full novelty
and rating weights; subsequent picks linearly taper those two signals by 10%
per pick to small floors, so the session naturally settles into its rolling
sound without losing discovery or taste history entirely. When the rolling context and a candidate share a
complete tempo-and-energy pair or matching CLAP embeddings, **tempo** and
**energy** proximity plus **audio** similarity by CLAP embedding are the
primary match signal. In that case genre tags are intentionally ignored. If
neither audio signal is available, radio falls back to shared **genres**
(alias-canonicalized and rarity-weighted, so `shoegaze` counts more than
`rock`) rather than combining incomplete audio with potentially incorrect
tags.

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
- **Refill padding** (`Radio > Radio refill padding…`) sets how many queued
  tracks may remain before the next batch starts loading. The default is 5;
  use 0 to wait until playback reaches the final queued track.
- **Refresh picks below** appears when you right-click a queue row during a
  radio session. It replaces only the unplayed radio picks below that row;
  earlier rows and manually queued tracks are kept.

## Tuning the scoring

`Radio > Customization…` edits every scorer weight and the session-decay
controls with validated ranges. Profiles are named and persisted in the
application configuration directory, with the active profile restored at
startup. New, Duplicate, Rename, Delete, and Reset to Default manage the
profile list; the Default profile cannot be deleted. Each profile retains its
own most recent 50 changes for Undo and Redo.

Edits preview immediately in a running radio or Radio Shuffle session: newly
generated picks use the changed settings, while tracks already queued keep
their existing order. A 500 ms pause saves one history step. Apply sets the
Revert baseline; Revert restores that baseline and applies it live.

The same operations exist client-side:

```sh
muzaitenctl radio-weights get | set '<json>' | save <name> | apply <name> | list
muzaitenctl radio-learn --dry-run
```

The existing command-line weight commands remain available for scripted
workflows. GUI profile changes take effect for the next generated pick.

## Genre curation

Scanned genre tags are matched through a folded, alias-canonicalized
vocabulary. `Radio > Genre curation…` edits aliases (`clássica` →
`classical`), radio-ignored genres (`soundtrack`), and shows the full
vocabulary with track counts for context. CLI equivalents: `genre-alias`,
`radio-genre`, `genre-report`.

## Audio analysis (content-aware tier)

`Library > Audio analysis > Analyze library audio` runs the bundled
`muzaiten-features` over your library with live progress: one canonical decode
per file computes exact audio identity, Chromaprint content groups
(duplicate detection across formats/codecs), and clean-room DSP scalars
(tempo, loudness, energy, brightness) into `features.sqlite`. Progress
moves through file analysis, grouping, then a **Writing features** phase
that refreshes only missing or stale group feature rows (with its own n/m,
rate, and ETA). Stopping mid-run keeps completed work; the next analysis
resumes remaining files and stale groups. Terminal equivalent:

```sh
muzaiten-features refresh --progress=jsonl
```

`Library > Audio analysis > Analysis status…` (or
`muzaitenctl features-status`) reports coverage. `Duplicate copies…` (or
`duplicate-groups` / `pin-copy` / `unpin-copy`) inspects detected duplicate
groups and pins which copy radio should prefer.

The current `muzaiten-dsp-v2` analyzer makes full-track analysis substantially
cheaper with a first-party fixed-size real FFT. The v2 comparison kept the
tempo and energy values used by radio exactly stable across the synthetic
oracle and an isolated DSF/high-resolution corpus. Radio treats half- and
double-tempo estimates as equivalent, so an 85 BPM context matches a 170 BPM
candidate. Upgrading marks v1 scalar rows stale until the normal resumable
feature phase refreshes them; stale rows never enter scoring.

**CLAP embeddings** (audio similarity + free-text search) are opt-in and
provided by the separately installed `muzaiten-features-clap` package. Native
analysis never needs Python. Install the provider yourself—the GUI never runs
uv—and explicitly consent to the model download:

```sh
uv tool install 'muzaiten-features-clap[model]'
muzaiten-features model download
muzaiten-features refresh --semantic
```

The saved policy is disabled by default. The orchestrator discovers and
handshakes the provider, holds the same feature-store lock across native and
semantic phases, uses durable inference batches, and rebuilds neighbors only
after embeddings change. Scans and queries never download the model implicitly.
See [semantic-analysis.md](semantic-analysis.md) for provider setup, device,
consent, progress, and provenance details.

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
Previous and direct queue-row selections are recorded as navigation, not as
rejections; Next remains a skip. Live scoring never reads that telemetry;
nothing is transmitted anywhere.
