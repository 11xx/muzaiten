CREATE TABLE IF NOT EXISTS user_track_ratings (
  track_path TEXT PRIMARY KEY,
  rating_0_100 INTEGER,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS user_album_ratings (
  album_artist_name TEXT NOT NULL,
  album_title TEXT NOT NULL,
  rating_0_100 INTEGER,
  updated_at TEXT NOT NULL,
  PRIMARY KEY(album_artist_name, album_title)
);

CREATE TABLE IF NOT EXISTS app_settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(2, datetime('now'));
