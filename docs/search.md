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
the Indic scripts, Sinhala, Myanmar, Khmer, Arabic, Hebrew, Syriac, Thaana,
Tibetan)
match as written when typed in their own script, ignoring Arabic and Hebrew
vowel marks and letter variants.

Many scripts are romanized more than one way, so the library side keeps every
common reading and a query matches any of them:

| Script | Readings a query can use |
|--------|--------------------------|
| Latin | `muller`, `mueller` and `miller` for `Müller`; `bjork`, `bjoerk` |
| Greek | letter by letter (`eycharisto`), modern (`efcharisto`), phonetic (`efharisto`, `yiorgos`) |
| Cyrillic | `khrushchev`, `hruschyov`, `hruscev`; Ukrainian, Belarusian and Serbian spellings when the text uses their letters |
| Armenian | Eastern (`yerevan`, `hayastan`) and Western (`hayasdan`) |
| Korean | syllable by syllable (`hangukeo`), as pronounced (`hangugeo`), McCune-Reischauer (`pochonbo` for `보천보`), conventional name spellings (`lee jieun` for `이지은`, `kim taehyung` for `김태형`, `kye young` for `계영`) |
| Japanese | wapuro (`toukyou`), Hepburn (`tokyo`); common kanji through a seed dictionary |
| Chinese | toneless pinyin; every authored reading of a polyphonic character can match, and choices combine across a title. Word readings settle their whole span (`yinyue` for `音乐`, `chongqing` for `重庆`) |
| Thai | RTGS (`sawatdi`, `khopkhun`) and the common spelling (`sawasdee`, `khobkhun`, `jai`); words spelling rules misread come from a word table (`ching` for `จริง`) |
| Lao | standard (`viangchan`) and French-style (`wiangjan`) |
| Arabic, Persian, Urdu | vowel marks when written; otherwise the consonants with ا و ي as vowels (`mhmud`), with a first-syllable a (`habibi`, `salam`), and in el-/ou/ee spelling (`nour`, `eljadeed`) |
| Hebrew, Yiddish | points when written (`bereshit`); otherwise soft (`tora`) and hard spellings with a first a (`shalom`, `torah`) |
| Syriac, Thaana | their vowel marks (`dhivehi`) |
| Tibetan | Wylie, syllables joined (`smonlam`, `lhasa`, `bka'`), and without the a-chung apostrophe (`bka`) |
| Indic, Sinhala, Myanmar, Khmer | common (`kamla`, `zindagi`, `dilvale`), with every inherent vowel (`kamala`), and the ee/oo spelling of film titles (`deewana`, `phool`, `dilwale`) |

For conventional Korean name spellings, the first syllable of each Hangul word
uses the surname table, with two-syllable surnames matched before one-syllable
ones. Remaining syllables use common given-name spellings when listed and
Revised Romanization otherwise.

Thai native marks remain distinct even when their readings are related: `ป็อด`
folds to `poot`/`pood` and does not match native `ป๊อด`, whose readings
include `pot`/`pod`. Japanese kanji readings come from a seed list: `三線`
has `sanshin`, while standalone `線` has no Japanese `sen` reading.

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

Han readings and simplified forms of the essential ideographs are
hand-authored in `src/search/fold/tables/han/`: one file per national
standard set (`gb2312`, `big5`, `jis`, `ksx`), each listing the ideographs no
earlier set has. Each tab-separated row has an ideograph, its Hanyu Pinyin
readings with tone marks (most common first; `-` for none), and the
simplified form a traditional or variant ideograph folds to (empty when it
keeps its written form).

Each variant family folds to a BMP common simplified form when a reference
gives one, otherwise to a General Standard form, including astral forms.
When neither exists, its BMP member remains the fold key and astral variants
fold back to it.

The authored files proceed through `tgh`, `big5-2`, `core`, `uro`, `exta`,
`extb`, `extc` and `cns` after the four essential sets. A file declares full
coverage with a `# complete` header line; a file without it may still be
partial while its set is being authored.

Other ideographs with Unihan data read from Unicode's Unihan database:
`tools/fold-han-data.py` generates `src/search/fold/unihan/han_pinyin.tsv`
and `han_simplified.tsv` from a pinned Unihan release, leaving out the
authored ideographs (Unicode License v3, see `UNICODE-LICENSE.txt` there).
The generator and checker validate the archive hash for Unihan 16.0.0,
independently of Python's `unicodedata.unidata_version`. Generated keys must
lie in the tool's ideograph blocks and have category `Lo` in the running
Python's Unicode database; reference entries for unrecognized characters are
excluded. Generation is byte-repeatable with the same reference, authored
tables, and runtime categories. The TSV headers identify the Unihan reference.
`tools/fold-han-data.py check` compares the authored tables with Unihan and
Taiwan's CNS 11643 open data for reading attestation,
including the exact tone of every reading across simplified and traditional
counterparts. It reports `family-target` disagreements for astral variant
families and `variant-guard` findings for unsupported semantic variant folds.
With no file arguments it requires the four essential tier files and checks
rare files that exist; explicit file arguments select the character files, while the
word file is still checked from `--words` or `words.tsv` in the authored
directory. Reviewed rows in `tools/fold-han-reviewed.tsv` match an ideograph
or word, finding kind, authored value and reference value, followed by the
reason for the decision. Rows in `tables/han/words.tsv` use
`word<TAB>reading[<TAB>second reading]`: the word may use any Han spelling;
its key folds to simplified forms at build time, and rows whose keys coincide
merge their readings. Each tone-marked reading has one syllable per character.
Each polyphonic word span or unmatched character contributes its readings in
authored order. Search combines these choices across a title, including
choices in separate Han runs. A matched word supplies its whole reading; its
characters do not add independent alternatives. The all-first and all-second
combinations come first, then other combinations with fewer changed
positions. A title contributes up to 64 pinyin variants; longer titles
enumerate whitespace- or slash-separated runs in lockstep when their full
product exceeds that limit.
Chinese word lookup folds
each character to its simplified form first, so a word row also supplies the
reading for traditional and variant spellings. Japanese seed readings match the written characters. Han runs use forward and backward longest
matches; the segmentation with fewer pieces wins, then the one with fewer
single characters. When those counts tie, the segmentation that leaves more
function words as single characters wins; backward matching breaks the final
tie. Every word row supplies its reading, including rows whose reading agrees
with the simplified characters’ defaults.

