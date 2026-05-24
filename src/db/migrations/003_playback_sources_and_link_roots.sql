CREATE TABLE IF NOT EXISTS media_sources (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  kind TEXT NOT NULL,
  name TEXT NOT NULL,
  root_hint TEXT,
  config_path TEXT,
  enabled INTEGER NOT NULL DEFAULT 1,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS link_roots (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  source_prefix TEXT NOT NULL,
  target_prefix TEXT NOT NULL,
  priority INTEGER NOT NULL,
  readable INTEGER NOT NULL DEFAULT 1,
  writable INTEGER NOT NULL DEFAULT 0,
  enabled INTEGER NOT NULL DEFAULT 1,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS mpd_tracks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  source_id INTEGER NOT NULL,
  uri TEXT NOT NULL,
  title TEXT,
  artist_name TEXT,
  album_artist_name TEXT,
  album_title TEXT,
  track_number INTEGER,
  disc_number INTEGER,
  duration_ms INTEGER,
  date TEXT,
  musicbrainz_artist_id TEXT,
  musicbrainz_album_artist_id TEXT,
  musicbrainz_album_id TEXT,
  musicbrainz_recording_id TEXT,
  musicbrainz_release_track_id TEXT,
  last_seen_at TEXT NOT NULL,
  UNIQUE(source_id, uri)
);

CREATE TABLE IF NOT EXISTS playlists (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  source_kind TEXT NOT NULL,
  source_id INTEGER,
  name TEXT NOT NULL,
  external_path TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS playlist_tracks (
  playlist_id INTEGER NOT NULL,
  position INTEGER NOT NULL,
  source_kind TEXT NOT NULL,
  source_uri TEXT NOT NULL,
  track_path TEXT,
  title_snapshot TEXT,
  artist_snapshot TEXT,
  album_snapshot TEXT,
  duration_ms INTEGER,
  PRIMARY KEY(playlist_id, position)
);

INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(3, datetime('now'));
