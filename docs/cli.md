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

## Related binaries

- `muzaiten-index` — the audio analysis indexer (identity, duplicate
  groups, DSP scalars). See [radio.md](radio.md) and
  [features-schema.md](features-schema.md).
- `muzaiten-import` — playlist conversion (`convert`/`youtube`
  subcommands). See [playlist-import-jsonl.md](playlist-import-jsonl.md).
- `tools/embedder` — optional CLAP embedding sidecar (Python/uv).
