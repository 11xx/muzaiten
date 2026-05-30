#pragma once

#include <QString>

// Resolves the application's on-disk locations following the XDG Base Directory
// spec, with explicit, fully customizable overrides. Each accessor ensures the
// directory exists and returns it (already suffixed with the app subdirectory).
//
// Per-category precedence (highest first):
//   1. CLI flag:  --data-dir / --state-dir / --cache-dir / --config-dir
//                 (stored as qApp properties muzaiten.{data,state,cache,config}Dir)
//   2. env var:   MUZAITEN_DATA_DIR / MUZAITEN_STATE_DIR / MUZAITEN_CACHE_DIR / MUZAITEN_CONFIG_DIR
//   3. combined root: --state-root flag (qApp property muzaiten.stateRoot) or
//                 MUZAITEN_STATE_ROOT env -> <root>/{data,state,cache,config}
//   4. dev shortcut: --dev-state flag (qApp property muzaiten.devState) or
//                 MUZAITEN_DEV_STATE env -> combined root = <cwd>/dev-state
//   5. default:   $XDG_<KIND>_HOME (or the spec fallback ~/.local/share, ~/.local/state,
//                 ~/.cache, ~/.config) + /muzaiten
namespace AppPaths {

QString dataDir();   // XDG_DATA_HOME  - library.sqlite
QString stateDir();  // XDG_STATE_HOME - state.sqlite, pending-scrobble queues
QString cacheDir();  // XDG_CACHE_HOME - artwork.sqlite, mpd-artwork/
QString configDir(); // XDG_CONFIG_HOME - holds the config file

// Path to the INI config file (configDir()/muzaiten.conf).
QString configFilePath();

// Writes a fully-commented template config file if none exists yet (no-op
// otherwise). Reading uses the [paths] section as a precedence layer below
// CLI flags / env / --state-root and above the XDG default.
void writeDefaultConfigIfMissing();

} // namespace AppPaths
