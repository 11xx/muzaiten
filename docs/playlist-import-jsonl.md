# Playlist import — JSONL interchange format

This document specifies the JSON Lines (JSONL / NDJSON) format consumed by
muzaiten's playlist import. It is the single intermediate format into which
external playlist sources (YouTube Takeout, Soundiiz CSV exports, m3u/m3u8, and
others) are normalized before import. Conversion and any network enrichment are
performed externally; muzaiten only parses and matches the resulting file.

The schema defined here is the only supported revision. There is no versioning
and no backward-compatibility contract: a producer targets exactly this schema.

## File format

- UTF-8 encoded text. One JSON object per line (JSON Lines / NDJSON). There is no
  enclosing array.
- Blank lines are ignored. A line whose first non-whitespace character is `#` is
  treated as a comment and ignored.
- Each line must be independently valid JSON. A malformed line is skipped and
  counted; it does not abort the import. The import dialog reports the skip count.

## Optional first line — playlist header

If the first content line is an object whose only key is `playlist`, it names (and
creates, if absent) the target playlist instead of importing into a pre-selected
one. When the import is started from an existing playlist, the header is ignored.

```json
{"playlist":{"name":"Late Night 2019","comment":"from YouTube Takeout 2024-01"}}
```

| key       | type   | required | meaning                       |
|-----------|--------|----------|-------------------------------|
| `name`    | string | yes      | playlist name (created if absent, appended if present) |
| `comment` | string | no       | playlist-level note           |

## Item lines

Every non-header content line is one playlist item.

| key          | type    | required          | meaning                                                        |
|--------------|---------|-------------------|----------------------------------------------------------------|
| `title`      | string  | yes\*             | track title                                                    |
| `artist`     | string  | no                | primary artist; multiple artists as `"A, B"`                   |
| `album`      | string  | no                | album title (improves match disambiguation)                    |
| `durationMs` | integer | no                | duration in **milliseconds** (convert seconds × 1000)          |
| `addedAt`    | integer | no                | source playlist-added Unix timestamp in **seconds**            |
| `externalId` | string  | recommended       | namespaced source identifier, e.g. `"youtube:ID"`, `"isrc:..."` |
| `directPath` | string  | no                | local file path (e.g. from m3u); tried before text matching    |
| `comment`    | string  | no                | free-form note; displayed on the item and retained across edits |

\* `title` may be omitted only when `directPath` is present. A line carrying
neither `title` nor `directPath` is skipped.

Additional rules:

- Unknown keys are ignored.
- A negative or non-integer `durationMs` is treated as `0`.
- `addedAt` accepts a JSON integer or numeric string. It must be a positive,
  integral Unix timestamp in seconds within the supported date range; missing,
  zero, negative, fractional, malformed, and out-of-range values are ignored.
  It represents when the source playlist received the item, not when muzaiten
  imports it. JSONL line order remains the canonical playlist ordinal.
- Leading and trailing whitespace in string values is trimmed.
- `externalId` should be namespaced (`source:id`) so it remains unambiguous across
  sources. It is used for source link-back and to make re-import idempotent
  (an item whose `externalId` already exists in the target playlist is skipped).

## Example

```json
{"playlist":{"name":"Late Night 2019","comment":"from YouTube Takeout"}}
{"title":"Nightcall","artist":"Kavinsky","album":"OutRun","durationMs":258000,"externalId":"youtube:MV_3Dpw-BRY"}
{"title":"Resonance","artist":"HOME","durationMs":211000,"externalId":"youtube:8GW6sLrK40k","comment":"synthwave"}
{"directPath":"/mnt/music/HOME/Odyssey/01 Resonance.flac"}
```

## Source conversion guidance

The following notes describe how each source is expected to be mapped onto this
format by the external converter.

### YouTube Takeout

A Takeout playlist CSV provides a `Video ID` column (and a creation timestamp);
it carries no title, artist, or duration. For each video id, metadata is obtained
with `yt-dlp -J "https://www.youtube.com/watch?v=<ID>"`. When YouTube Music fields
are present (`track`, `artist`, `album`, `duration`), they are used directly;
otherwise the video `title` is split on `"Artist - Title"`. Each item carries
`externalId: "youtube:<ID>"` and `durationMs = duration × 1000`. One Takeout
playlist CSV maps to one JSONL file, with a `playlist` header derived from the CSV
filename. Requests should be rate-limited; `yt-dlp` is the slow step.

### Soundiiz CSV

Columns map directly: `Title → title`, `Artist → artist`, `Album → album`,
`Duration → durationMs` (parse `m:ss` or seconds), and `ISRC → externalId`
(`"isrc:<ISRC>"`). A non-empty, valid ISO-8601 `addedDate` maps to `addedAt` in
epoch seconds; empty or invalid dates are omitted. Converters should place
human-readable provenance in the playlist header comment rather than creating
synthetic item comments, for example:

```text
Source: Spotify via Soundiiz
Exported: 2020-11-09 20:48:16 UTC (filename timestamp)
File: Spotify - Forza - (484) playlistCSV1604954896.csv
```

If no plausible `playlistCSV<epoch>` suffix exists, use `File modified: … UTC
(filesystem timestamp)` instead. Filesystem time is export provenance, not a
playlist creation date. Import only fills a playlist comment when it is empty;
an existing non-empty playlist comment is never overwritten.

### m3u / m3u8

`#EXTINF:<seconds>,Artist - Title` followed by a path line maps to `directPath`
plus a split `title`/`artist`, with `durationMs = seconds × 1000`.