The Han checker also checks word format, each syllable against its character's
authored reading or Unihan default, and each toneless reading against
CC-CEDICT. An identical second reading is a format finding. Findings are
`format`, `duplicate` (a row adds no reading to an earlier row with the same
key), `joined-key` (the key hides the first reading of a spelling that folds
into it), `reading`, `cedict` (the dictionary lists the word only with other
readings), `uncommon` (the dictionary has no entry), `edge-particle` (a word
edge lacks a particle reading), or `opencc` (the authored simplified form
differs from OpenCC's first mapping).
`edge-particle` reports a word whose first or last character lacks its particle
reading in both authored readings; a reviewer adds that reading or records why
the word should keep its existing readings. `opencc` reports a simplified form
that differs from OpenCC's first traditional-to-simplified mapping, including
another listed target; a reviewer chooses the intended fold or records the
disagreement. CC-CEDICT and OpenCC are check-only references and never ship.
The checker downloads them to
`$XDG_CACHE_HOME/muzaiten-tools/` (`~/.cache/muzaiten-tools/` by default, apart
from the application's own cache) on first use and prints its date line with the word
findings. Use `tools/fold-han-data.py check --words FILE` to check a word table
outside the repository. Reviewed word findings match the word, kind, ours and
reference fields in the same way as character findings.

The conformance file `src/search/fold/fold_cases.tsv` holds one UTF-8 case per
line: `input<TAB>primary fold`, optionally followed by `<TAB>`-separated
alternate readings. A row with one expected field checks the primary fold; a
row with more checks every distinct fold, in order. Blank lines and lines
beginning with `#` are comments.

`python3 tools/fold-sweep.py --base master` compares the current working tree's
fold segments with a detached base revision that provides `Fold::foldSegments`.
The sweep owns a detached worktree at `muzaiten-tools/fold-sweep/base` in the
same cache directory; `--base-tree DIR` selects another detached tree, and a
tree checked out on a branch is refused. The base build sits beside the tree,
in `<tree>-build`. The sweep builds only the search library in each tree and
checks CC-CEDICT spellings and readings, the phrase corpus
`tools/fold-sweep-phrases.tsv`, optional `--corpus FILE` line corpora labelled
`corpus:<stem>`, and an optional `--library FILE` copy of a library database. Each
phrase row is `text<TAB>expected fold`, where the expected fold is an ordinary
reading or the simplified form a query written in the other script folds to.
The library option refuses files inside the live data directory and counts
skipped values containing line breaks or the segment separator.
`--report FILE` writes every changed or broken row as TSV; `--examples N`
limits printed examples per class.

`worse` loses an expected reading, `better` gains one, and `still-broken` marks
an expected reading (a phrase's or a CC-CEDICT entry's) absent on both sides. `primary` changes the
first segment, `lost` removes another segment, `gained` adds one, and
`reordered` changes only the order of alternate segments. Unchanged
rows appear only in counts. `--fail-on-regression` exits with status 1 if any
expected reading that worked in the base stops working in the current tree.
Input, checkout, and build errors exit with status 2.

The result list always keeps a highlighted cursor you move with the arrow or
`Ctrl+P`/`Ctrl+N` keys while the search box keeps focus, fzf-style.
`Esc`/`Ctrl+G` first clears the query, then (when already empty) releases the
text box so `1` and `2` switch views; `/` jumps back into the box. The folded
index is cached on disk, so opening search is near-instant on a warm cache;
loading it also builds in-memory character masks that let a query skip fields
that cannot match. As a query narrows while typing, search rechecks only the
previous matches; an index update or changed exclusions starts a fresh scan,
and a superseded query stops scanning. A cold or stale cache streams results
in as it builds (and refreshes quietly in the background, shown by a small
"updating index" note). Deleting the cache file is harmless — it rebuilds on
next open.

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

A fuzzy free-text term matches a file path within one folder name or the
file name, the library's own root folders included, so letters scattered
across several folders do not add up to a match. A term containing `/`, and
any `path:` term, matches the whole path, across folders.

Literal text in exclusion globs folds like a search query, so `*café*`
matches `Café`, and `*東京*` matches the simplified primary fold of `東京`.
`*` and `?` remain wildcards, and each glob matches one folded form at a time.

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
  stay out of ranking and the result list. Examples: `*/Podcasts/*` (Path),
  `*.m4b` (Path), `*live*` (Any field).

Ranking and exclusion changes apply live and persist across restarts.
For a nonempty GUI query, a muted count beside the result count shows how many
matching tracks the exclusion patterns hide. It counts each track once, even
when several patterns match, and remains visible when no results remain. It
does not include tracks omitted by the display limit. Terminal searches,
including the fzf picker, ignore the GUI's saved exclusion patterns and search
the full indexed library.

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
