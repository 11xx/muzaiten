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
                                        #   [--limit N] [--no-cache]
muzaitenctl features-status             # features.sqlite coverage report
muzaitenctl duplicate-groups [--min-size N]
muzaitenctl pin-copy <group-id> <path> | unpin-copy <group-id>
```

Search details and query syntax: [search.md](search.md). The analysis
pipeline that produces `features.sqlite` (the in-app runner,
`muzaiten-features`, and the optional CLAP embedder): [radio.md](radio.md).

Repeated `semantic-search` texts answer from a persistent query-vector
cache (`semantic-query.sqlite` under the cache directory) keyed by the
active model identity, skipping the provider process entirely; pass
`--no-cache` to force a fresh provider embedding. The cache is disposable:
deleting the file only costs the next query a warm-up.

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

## Feature orchestration

`muzaiten-features` is the native analyzer and optional-provider orchestrator
used by the app. Paths default through the same XDG/AppPaths rules as the GUI:

```sh
muzaiten-features refresh --json
muzaiten-features refresh --semantic --progress=jsonl
muzaiten-features refresh --no-semantic --power background
muzaiten-features status --json
muzaiten-features doctor
muzaiten-features model download --progress=jsonl
muzaiten-features query "warm piano with brushed drums" --json
muzaiten-features neighbors --force
```

Semantic analysis reads `analysis.semantic.enabled` from `state.sqlite` and is
disabled by default. `--semantic` and `--no-semantic` override one refresh
without changing the saved setting. Native-only refreshes do not discover or
start Python. `--provider PATH` overrides provider discovery.

`--power background|balanced|turbo` controls native worker count and process
priority; `--jobs N` overrides only the worker count. `--verbose` writes
per-file diagnostics to stderr. `--progress=jsonl` writes versioned phase,
progress, and one terminal result event to stdout; stderr remains diagnostic.
After the scalar-feature phase, counters describe stale representative groups,
not files. Provider embedding and neighbor events use the same JSONL stream.
When a representative has a fresh persisted per-file scalar row, the indexer
copies it into the group row without decoding audio. Missing or stale per-file
rows use the same resolved `--power` / `--jobs` worker count as file analysis,
then backfill the per-file cache. Feature-row writes remain serialized on the
indexer thread for SQLite safety. A feature-only run also avoids rebuilding
already-complete content groups. On normal changed-file scans, `phase grouping`
rechecks only the new or changed tracks and the previous groups they can split;
unaffected groups retain their connectivity and ids without pairwise
Chromaprint work. Initial scans and recovery from pre-existing ungrouped rows
still perform an exact full regroup. `phase grouping` remains in the progress
protocol and is normally instantaneous on an unchanged store.

Both `muzaiten-features status --json` and refresh JSON retain `featured_groups` as
the total number of existing feature rows and add `featured_fresh` /
`featured_stale` counts for the running build. `muzaitenctl features-status`
shows the same split and reports when the store's recorded DSP version differs
from the version expected by the installed binary. Missing or empty store
version metadata is displayed as `unknown`; row versions remain the authority.

Rates and ETAs are phase-local. Exit codes are 0 success/no-op, 2 invalid input,
3 missing optional provider/model component, 4 operational failure, 5 lock
busy, and 130 canceled.

Refresh JSON may include feature-fill counters when that phase ran:
`feature_groups_processed`, `features_written` (includes NULL-scalar rows
written as current version), and `feature_groups_failed` (decode/analyze
exceptions that stay stale for a rerun). The inventory field
`featured_groups` remains the features-table row count, while
`featured_fresh` / `featured_stale` split that inventory by the version this
build expects. A stop that lands
during the features phase returns
`"canceled": true` and does not write the last-scan summary meta; completed
feature rows stay durable.

`--json` emits one terminal object and cannot be combined with
`--progress=jsonl`.

## Related binaries

- `muzaiten-features` — native audio analysis plus optional semantic-provider
  orchestration. See [semantic-analysis.md](semantic-analysis.md), [radio.md](radio.md), and
  [features-schema.md](features-schema.md).
- `muzaiten-import` — playlist conversion (`convert`/`youtube`
  subcommands). See [playlist-import-jsonl.md](playlist-import-jsonl.md).
- `muzaiten-features-clap` — optional protocol provider installed separately.
  It is not a human-facing CLI; invoke it through `muzaiten-features`.
