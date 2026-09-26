# Library search

Press `4` to open the search view. Type to filter interactively; all terms
AND together in any order.

Matching is by **sound/shape, not encoding**: both the query and the library
are folded to a lowercase ASCII-leaning form before matching, so accents,
scripts and look-alike punctuation don't get in the way. `cafe` finds `Café`,
`11:11` finds `11∶11`, `don't` finds `don’t`, Greek and Cyrillic transliterate
(`σωκρατης` ↔ `sokrates`), Korean matches by romanization (`ak dan chil` →
`악단광칠`), Japanese by romaji (`sanshin no hana` → `三線の花`), and Chinese by
pinyin (`yinyue` → `音乐`). Picard/MusicBrainz `*sort` reading tags fill in
proper-noun readings (`utada` → 宇多田ヒカル). Typing the original script still
matches too, and a kanji or hanzi matches the words that contain it. Scripts
whose reading depends on the surrounding letters (CJK ideographs, Thai, Lao,
the Indic scripts, Sinhala, Myanmar, Khmer, Arabic, Hebrew, Syriac, Thaana)
match as written when typed in their own script, ignoring Arabic and Hebrew
vowel marks and letter variants.

Many scripts are romanized more than one way, so the library side keeps every
common reading and a query matches any of them:

| Script | Readings a query can use |
|--------|--------------------------|
| Latin | `muller` and `mueller` for `Müller`; `bjork`, `bjoerk` |
| Greek | letter by letter (`eycharisto`), modern (`efcharisto`), phonetic (`efharisto`, `yiorgos`) |
| Cyrillic | `khrushchev`, `hruschyov`, `hruscev`; Ukrainian, Belarusian and Serbian spellings when the text uses their letters |
| Armenian | Eastern (`hayastan`) and Western (`hayasdan`) |
| Korean | syllable by syllable (`hangukeo`), as pronounced (`hangugeo`), McCune-Reischauer (`pochonbo` for `보천보`) |
| Japanese | wapuro (`toukyou`), Hepburn (`tokyo`); common kanji through a seed dictionary |
| Chinese | toneless pinyin, with word readings for common polyphones (`yinyue` for `音乐`, `changcheng` for `长城`) |
| Thai | RTGS (`sawatdi`, `khopkhun`) and the common spelling (`sawasdee`, `khobkhun`, `jai`) |
| Lao | standard (`viangchan`) and French-style (`wiangjan`) |
| Arabic, Persian, Urdu | vowel marks when written; otherwise the consonants with ا و ي as vowels (`mhmud`), with a first-syllable a (`habibi`, `salam`), and in el-/ou/ee spelling (`nour`, `eljadeed`) |
| Hebrew, Yiddish | points when written (`bereshit`); otherwise soft (`tora`) and hard spellings with a first a (`shalom`, `torah`) |
| Syriac, Thaana | their vowel marks (`dhivehi`) |
| Indic, Sinhala, Myanmar, Khmer | common (`kamla`, `zindagi`, `dilvale`), with every inherent vowel (`kamala`), and the ee/oo spelling of film titles (`deewana`, `phool`, `dilwale`) |

Traditional and simplified Chinese characters match each other. Half-width
and full-width forms, styled "fancy text" letters (`𝓗𝓮𝓵𝓵𝓸`, `🅲🅻🅴🅰🅽`), macOS
decomposed file names, and digits of every script fold like their plain
counterparts.

### Fold data and conformance corpus

The fold is compiled at build time: `tools/foldgen.py` reads the tables and
Unicode's character database and writes one entry per code point, so folding
is a table lookup per character. The hand-authored tables live in
`src/search/fold/tables/`, one JSON file per script family (`latin`,
`punctuation`, `greek`, `cyrillic`, `armenian`, `georgian`, `kana`, `han`,
`hangul`, `thai`, `brahmic`, `abjad`, `other`); each file's `_doc` key
describes its fields. Letters the
tables don't list are derived from their Unicode names where the name spells
the sound (`LATIN SMALL LETTER D WITH HOOK` reads `d`, Ethiopic, Cherokee and
Canadian syllabics read their syllable names).

Per-character pinyin and traditional-to-simplified forms come from Unicode
CLDR via ICU: `tools/fold-han-data.py` regenerates `han_pinyin.tsv` and
`han_simplified.tsv` (Unicode License v3, see `UNICODE-LICENSE.txt` beside
them). Word-level readings and overrides are in `tables/han.json`.

The conformance file `src/search/fold/fold_cases.tsv` holds one UTF-8 case per
line: `input<TAB>primary fold`, optionally followed by `<TAB>`-separated
alternate readings. A row with one expected field checks the primary fold; a
row with more checks every distinct fold, in order. Blank lines and lines
beginning with `#` are comments.

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

A field can have more than one folded form: its display text and its sort
name (`The Wall` and `Wall, The`). Anchors and fuzzy matching check each form
on its own, so `^wall` and `the wall$` both find that title. Exclusion globs
with a leading or trailing `*` behave the same way.

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
