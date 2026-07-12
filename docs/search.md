# Library search

Press `4` to open the search view. Type to filter interactively; all terms
AND together in any order.

Matching is by **sound/shape, not encoding**: both the query and the library
are folded to a lowercase ASCII-leaning form before matching, so accents and
scripts don't get in the way. `cafe` finds `Café`, `bjork` finds `Björk`,
Greek/Cyrillic/Turkish transliterate (`σωκρατης` ↔ `sokrates`), and Japanese
matches by romaji — kana and common kanji romanize (`sanshin no hana` or
`san shin no ha na` → `三線の花`), with Picard/MusicBrainz `*sort` reading
tags filling in proper-noun readings (`utada` → 宇多田ヒカル). Typing the
original script still matches too.

The result list always keeps a highlighted cursor you move with the arrow or
`Ctrl+P`/`Ctrl+N` keys while the search box keeps focus, fzf-style.
`Esc`/`Ctrl+G` first clears the query, then (when already empty) releases the
text box so `1` and `2` switch views; `/` jumps back into the box. The folded
index is cached on disk, so opening search is near-instant on a warm cache; a
cold or stale cache streams results in as it builds (and refreshes quietly in
the background, shown by a small "updating index" note). Deleting the cache
file is harmless — it rebuilds on next open.

## Keybindings

| Key | Action |
|-----|--------|
| `Enter` | Add selected (or cursor) results to queue |
| `Alt+Enter` | Add to queue and play immediately |
| `↑` / `↓` / `Ctrl+P` / `Ctrl+N` | Move the result cursor |
| `PgUp` / `PgDn` / `Home` / `End` | Jump the cursor |
| `Tab` | Mark current row, advance cursor |
| `Ctrl+Space` | Toggle mark on current row |
| `Ctrl+A` | Mark all results |
| `Ctrl+F` | Toggle fuzzy mode (default: exact orderless substring) |
| `Esc` / `Ctrl+G` | Clear the query; press again to leave text-input mode |
| `/` | Return focus to the search box (from browse mode) |
| `Ctrl+S` | Open semantic search (describe the music) |
| `F5` (or re-press `4`) | Rebuild the search index |
| `Ctrl+scroll` | Adjust result row height |
| Double-click | Play now |
| Right-click | Full track context menu (play/queue/radio/taste/copy path/…) |

## Query syntax

| Pattern | Meaning |
|---------|---------|
| `miles blue` | Both "miles" AND "blue" anywhere in the record (orderless) |
| `!classical` | Negate: exclude matches |
| `^miles` | Prefix anchor: field must start with "miles" |
| `blue$` | Suffix anchor: field must end with "blue" |
| `'exact` | Force exact match in fuzzy mode |
| `artist:davis` | Match only in artist/album-artist field |
| `album:blue` | Match only in album title |
| `title:what` | Match only in track title |
| `path:/gak` | Match only in file path |
| `ext:flac` | Exact extension/codec filter |
| `khz:>=96` | Sample rate ≥ 96 kHz |
| `hz:44100` | Sample rate = 44100 Hz |
| `kbps:>320` | Bitrate > 320 kbps |
| `ch:2` | Stereo (2 channels) |
| `rating:>=80` | User rating ≥ 80/100 |
| `year:>=2000` | Release year ≥ 2000 |
| `dur:>3:30` | Duration > 3 min 30 sec |

Combine freely: `miles ext:flac rating:>=80 !live`

## Ranking and exclusions

`Settings > Search ranking…` opens a panel that controls how results are
ordered and what is filtered out:

- **Ranking criteria** — an ordered list (top = highest priority). Each row
  is a criterion with a direction: Relevance, Audio quality, Preferred
  directory (with a path), Library order (the same grouping as the rest of
  the app), or a single sort field. The default is **Relevance → Audio
  quality → Library order**, so the most relevant matches come first and,
  among equally relevant ones, higher-quality audio (lossless / higher
  sample rate) floats up. Move a criterion above Relevance to make it
  dominate the matched results.
- **Audio quality** scores lossless codecs first, then sample rate, then
  bitrate/channels. (Bit depth is not scanned yet; it will factor in once
  it is.)
- **Exclude patterns** — glob patterns (`*`, `?`) that drop results
  entirely, each scoped to the file path or to any field. Excluded tracks
  are skipped before ranking, so excluding large folders can make search
  faster. Examples: `*/Podcasts/*` (Path), `*.m4b` (Path), `*live*` (Any
  field).

Ranking and exclusion changes apply live and persist across restarts.

## Search from the terminal

`muzaitenctl search` runs **entirely client-side**, opening the library
database and the shared folded-index cache directly, so it works whether or
not the app is running:

```sh
muzaitenctl search sanshin            # TSV: path  title  artist  album  date  ms  rating
muzaitenctl search --plain utada      # human-readable blocks
muzaitenctl search --limit 5 --json jazz
muzaitenctl search --fuzzy nhuage     # fuzzy instead of exact substring
muzaitenctl search --refresh          # rebuild the cache; --clear-cache to drop it
```

With no query in a terminal (and `fzf` installed) it launches an **fzf
picker** over the whole library — multi-line rows, romaji matches kanji,
`Enter` queues the selection and `Alt+Enter` plays it. Piped or without fzf,
a bare `search` dumps the whole library as TSV. First run builds the cache
(a few seconds); later runs are instant.

For free-text *meaning* search ("melancholic shoegaze") press `Ctrl+S`
inside the Search view: a semantic search dialog embeds the description
with the CLAP provider and ranks the analyzed library by audio similarity,
with play/queue actions on the rich result rows. Repeat queries answer
instantly from the shared query-vector cache. The same engine drives
`muzaitenctl semantic-search`; see [radio.md](radio.md) for the analysis
pipeline behind it.
