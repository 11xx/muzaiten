# Demo screenshots

Run `make demo-screens` from the repository, or use `make -C /path/to/muzaiten
demo-screens`. The target builds the application and captures each view in a
disposable profile. Python 3 provides SQLite online backups, including committed
WAL data, from the default XDG data, state and cache directories on every run.
An inherited `MUZAITEN_STATE_ROOT` does not select the source profile.
Unsaved state in another process cannot be copied; closing the player first
also gives the independent database snapshots a consistent stopping point.
The copies live beside the output directory and are removed after the run.
Captures use silent audio and disable scrobbling.

```sh
make demo-screens \
    DEMO_THEMES="KvGnome KvGnomeDark" \
    DEMO_SIZE=1440x900 \
    DEMO_LIBRARY_ARTIST="OVERWERK" \
    DEMO_LIBRARY_ALBUM="State" \
    DEMO_LIBRARY_TRACK="overwerk need" \
    DEMO_QUEUE_TRACK="nightwish greatest show" \
    DEMO_PLAYLIST_NAME="Favorites" \
    DEMO_PLAYLIST_TRACK="rainbow stargazer" \
    DEMO_FILE_EXPLORER_LIBRARY_PATH="/srv/music/Rainbow/Rising" \
    DEMO_FILE_EXPLORER_LIBRARY_TRACK="Stargazer" \
    DEMO_FILE_EXPLORER_SYSTEM_PATH="/srv/music" \
    DEMO_FILE_EXPLORER_SYSTEM_TRACK="Rainbow" \
    DEMO_NOW_PLAYING="nightwish greatest show on earth remastered" \
    DEMO_NOW_PLAYING_STATE=playing \
    DEMO_NOW_PLAYING_POSITION=0.6667 \
    DEMO_SEARCH="sanshin no hana"
```

`DEMO_LIBRARY_ARTIST` selects the artist sidebar; `DEMO_LIBRARY_ALBUM`
highlights the middle album grid. `DEMO_ARTIST` and `DEMO_ALBUM` remain aliases;
an explicitly supplied view-prefixed variable takes precedence.
`DEMO_LIBRARY_TRACK` selects within the highlighted album's tracklist.
`DEMO_QUEUE_TRACK` selects a queue row independently of playback; when omitted
or unmatched, it reveals the now-playing row, then retains an available selection
or selects the first row.

Playlist names match case-insensitively. Track selectors use the panel search
parser and matcher: words can match across fields in any order, with the same
folding and query syntax as `/`. For example, `the asteroids galaxy tour hurricane`
matches an artist and title stored in separate columns. The first matching row
in display order is selected; selection does not filter the screenshot's rows.
File explorer selectors search the displayed name/title, artist and album only,
not hidden paths or on-disk filenames when a metadata title is displayed.
Use `Stargazer (rough mix)`, not `5. Stargazer (rough mix).flac`, for a tagged track.
Library, playlist and explorer selections fall back to the first available row
when omitted or unmatched; the queue uses the priority described above. Invalid
library directories fall back to library roots; invalid system directories
retain the copied explorer location, with its normal missing-directory recovery.
Empty collections produce empty views.

File explorers show the full filename including its extension for an untagged
track, and muted em dashes for empty artist/album cells. Stored metadata is not
rewritten by this display fallback. Trailing slashes on explorer directories
are normalized before lookup.

`DEMO_NOW_PLAYING` accepts a library path or search terms. A match absent from
the queue is appended to the disposable queue and made current, without playing
audio. An unmatched query uses the first queue track, then the first library
track; an empty library and queue leave the player bar empty. `playing` and
`paused` control the visual state, and the position ratio is clamped to 0–1.

The default output is `demo-screens/<theme>/`, with six images:
`01-library.png`, `02-search.png`, `03-queue.png`, `04-playlists.png`,
`05-file-explorer-library.png`, and `06-file-explorer-system.png`.
`DEMO_SCREEN_DIR` changes the output root. Each run captures into a fresh staging
directory, optimizes those images only, then replaces matching output files.
The obsolete `05-explorer.png` and `02-search.mp4` outputs in regenerated themes
are removed; other files and themes are left alone. A capture or optimizer failure leaves
the previous output intact; a successful run reports how many files it replaced.

`DEMO_SEARCH_VIDEO=1` (default) produces a looping APNG when `DEMO_SEARCH` is
nonempty; `0` produces a still at the same path. FFmpeg is required for animation.
`DEMO_SEARCH_DELAY_MS` controls typing speed. APNGs bypass PNG optimizers.
For stills, `pngquant` may deliberately keep the original when compression would
increase its size or miss its quality threshold; those skips succeed.
`DEMO_OPTIMIZE_PNG=0` disables optimization, and `DEMO_PNG_LOSSY=0` disables
only quantization. Missing optional optimizers produce warnings.

The hidden application options use the equivalent kebab-case names, such as
`--demo-library-artist`, `--demo-playlist-name`, and
`--demo-file-explorer-system-path`. Direct `--demo-screens` invocation requires
an isolated state root and does not copy a profile; use the Make target for a
fresh snapshot of the default profile.
