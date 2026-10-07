# Command-line client (`muzaitenctl`)

`muzaitenctl` is installed alongside the app. Transport and queue commands
talk to a running instance over its IPC socket (resolved from the same
`MUZAITEN_*` environment as the app, so they target the matching instance).
Every command supports `--json` for machine-readable output.

`status` adds an `output:` line when the chosen output is missing (waiting,
or playing through the system output) or cannot be used by the transport.
`status --json` always carries an `output` object: `route` (`system`,
`preferred`, `waiting`, `fallback`, `direct` or `unsupported`), `transport`,
an optional `transport_note`, the `preferred` output's stable name, label and
availability when one is chosen, and `current`, where the stream is linked,
when the transport reports it.

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
muzaitenctl radio-weights get | set '<json>'      # active profile weights
muzaitenctl radio-weights save <n> | apply <n> | list | remove <n>
muzaitenctl radio-weights export <file> [--profile <name>] [--allow-learned] [--replace]
muzaitenctl radio-weights import <file> [--dry-run] [--replace]
muzaitenctl radio-learn [--dry-run] [--min-samples N]
muzaitenctl radio-genre ignore <genre> | unignore <genre> | list
muzaitenctl genre-alias set <alias> <canonical> | remove <alias> | list
muzaitenctl genre-report      # folded genre vocabulary stats
```

All of these run client-side, with no running app needed. `radio-weights`
edits the named profiles in `radio-profiles.json`: `set` changes the active
profile, `save` snapshots it under a name, `apply` selects a profile, and
`list` and `remove` manage saved profiles. Changes take effect when muzaiten
next starts. `radio-learn` is suggestion-only: it saves a
`learned-YYYYMMDD` profile and never changes the active profile. See
[radio.md](radio.md) for the full engine guide.

`export` writes a canonical `muzaiten-radio-profile` version 1 JSON file with
the name, origin, weights and session decay, using the active profile unless
`--profile` names another. Learned or unknown-origin profiles require
`--allow-learned` because their tuning can reveal listening taste; unknown
origins are exported as learned. Existing export files require `--replace`.

`import --dry-run` prints the validated profile and whether a same-name local
profile exists, without writing or migrating stores. `import` saves it without
selecting it; a same-name profile requires `--replace`. These subverbs print
the contents they exchange. With `--json`, they return `ok`, `action`,
`dry_run`, `requires_replace`, `contents` and a `profile` object containing the
full exchange payload. `requires_replace` reports a name collision on import;
it is false on export. Existing radio-weight verbs retain their output shapes.
Files exceeding 64 KiB or failing strict format validation are refused without
changing the recipient store. No network connection is needed. Close a running
app before CLI import; import refuses while the app is open. Export and dry-run
can read saved profiles while it runs.

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
muzaiten-features model download --components audio   # 285 MB, radio/analysis only
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
`--semantic-decode-workers N` sets the optional CLAP provider's concurrent
audio decodes for that refresh; omit it to use the provider default.
`--staging-bytes N` (suffixes `K`, `M`, `G`; `0`, the default, disables it)
lets a small reader pool stage upcoming files' compressed bytes in memory,
up to `N` bytes ahead of the decoders, which then feed ffmpeg from that buffer
instead of reading the medium themselves. It helps when the library sits on a
network mount, where reading the file is most of what a decode slot waits
for; on local storage it changes little. When the flag is absent the
`analysis.stagingBytes` setting in `state.sqlite` supplies the value. A
format that needs a seekable input (an m4a whose index trails the audio)
fails on the pipe, is decoded from its path once, and that extension is read
from the path for the rest of the run; the scan JSON lists such extensions
under `staging.fallback_extensions`, and its `timings.stage_wait` aggregate
shows how long decode slots waited for staged bytes.
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
If a stop is observed immediately after native analysis completes, exit 130
still carries `"canceled": true` in JSON and JSONL output. Native counters can
describe a fully completed phase; its committed rows remain durable.

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

## Local scalar feature packs

```sh
muzaiten-features pack export scalars.ndjson [--features PATH] [--dry-run] [--replace] [--json]
muzaiten-features pack import scalars.ndjson [--features PATH] [--dry-run] [--replace] [--json]
```

`--features` selects the feature store; otherwise the application's resolved
data directory supplies `features.sqlite`. Both operations require an
existing schema-5 store and use the native feature-writer lock. Run
`muzaiten-features refresh --stage identity --no-semantic` before importing
to establish current local audio hashes. Export reads each group
representative's own current per-file analysis, which identity refresh
writes, without analyzing audio, invoking a provider or using the network.
Groups without that analysis are skipped, and local rows whose hash or values
fail pack validation are skipped and counted as `invalid`.

`--dry-run` previews the fully validated operation without writing feature
rows or a pack file. `--replace` permits overwriting an existing export
file, or replacing current recipient group rows on import. Without it, export
refuses an existing destination and import counts current rows as skipped.
Import replaces stale rows, written by another DSP version, either way,
because they never enter scoring. Import requires exact representative decoded-audio
hashes and the running binary's scalar extractor/version. Other incoming
records are counted as mismatched, and local unmatched files/groups are
reported separately. Fingerprints and MusicBrainz IDs cannot authorize
adoption. Adopted current rows are skipped by the next analysis; a DSP
upgrade invalidates them normally.

Plain and JSON output disclose that taste/history is absent but content
hashes identify recordings. The strict v1 payload contains no paths,
filenames, timestamps, ratings or embeddings. Files exceed neither 64 MiB,
100,000 records nor 16 KiB per line. Export uses atomic file replacement;
import validates the entire file before store access and writes matching
rows in one transaction. Exit codes are 0 success/no-op, 2 invalid input,
4 operational failure and 5 writer lock busy. See
[features-schema.md](features-schema.md#local-feature-pack-format) for
the exact keys, NULL handling, report fields and rejection rules.

## Related binaries

- `muzaiten-features` — native audio analysis plus optional semantic-provider
  orchestration. See [semantic-analysis.md](semantic-analysis.md), [radio.md](radio.md), and
  [features-schema.md](features-schema.md).
- `muzaiten-import` — playlist conversion (`convert`/`youtube`
  subcommands). See [playlist-import-jsonl.md](playlist-import-jsonl.md).
- `muzaiten-features-clap` — optional protocol provider installed separately.
  It is not a human-facing CLI; invoke it through `muzaiten-features`.
