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
`Add to playlist…`, `Start Radio` (`Start Radio (N)` for a multi-selection), the
taste flags (`Never play on radio`, `Don't learn from this`), `Find in library`,
`Open containing directory`, `Copy path`, and `Properties`. Albums and artists
carry collection-level equivalents (play/queue all, add album to playlist,
start radio). A mixed selection ignores rows that are not in the library; the
first library track plays and the other library tracks become equal starting
points. Right-clicking
**empty space** opens view-level menus: queue-wide actions in the queue,
play-all/layout in the track table, search options in search, sort/alignment in
the album views.

Refreshing a track list or playlist keeps your selection, the keyboard cursor, and the scroll position on the same rows, even when the rows move.

## Stop after

Choose `Playback > Stop after`, or right-click anywhere on the player timeline.
Both offer `15 minutes`, `30 minutes`, `1 hour`, `2 hours`, `Current song`, `3
songs`, `5 songs`, `10 songs`, and `Custom…`. `Custom…` opens a dialog with
minute values from 1 to 1440 or song values from 1 to 999. Arming a new
condition replaces the previous one. `Cancel Stop after` is available while a
condition is armed.

Song conditions count songs that finish naturally. Manual Next, Previous,
direct queue jumps, seeking, and pausing do not count. A deadline pauses
playback in place, and Play resumes the same position. After the last counted
song, an existing next song is ready to play but remains paused; if there is no
existing next song, playback stops on the completed song. Queue contents and
order, radio activity, and radio refill behavior are unchanged.

The `Stop after …` indicator is in the player bar between the timeline and
volume; click it to cancel immediately. Stop-after conditions are process-local
and are not restored after restart.

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
- `Radio > Anchor for new sessions` — choose `Stay near the starting song` or
  `Drift with what plays` for new seeded and artist sessions.
- `Radio > Customization…` / `Radio > Genre curation…` — engine tuning
  ([radio.md](radio.md)).
- `Settings > Track information panel…` — fields shown in the right-side
  info pane. `Settings > Album art resolution…` — cached artwork size.
Both scrobbling entries open one window with two tabs, `Scrobblers` and
`Listening history`; the entry you use decides which tab it opens on.

- `History > Scrobblers ▸ > Manage scrobblers…` — every scrobbling
  destination in one list: Last.fm, ListenBrainz, and any number of
  ListenBrainz-compatible servers such as Koito. Each row carries its own
  switch, name, address, token, status, pending count, and its `Test` and
  remove buttons. The switch enables a destination in one click, and the
  fields are edited where they are shown. ListenBrainz's token can be
  replaced here while its built-in identity and URL stay fixed; a server
  with no address cannot be enabled. Changes are saved as you make them and
  applied when the window closes, so there is no OK or Cancel; removing a
  destination confirms first and cannot be undone. Changing an address is the
  one edit applied at once, since until it is, listens would still be sent to
  the address that destination has stopped using. The window also carries
  offline mode and `Last.fm API settings…`, which accepts your own Last.fm
  credentials. The Scrobblers menu holds the same two, plus history
  backfill.
- `History > Listening history…` — pick one or more destinations to see and
  manage their delivery state, or pick none for a read-only overview. The
  destination popup stays open while you toggle entries, so several can be
  picked in one visit; the `Scrobbled` column, the summary, and the queue,
  retry and clear actions all apply to exactly what is picked. The choice
  lasts as long as the window.

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
FLAC, MP2/MP3, M4A/M4B (MP4 audio), Matroska Audio, Musepack, Ogg
(`.oga`, `.ogg`, `.ogx`, `.opus`), True Audio, WAV, Windows Media Audio, and
WavPack files. Bare `.mp4` is not scanned, since a video file carrying an audio
track is indistinguishable from an audio one by extension. Playback still
requires a matching GStreamer decoder plugin.
Unsupported files are hidden by default — enable `View > List unsupported
files in explorer` while in an explorer view to show them.
