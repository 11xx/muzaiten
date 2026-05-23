CREATE TABLE IF NOT EXISTS schema_migrations (
  version INTEGER PRIMARY KEY,
  applied_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS scan_roots (
  id INTEGER PRIMARY KEY,
  path TEXT NOT NULL UNIQUE,
  created_at TEXT NOT NULL,
  last_scanned_at TEXT
);

CREATE TABLE IF NOT EXISTS artists (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  sort_name TEXT,
  musicbrainz_artist_id TEXT,
  UNIQUE(name, musicbrainz_artist_id)
);

CREATE TABLE IF NOT EXISTS albums (
  id INTEGER PRIMARY KEY,
  title TEXT NOT NULL,
  album_artist_id INTEGER,
  sort_title TEXT,
  date TEXT,
  original_date TEXT,
  musicbrainz_release_id TEXT,
  musicbrainz_release_group_id TEXT,
  artwork_cache_key TEXT,
  FOREIGN KEY(album_artist_id) REFERENCES artists(id)
);

CREATE TABLE IF NOT EXISTS tracks (
  id INTEGER PRIMARY KEY,
  path TEXT NOT NULL UNIQUE,
  parent_dir TEXT NOT NULL,
  filename TEXT NOT NULL,
  title TEXT,
  artist_name TEXT,
  album_artist_name TEXT,
  album_title TEXT,
  album_id INTEGER,
  track_number INTEGER,
  track_total INTEGER,
  disc_number INTEGER,
  disc_total INTEGER,
  duration_ms INTEGER,
  rating_0_100 INTEGER,
  rating_source TEXT NOT NULL DEFAULT 'none',
  play_count INTEGER,
  date TEXT,
  original_date TEXT,
  musicbrainz_recording_id TEXT,
  musicbrainz_track_id TEXT,
  musicbrainz_release_id TEXT,
  file_size INTEGER NOT NULL,
  file_mtime INTEGER NOT NULL,
  scanned_at TEXT NOT NULL,
  scan_error TEXT,
  FOREIGN KEY(album_id) REFERENCES albums(id)
);

CREATE TABLE IF NOT EXISTS artwork (
  id INTEGER PRIMARY KEY,
  album_id INTEGER,
  source_type TEXT NOT NULL,
  source_path TEXT,
  cache_path TEXT,
  width INTEGER,
  height INTEGER,
  content_hash TEXT,
  updated_at TEXT NOT NULL,
  FOREIGN KEY(album_id) REFERENCES albums(id)
);

CREATE INDEX IF NOT EXISTS idx_artists_name ON artists(name);
CREATE INDEX IF NOT EXISTS idx_albums_album_artist ON albums(album_artist_id);
CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album_id);
CREATE INDEX IF NOT EXISTS idx_tracks_album_artist ON tracks(album_artist_name);
CREATE INDEX IF NOT EXISTS idx_tracks_rating ON tracks(rating_0_100);

INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(1, datetime('now'));
