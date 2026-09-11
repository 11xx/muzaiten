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

### Fold data and conformance corpus

NFKC runs before folding, so half-width kana and full-width compatibility forms
share the same search representation, while Japanese iteration marks expand to
their repeated kana or kanji reading.

The shared fold resource is `src/search/fold/fold_tables.json`. Its top-level
keys are:

- `transliteration`: single-character script mappings for Latin, Greek, and
  Cyrillic characters that need explicit readings. Keys and values are strings.
- `kana`: hiragana character to Hepburn-romaji mappings.
- `yoon_prefix`: hiragana syllable to consonant-prefix mappings used with
  small `ゃ`, `ゅ`, and `ょ`.
- `small_vowel`: small yōon kana to the vowel appended to a yōon prefix.
- `kanji`: surface text to hiragana reading mappings for the seed dictionary.

The C++ fold compiles the file into its own object and parses it once at first
use. The Python fold can read the same UTF-8 file with its standard JSON
library. The conformance file
`src/search/fold/fold_cases.tsv` uses one UTF-8 case per line in the form
`input<TAB>expected`. Blank lines and lines beginning with `#` are comments;
the expected field is the complete folded output. An unmapped kanji remains in
the expected output so the original-script search path stays covered.

The result list always keeps a highlighted cursor you move with the arrow or
`Ctrl+P`/`Ctrl+N` keys while the search box keeps focus, fzf-style.
`Esc`/`Ctrl+G` first clears the query, then (when already empty) releases the
text box so `1` and `2` switch views; `/` jumps back into the box. The folded
index is cached on disk, so opening search is near-instant on a warm cache; a
cold or stale cache streams results in as it builds (and refreshes quietly in
the background, shown by a small "updating index" note). Deleting the cache
file is harmless — it rebuilds on next open.

Freshness is tracked by a persisted database identity and search-content revision.
SQLite triggers advance that revision for searchable track metadata, rating
overlays, MPD rows and library-root visibility; rollback also rolls back the
revision. Playback settings and scan bookkeeping do not invalidate the cache.
An unchanged warm load reads this small revision record rather than scanning
all tracks for counts or timestamps. Cache format 2 and library schema 19 carry
the identity/revision contract; older caches are rebuilt automatically.

Cache builders read local and MPD records under one database snapshot. Canceled
or failed builds do not publish a partial cache. A GUI build whose source changed
while it was working schedules another refresh. The status label's tooltip
reports its cache decision and source revision.

## Keybindings

An explicit rebuild bypasses the saved cache and reads the database, including
metadata changes that leave the file count and maximum modification time unchanged.
Rapidly superseded queries are coalesced before scanning the index.

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
| `ext:flac` | Exact file-extension filter (the name on disk) |
| `codec:vorbis` | Codec filter; differs from `ext:` for containers, so an `.oga` may be `vorbis`, `opus` or `flac`, and an `.m4a` may be `aac` or `alac` |
| `khz:>=96` | Sample rate ≥ 96 kHz |
| `hz:44100` | Sample rate = 44100 Hz |
| `kbps:>320` | Bitrate > 320 kbps |
| `ch:2` | Stereo (2 channels) |
| `rating:>=80` | User rating ≥ 80/100 |
| `year:>=2000` | Release year ≥ 2000 |
| `dur:>3:30` | Duration > 3 min 30 sec |

The codec is recorded when a file is scanned. A library scanned before codecs
were read separately from extensions still stores the extension there, so
`codec:alac` finds nothing until `Library > Force full rescan` re-reads those
files.

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
muzaitenctl search --cache-info       # JSON decision, source revision and record count
```

With no query in a terminal (and `fzf` installed) it launches an **fzf
picker** over the whole library — multi-line rows, romaji matches kanji,
`Enter` queues the selection and `Alt+Enter` plays it. Piped or without fzf,
a bare `search` dumps the whole library as TSV. First run builds the cache
(a few seconds); later runs reuse a fresh cache. Stale caches are rebuilt for
normal CLI searches and the picker without requiring `--refresh`.

`--cache-info` resolves the cache without displaying library contents. Its
`muzaiten-search-cache/1` report includes `reason`, `used_cache`, `rebuilt`,
`source_revision` and `track_count`. Reasons include `hit`, `content-revision`,
`database-identity`, `database-path`, `schema-version`, `fold-version`,
`missing-cache`, `invalid-cache` and `forced-refresh`. An invalid cache detected
after streaming has already emitted rows fails rather than mixing a second
database stream into those rows; retry with `--refresh` in that case.

For free-text *meaning* search ("melancholic shoegaze") press `Ctrl+S`
inside the Search view: a semantic search dialog embeds the description
with the CLAP provider and ranks the analyzed library by audio similarity,
with play/queue actions on the rich result rows. Repeat queries answer
instantly from the shared query-vector cache. The same engine drives
`muzaitenctl semantic-search`; see [radio.md](radio.md) for the analysis
pipeline behind it.
