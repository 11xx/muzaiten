# Runtime state and paths

By default, muzaiten uses XDG paths:

- data: `$XDG_DATA_HOME/muzaiten` or `~/.local/share/muzaiten`
- state: `$XDG_STATE_HOME/muzaiten` or `~/.local/state/muzaiten`
- cache: `$XDG_CACHE_HOME/muzaiten` or `~/.cache/muzaiten`
- config: `$XDG_CONFIG_HOME/muzaiten` or `~/.config/muzaiten`

The data directory holds the library (`library.sqlite`), playlists
(`playlists.sqlite`), listening history (`history.sqlite`), and audio-analysis
results (`features.sqlite`). State holds UI and queue state (`state.sqlite`);
cache holds artwork and the search index. The restored free-roam
file-explorer directory must be a nonempty, existing, readable directory. If
that saved directory is no longer browsable, Muzaiten immediately repairs only
that setting to the cleaned home directory, or to the filesystem root when the
home directory is not browsable, while preserving the other window settings.

Stop after conditions are process-local. They are never persisted and are not
restored after restart.

## Queue state

The queue survives a restart through two keys in `state.sqlite`. `queue.state`
holds the tracks and the queue's identity (its id, source kind, source
playlist, and name) and is rewritten only when one of those changes.
`queue.cursor` holds the current index and the play-next boundary, tagged with
the id of the queue they belong to, and is written on every save, so stepping
through a long queue costs a few bytes per track change. On load, a cursor
whose id matches the saved queue wins; otherwise the cursor fields that
`queue.state` may carry are used, and the index is clamped to the saved track
count either way. Saved and automatic queue snapshots live under
`queue.snapshots`.

## Overrides

CLI flags:

```sh
muzaiten --state-root /tmp/muzaiten-state    # everything under one root
muzaiten --data-dir /path/to/data
muzaiten --state-dir /path/to/state
muzaiten --cache-dir /path/to/cache
muzaiten --config-dir /path/to/config
muzaiten --dev-state                         # isolated ./dev-state profile
```

Environment equivalents:

```sh
MUZAITEN_STATE_ROOT=/tmp/muzaiten-state muzaiten
MUZAITEN_DEV_STATE=1 muzaiten
MUZAITEN_DATA_DIR=... MUZAITEN_STATE_DIR=... MUZAITEN_CACHE_DIR=... MUZAITEN_CONFIG_DIR=... muzaiten
```

`muzaitenctl` and `muzaiten-features` resolve the same `MUZAITEN_*`
environment, so client and analysis commands target the matching instance.
Explicit analyzer automation can additionally pass `--library`, `--features`,
and `--state` paths.

Semantic policy lives in `state.sqlite` under
`analysis.semantic.enabled` (default `false`),
`analysis.semantic.providerPath` (default automatic), and
`analysis.semantic.device` (default `auto`). Model weights live under the XDG
cache at `muzaiten/models/`, never in the data or state databases.

## Config file

On first run, muzaiten writes a commented template at
`$XDG_CONFIG_HOME/muzaiten/muzaiten.conf`. It can set `paths.data`,
`paths.state`, and `paths.cache`. CLI flags and environment variables take
precedence over the config file.

## Diagnostics

`muzaiten --check-storage` prints a `muzaiten-storage-health/1` JSON report and
exits without starting services or creating database files. It honors the same
path overrides and can create their parent directories. Exit 0 means preflight
passed (`ready` or `degraded` for optional-store warnings); exit 2 means a
required store failed. This checks paths, access and recognized schema versions,
not every database page or a dry run of all migrations.

Library, playlists, UI/queue state and listening history are required for normal
startup. Errors identify the store, resolved path and cause. The GUI offers
Retry and Close before playback, scanning or delivery starts. Offscreen/minimal
and demo runs instead print the structured failure on stderr and exit 2. The
report's `phase` distinguishes preflight from initialization failure.

Migrations commit atomically per store; a failed migration rolls back its logical
changes. No store is reset, replaced or redirected automatically. Newer required
schemas are rejected. Artwork cache and analysis-feature failures produce
warnings and allow degraded operation; an unavailable artwork cache is bypassed.

```sh
muzaiten --verbose
MUZAITEN_VERBOSE=1 muzaiten
```
