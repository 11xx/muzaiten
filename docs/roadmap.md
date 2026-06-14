# Roadmap

Versions are date-based (`YYYY.MM.DD.HHMMSS.g<sha>`), derived from the committed
HEAD. There is no separate semantic version.

## Shipped

- Read-only library browser: album-artist sidebar, album grid with cover art,
  expanded album track details, and an artist-wide track table.
- Library scanner with source-directory management, incremental rescans,
  missing-file marking, and cached artwork from folder and embedded images.
- Ratings: display, sorting, and explicit MusicBee-compatible rating-tag
  writeback (see [data-safety.md](data-safety.md)).
- Queue-based playback (ncmpcpp-influenced) through a GStreamer backend, with
  configurable output: PipeWire / PulseAudio / ALSA sinks and an exclusive
  bit-perfect ALSA mode, plus resume behavior.
- fzf-style library search across the whole library (including MPD tracks) that
  matches by sound/shape — diacritics, Greek/Cyrillic/Turkish, and Japanese
  romaji↔kana/kanji (with MusicBrainz reading tags) — backed by an on-disk folded
  cache, exposed in-app and through the `muzaitenctl` CLI (with an fzf picker).
- Scrobbling to both ListenBrainz and Last.fm (now-playing + completed listens).
- Optional MPD bridge for browsing MPD libraries alongside local sources.
- MPRIS service and persistent UI state.

## Planned / ideas

- MusicBrainz network lookup (IDs are already read from tags when present).
- Bit-depth-aware audio-quality scoring in search ranking.
- Broader playlist import/export.
- More search-fold scripts (Korean Hangul, Chinese pinyin, Arabic, Hebrew, Indic),
  sourced from CLDR/Unihan rather than hand-authored.
- Shared persisted settings with a `muzaitenctl config` command (e.g. a default
  fuzzy/exact mode for the CLI picker).
