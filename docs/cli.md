# Command-line client (`muzaitenctl`)

`muzaitenctl` is installed alongside the app. Transport and queue commands
talk to a running instance over its IPC socket (resolved from the same
`MUZAITEN_*` environment as the app, so they target the matching instance).
Every command supports `--json` for machine-readable output.

## Transport and queue

```sh
muzaitenctl status            # current track + player state
muzaitenctl play | pause | play-pause | next | prev | stop
muzaitenctl seek +30          # relative seconds, or absolute mm:ss
muzaitenctl volume 70         # absolute 0-100, or +/-N
muzaitenctl rate 4            # 0-5 stars (rate raw <0-100>, rate clear)
muzaitenctl queue             # list the queue; `queue <n>` jumps to a row
muzaitenctl enqueue [--play|--next] <path...>
muzaitenctl play-file <path>  # append a file and play it
muzaitenctl raise             # show and focus the window
```

## Radio

```sh
muzaitenctl start-radio <path> | stop-radio
muzaitenctl radio-weights get | set '<json>'      # active scoring weights
muzaitenctl radio-weights save <n> | apply <n> | list | remove <n>
muzaitenctl radio-learn [--dry-run] [--min-samples N]
muzaitenctl radio-genre ignore <genre> | unignore <genre> | list
muzaitenctl genre-alias set <alias> <canonical> | remove <alias> | list
muzaitenctl genre-report      # folded genre vocabulary stats
```

All of these run client-side against the library database — no running app
needed. Weight changes are validated before writing and take effect on the
next radio session. `radio-learn` is suggestion-only: it saves a
`learned-YYYYMMDD` profile and never touches the active weights. See
[radio.md](radio.md) for the full engine guide.

## Search and audio analysis

```sh
muzaitenctl search [opts] [text]        # client-side folded search / fzf picker
muzaitenctl semantic-search "<text>"    # CLAP text-to-library search
muzaitenctl features-status             # features.sqlite coverage report
muzaitenctl duplicate-groups [--min-size N]
muzaitenctl pin-copy <group-id> <path> | unpin-copy <group-id>
```

Search details and query syntax: [search.md](search.md). The analysis
pipeline that produces `features.sqlite` (the in-app runner,
`muzaiten-index`, and the optional CLAP embedder): [radio.md](radio.md).

## Scrobble backfill

```sh
muzaitenctl scrobble-backfill <listenbrainz|lastfm>   # import history / sync play counts
muzaitenctl scrobble-backfill status | cancel
muzaitenctl scrobble-backfill reset <listenbrainz|lastfm>
```

The same controls live under `History > Scrobblers` in the app. A completed
ListenBrainz import stops early once it pages into the already-imported
range, so listens later added *behind* that range are never revisited. Run
`scrobble-backfill reset listenbrainz` to clear the completed marker and
force the next import to re-walk full history (imported-listen dedup keeps
re-walks safe); `reset` is refused while a backfill is running.

## Audio indexer

`muzaiten-index` is the standalone analyzer used by the app:

```sh
muzaiten-index scan --library library.sqlite --features features.sqlite --json
muzaiten-index scan --library library.sqlite --features features.sqlite --power background --progress
muzaiten-index scan --library library.sqlite --features features.sqlite --verbose
muzaiten-index status --features features.sqlite --json
```

`--power background|balanced|turbo` controls worker count and process
priority; `--jobs N` overrides only the worker count. `--verbose` writes
per-file timing lines to stderr. With `--progress`, stderr carries:

```text
progress <n>/<m> elapsed=<s> rate=<r> eta=<s>
phase grouping
phase features
progress <n>/<m> elapsed=<s> rate=<r> eta=<s>
```

Progress keeps one dialect across phases. `phase <name>` switches the domain
for the following `progress` lines (and may reset UI counters); the same
`progress n/m elapsed= rate= eta=` line shape is reused. After `phase
features`, `n/m` counts **stale representative groups** (missing, older, or
NULL `features.version` rows for the active DSP version), not files.
When a representative has a fresh persisted per-file scalar row, the indexer
copies it into the group row without decoding audio. Missing or stale per-file
rows use the same resolved `--power` / `--jobs` worker count as file analysis,
then backfill the per-file cache. Feature-row writes remain serialized on the
indexer thread for SQLite safety. A feature-only run also avoids rebuilding
already-complete content groups; `phase grouping` remains in the progress
protocol but is normally instantaneous on an unchanged store.

Both `muzaiten-index status --json` and scan JSON retain `featured_groups` as
the total number of existing feature rows and add `featured_fresh` /
`featured_stale` counts for the running build. `muzaitenctl features-status`
shows the same split and reports when the store's recorded DSP version differs
from the version expected by the installed binary. Missing or empty store
version metadata is displayed as `unknown`; row versions remain the authority.

`elapsed` is total scan wall time; `rate` and `eta` are **phase-local** recent
throughput (roughly the last minute within the current phase). Right after a
phase boundary the indexer may emit `rate=- eta=-` until the phase window is
warm enough (about 2 s of movement). There is no second stream name such as
`features-progress`.

Scan JSON may include feature-fill counters when that phase ran:
`feature_groups_processed`, `features_written` (includes NULL-scalar rows
written as current version), and `feature_groups_failed` (decode/analyze
exceptions that stay stale for a rerun). The inventory field
`featured_groups` remains the features-table row count, while
`featured_fresh` / `featured_stale` split that inventory by the version this
build expects. A stop that lands
during the features phase returns
`"canceled": true` and does not write the last-scan summary meta; completed
feature rows stay durable.

stdout remains JSON-only when `--json` is used.

## Related binaries

- `muzaiten-index` — the audio analysis indexer (identity, duplicate
  groups, DSP scalars). See [radio.md](radio.md) and
  [features-schema.md](features-schema.md).
- `muzaiten-import` — playlist conversion (`convert`/`youtube`
  subcommands). See [playlist-import-jsonl.md](playlist-import-jsonl.md).
- `tools/embedder` — optional CLAP embedding sidecar (Python/uv).
