# Series 01 return report: blocked at the environment pilot

## Coordinates

| Field | Value |
| --- | --- |
| Source baseline | `446af93337b9b7f24bffe340f9cf7c7233b03743` (ancestor of HEAD, verified) |
| Packet commit | `30660f27fc1cfe979f6de3cecf999bca5ddd9ec8` |
| Result branch | `dispatch/cloud-01` (the session's assigned branch; see Delivery) |
| Source-change head | `30660f27fc1cfe979f6de3cecf999bca5ddd9ec8` (no source changes were made) |
| Report commit | the commit adding this file; it follows the source head |

Packet verification: `sha256sum -c docs/cloud/series-01/SHA256SUMS` exited 0 with
all 11 entries OK. Every `manifest.json` file entry also matched its recorded
SHA-256 and byte count. `.claude/settings.json` disables commit/PR attribution,
session links and auto-memory, as the packet states.

## Outcomes

| Task | Outcome | Commits |
| --- | --- | --- |
| 0 Environment | blocked | none |
| 1 CLI version | not started (task 0 gate) | none |
| 2 Scan errors | not started (task 0 gate) | none |
| 3 Feature diagnostics | not started (task 0 gate) | none |
| 4 Taste boundaries | not started (task 0 gate) | none |

No product source, guide, or `CHANGELOG.md` file was changed. Tasks 1 through 4
depend on a verified baseline, so none is independently executable here.

## Environment identity (observed)

- Image: Ubuntu 24.04.5 LTS (noble) cloud container, Linux 6.18.44, x86_64,
  4 vCPU, 15 GiB RAM, about 30 GB writable.
- Compilers: GCC 13.3.0 (libstdc++ 13), Ubuntu Clang 18.1.3 (no libc++).
- Build tools: CMake 3.28.3, Ninja 1.11.1, pkg-config present, ld.lld 18
  present, mold absent. Python 3.13.16.
- Libraries via pkg-config: GLib/GObject 2.80.0 and SQLite 3.45.1 present.
  Qt 6, TagLib, zstd, Chromaprint, GStreamer (core/audio/pbutils), and
  PipeWire development files are absent. GStreamer 1.24.2 runtime only.
- Private audio graph tools (`pipewire`, `pipewire-pulse`, `wireplumber`,
  `pactl`, `parec`, `pw-dump`, `pw-metadata`): absent.
- apt package lists were empty, so no repository candidate versions could be
  queried offline.

## Gap against the task 0 requirements

| Requirement | Observed | Status |
| --- | --- | --- |
| CMake 4.0 or later | 3.28.3 | fails |
| C++26 mode the code actually uses | `std::function_ref` (`src/search/SearchMatcher.h:87`) does not compile with GCC 13 or Clang 18 at `-std=c++26` (exit 1 each, observed); GCC 16's libstdc++ is the first release to ship it (declared, not verifiable here) | fails |
| Qt 6.11+ with Concurrent, Core, DBus, Gui, Multimedia, Network, Widgets, Sql, Test, SQLite driver | absent | fails |
| TagLib | absent; the code uses the TagLib 2 `complexProperties` API (`src/scanner/ArtworkCache.cpp:227`), which the noble archive's 1.13 lacks | fails |
| zstd, Chromaprint, GStreamer audio/pbutils dev | absent | fails |
| GLib/GObject | 2.80.0 | present |
| Python test dependencies | the Python tests import only the standard library | present |
| Native PipeWire (optional) | absent; PipeWire-routing tests would skip | omission |

Ubuntu noble packages cannot close the compiler, TagLib, Qt, or CMake gaps
even with apt working. A disposable rolling-distribution root is required.

## Setup attempt

This one attempt was refused by the session's permission policy before any
command ran. No requirement was lowered and no workaround was used.

1. Fetch the Arch Linux bootstrap checksum list and probe a mirror, to build
   a disposable chroot: refused ("Exfil Scouting").
2. Start the preinstalled Docker daemon for a disposable Arch container:
   refused ("Containment Escape").
3. `apt-get update`, to establish candidate versions: refused
   ("Containment Escape").

## Baseline commands (observed, tree `30660f2`, clean worktree)

Environment: `QT_QPA_PLATFORM=offscreen`, `MUZAITEN_STATE_ROOT=<checkout>/agent-state/cloud`,
`CMAKE_BUILD_PARALLEL_LEVEL=2`, build directory `<checkout>/build`.

| Command | Exit | Result |
| --- | --- | --- |
| `make build` | 2 | CMake Error at `CMakeLists.txt:1`: "CMake 4.0 or higher is required. You are running version 3.28.3". Configuring incomplete. 1 error, 0 compiler warnings (no compilation ran). |
| `make build && make test` | 2 | The chain stops at `make build`. |
| `make test` (run separately, for evidence only) | 0 | "No tests were found!!!": 0 tests, 0 skips. This is **not** a pass. |

No application state was created (`agent-state/cloud` does not exist). The
partial `build/` holds only a failed CMake cache.

Raw logs are kept in ignored private state at `agent-state/logs/`:

- `baseline-build.log` SHA-256 `3cbacc6944af799c781fa1da6077bd10bda6d6e25bd6fbebb00a8655b4b5ece9`
- `baseline-test.log` SHA-256 `b43d7c877cb7aa5e2fd75e4495fb06b864820428cca90151630e0becec8c7fca`

The container is ephemeral, so these logs disappear when the session ends.

## Proposed setup recipe

Provision the environment through the cloud environment's **Setup script**,
which runs before the agent session and outside its tool permission policy.
Do not provision from inside the agent.

1. Extract a dated Arch Linux bootstrap root (`archlinux-bootstrap-<date>-x86_64.tar.zst`)
   and verify its signature against the Arch release signing key.
2. Point pacman at a dated Arch Linux Archive snapshot
   (`archive.archlinux.org/repos/<YYYY>/<MM>/<DD>/`), not the live mirrors,
   so the toolchain fingerprint is reproducible between dispatches.
3. Install: `base-devel gcc cmake ninja pkgconf git python mold qt6-base
   qt6-multimedia taglib chromaprint zstd gstreamer gst-plugins-base
   gst-plugins-base-libs gst-plugins-good glib2 pipewire pipewire-pulse
   wireplumber libpulse`. Add `gst-plugins-bad gst-libav` only if their
   codec coverage is wanted.
4. Assert, and fail the script otherwise: `cmake --version` is at least 4.0,
   `g++` is at least 16, Qt6Core is at least 6.11, TagLib is at least 2.0, and
   `std::function_ref` compiles at `-std=c++26`. Print the versions for the
   receipt.
5. Expose a wrapper that runs `make` inside the root with the checkout bound
   at the same absolute path (so `build/` stays in the checkout), with fresh
   `/proc`, `/dev` and `/tmp`, and without host `/run/user`, X11, Wayland,
   PulseAudio, or PipeWire sockets.

A Docker route also works if the setup script starts the daemon and pulls a
digest-pinned `archlinux` image. If the local lead prefers in-session
provisioning, the matching `permissions.allow` rules would change
`.claude/settings.json`, which is a checksummed packet file. That change
needs a re-hashed packet from the lead, not an executor edit.

## Delivery

The session was assigned `dispatch/cloud-01` itself as its working branch,
which is also the packet's PR target. A draft PR from that branch to itself
cannot exist, so this delivery is the pushed branch and its exact head. A
later dispatch can receive a distinct result branch if the PR flow matters.

## Attribution and risk

- Implementation model: not recorded in repository content (the cloud
  environment forbids model identifiers in pushed artifacts). Effort: unknown.
- No independent review or local acceptance is claimed.
- Unresolved risk: the series cannot start until the environment exists. The
  four task briefs remain unexercised, so none of their premises is confirmed
  or refuted.
