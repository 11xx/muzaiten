# Runtime state and paths

By default, muzaiten uses XDG paths:

- data: `$XDG_DATA_HOME/muzaiten` or `~/.local/share/muzaiten`
- state: `$XDG_STATE_HOME/muzaiten` or `~/.local/state/muzaiten`
- cache: `$XDG_CACHE_HOME/muzaiten` or `~/.cache/muzaiten`
- config: `$XDG_CONFIG_HOME/muzaiten` or `~/.config/muzaiten`

The data directory holds the library (`library.sqlite`), playlists, and
audio-analysis results (`features.sqlite`); state holds listening history
and UI state; cache holds artwork and the search index.

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

`muzaitenctl` resolves the same `MUZAITEN_*` environment, so client
commands target the matching instance.

## Config file

On first run, muzaiten writes a commented template at
`$XDG_CONFIG_HOME/muzaiten/muzaiten.conf`. It can set `paths.data`,
`paths.state`, and `paths.cache`. CLI flags and environment variables take
precedence over the config file.

## Diagnostics

```sh
muzaiten --verbose
MUZAITEN_VERBOSE=1 muzaiten
```
