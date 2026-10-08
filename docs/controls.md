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

The player bar carries the current track's album art on every view except the
library ones, where the right sidebar shows it at full width. Right-click the
art for **Find in library** and **Open containing directory**.

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
  `Semantic provider setup…` and `Download semantic model…` show a cancellable
  waiting dialog while checking provider status, with a 45-second timeout.
  Playback and the main window remain responsive during the check. Starting
  another setup or download request, changing a provider or analysis-store
  path, or closing the main window cancels pending status and consent.
  A model download starts only after confirming its source, license, size,
  cache location, and checksum; providers that report an audio-only size
  also offer `Audio only` beside `Full model`.
- `Playback > Output profile…` — transport and output, direct device,
  software volume, resampling, sink release on pause, read-ahead. Native
  bit-perfect DSD
  (`.dsf`) needs a direct-capable device selected here with resampling
  off; with **Allow resampling** DSD decodes to PCM for shared output.
  A saved DSD track that needs device takeover waits for the first window and
  prompts there. A newly started DSD track that needs takeover is skipped
  after the window has been closed to the tray; open it to approve a new start.
  If an output choice cannot be saved, the status bar reports the storage
  error. The choice remains active until the app exits; the next launch uses
  the last saved profile.
  See [Audio output](#audio-output) for how the destination is chosen.
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
  with no address cannot be enabled. Changes are saved and take effect as you
  make them, so there is no OK or Cancel; a field commits when you leave it,
  not as you type. `Test` reports one of three things: the account the token
  connects as, a token the server refused, or no answer at all, which points
  at the server or the network rather than the token (hover the status for the
  detail). A destination enabled in the middle of a track announces
  that track as playing now right away; the listen itself is delivered at the
  end only if the destination was enabled before the track counted as
  listened. Removing a destination confirms first and cannot be undone. The window also carries
  offline mode and `Last.fm API settings…`, which accepts your own Last.fm
  credentials. The Scrobblers menu holds the same two, plus history
  backfill.
- `History > Listening history…` — pick one or more destinations to see and
  manage their delivery state, or pick none for a read-only overview. The
  destination popup stays open while you toggle entries, so several can be
  picked in one visit; the `Scrobbled` column, the summary, and the queue,
  retry and clear actions all apply to exactly what is picked. The choice
  lasts as long as the window.

## Audio output

**Shared** output plays through the system sound server (PipeWire, or
PulseAudio). The session manager chooses the destination of every stream
muzaiten opens: it follows the system default output and remembers where
muzaiten was last moved in a mixer such as pavucontrol or Wiremix. A move
made while a track plays holds for the tracks after it, including across
pauses, sample-rate changes between tracks and error recovery.

When an output disappears, the sound server moves playback to another one.
muzaiten never treats that fallback as a choice: the next stream it opens
asks for no particular output, so the session manager can put playback back
once the original output returns.

In `Playback > Output profile…`, the **Shared** side has two separate
choices. **Transport** is how muzaiten talks to the sound server: Auto uses
GStreamer's PipeWire plugin when it is installed (and the build can follow
PipeWire streams) and PulseAudio otherwise;
the ALSA transport cannot choose an output. **Output** is where shared audio
goes: `System managed` (the behavior above), or one listed playback output,
virtual outputs such as an equalizer included. A chosen output is kept by its
stable name, so it survives being unplugged, switched off or renumbered, and
other devices appearing or becoming the default do not move playback off it.
It affects muzaiten only; system defaults are never changed. The saved
output applies from startup, including when muzaiten starts in the tray and
plays before its window opens.

**Release output device on pause** closes the playback stream while paused.
Resuming restores the held position before audio becomes audible. With the
native PipeWire transport, reconnecting stays silent while the stream becomes
ready for that seek; pausing or seeking during reconnect keeps the latest
requested state and position, and the position display follows an explicit
seek immediately. Direct ALSA and native DSD pauses release their
exclusive device regardless of the shared-output preference.

If a held position cannot be restored, playback stops with an error instead of
starting from the beginning silently. That happens when the stream is not ready
for its seek within 15 seconds of resuming, when the source refuses the seek,
or when the pipeline fails during the restore. The output stays silent, and
Play starts the current track from its beginning.

While a chosen output is missing, playback pauses at its position and resumes
there when the output returns, if it was playing; pausing, seeking or picking
another track meanwhile is respected. With **Play through the system output
while it is missing** checked, playback continues on the system-managed output
instead and moves back when the chosen one returns. When the output vanishes
mid-track, the sound server can play a moment of audio elsewhere before
playback pauses. Choosing another output meanwhile continues the held track
there, at its position, playing or paused as it was. A missing output stays
selected in the dialog, marked `(unavailable)`; a fallback never replaces it.

Moving muzaiten's stream to another output in a mixer takes effect at once
for every stream that follows, including the next track and a sample-rate
change, and a second later becomes the new choice, saved as if picked in the
dialog, also while muzaiten plays from the tray without its window. A move the sound server made because the output the stream was on
disappeared is a fallback and never becomes the choice. Both transports
see where the stream is linked: PulseAudio reports it, and with the PipeWire
transport muzaiten follows its own stream's links in the PipeWire graph. A
build without PipeWire support cannot do the latter, so there Auto uses the
PulseAudio transport, and an explicit PipeWire choice says in the status line
that mixer moves are not followed.

**Bit-perfect** output opens one ALSA card directly. A card picked from the
list is remembered by its stable identity rather than its number (`hw:N`),
which changes across reboots and replugs and can pass to another card. Before
every open, retry, takeover or release, muzaiten looks the card up by that
identity. While it is absent, playback waits at its position, as for a missing
shared output, and resumes when the card returns under whatever number it now
has; muzaiten never opens, takes over or releases the card that took its old
number. The dialog lists an absent card as `(unavailable)` and keeps it
selected. A device typed by hand (such as `hw:CARD=Name,DEV=0`), or saved
before identities were recorded, is opened exactly as written. Native DSD
playback uses the same card under the same rule.

The line under both sides reports the transport in use (and why Auto chose
it), where playback is going, and whether it is waiting or falling back. A
waiting or falling-back output also shows in the status bar, and
`muzaitenctl status` reports it (see [cli.md](cli.md)).

## File explorers

Panel search (`/`) matches the displayed name/title, artist and album fields.
It does not match hidden parent paths or a tagged track's on-disk filename.
Untagged tracks show their complete filename, including extension; absent artist
and album values appear as muted em dashes. These are display fallbacks and do
not change the tags or stored metadata.

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

A file with a recognized extension that cannot be read as audio (the tag reader
cannot open it, or it reports no sample rate and no channels) still becomes a
library row, and `Track properties` shows the reason under **Scan**. It exists
on disk, so it is not hidden. It is left out of radio candidates and of the
audio analysis pass, which would otherwise offer it up to be played or spend a
decode attempt on it. The reason is written when the file is scanned, so a
library built before this existed shows it after `Library > Force full rescan`.
Unsupported files are hidden by default — enable `View > List unsupported
files in explorer` while in an explorer view to show them.
