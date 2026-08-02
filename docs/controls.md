# Controls and keybindings

## Global

- `1` — queue view (press again to reveal the currently playing row)
- `2` — library panels view (artist sidebar, album grid, track table)
- `3` — toggle between the library explorer and free-roam file explorer
- `4` — library search view ([search.md](search.md) has the full key
  table and query syntax)
- `5` — playlist management view ([playlists.md](playlists.md))
- `o` — find the current track in the active library view
- `r` / `s` — cycle repeat / shuffle modes (shuffle includes the
  taste-aware Radio shuffle; right-click a mode button to reset it)
- `Ctrl+scroll` over the queue, search results, or file explorers —
  adjust row height

Keybindings are customizable at `Settings > Keybinds…`.

## Context menus

Every track surface (library table, queue, playlists, search results,
explorers, music explorer) shares one menu vocabulary: play/queue actions,
`Add to playlist…`, `Start Radio`, the taste flags (`Never play on
radio`, `Don't learn from this`), `Find in library`, `Open containing
directory`, `Copy path`, and `Properties`. Albums and artists carry
collection-level equivalents (play/queue all, add album to playlist,
start radio). Right-clicking **empty space** opens view-level menus:
queue-wide actions in the queue, play-all/layout in the track table,
search options in search, sort/alignment in the album views.

Refreshing a track list or playlist keeps your selection, the keyboard cursor, and the scroll position on the same rows, even when the rows move.

## Settings worth knowing

- `Library > Source directories…` — scan-enabled and library-visible roots;
  `Library > Link roots…` maps stored paths to local mounts.
- `Library > Audio analysis ▸` — run the audio analyzer, choose
  `Analysis power ▸ Background`, `Balanced`, or `Turbo`, inspect coverage,
  manage duplicate copies ([radio.md](radio.md)). `Stop analysis` keeps all
  completed work — running the analyzer again resumes where it stopped —
  and changing the power during a scan applies immediately by restarting
  the scan at the new level.
- `Playback > Output profile…` — output device, software volume,
  resampling, sink release on pause, preload. Native bit-perfect DSD
  (`.dsf`) needs a direct-capable device selected here with resampling
  off; with **Allow resampling** DSD decodes to PCM for shared output.
- `Playback > Resume behavior…` — whether position and playback state are
  restored across launches.
- `Radio > Customization…` / `Radio > Genre curation…` — engine tuning
  ([radio.md](radio.md)).
- `Settings > Track information panel…` — fields shown in the right-side
  info pane. `Settings > Album art resolution…` — cached artwork size.
- `History > Scrobblers ▸` — ListenBrainz/Last.fm toggles, offline
  buffering, history backfill; `Last.fm API settings…` accepts your own
  credentials.

## File explorers

Selectable keybinding profiles from the explorer context menu:

- **Vim-style**: `j/k/h/l` move, `Space` play, `a` add, `p` play next,
  `i` import, `f` open containing directory, `b` jump to start folder,
  `~` home.
- **Emacs/Dired-style**: `n/p` move, `Space` play, `s` add, `!` play
  next, `i` import, `f` open containing directory, `b` jump to start
  folder, `~` home.

`Backspace` and the icon-only **Go up one directory** button navigate to the
parent directory in both explorers. The button does not take keyboard focus.
The free-roam explorer can set a start folder from its context menu; `b` jumps
back to it. Requests for empty, missing, non-directory, or unreadable locations
leave the free-roam explorer at its current location.
The scanner and free-roam explorer recognize AAC, AIFF, Monkey's Audio, DSD,
FLAC, MP2/MP3, MP4/M4A/M4B, Matroska Audio, Musepack, Ogg
(`.oga`, `.ogg`, `.ogx`, `.opus`), True Audio, WAV, Windows Media Audio, and
WavPack files. Playback still requires a matching GStreamer decoder plugin.
Unsupported files are hidden by default — enable `View > List unsupported
files in explorer` while in an explorer view to show them.
