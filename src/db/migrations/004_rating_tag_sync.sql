CREATE TABLE IF NOT EXISTS pending_track_rating_writes (
  track_path TEXT PRIMARY KEY,
  rating_0_100 INTEGER NOT NULL,
  status TEXT NOT NULL,
  last_error TEXT,
  updated_at TEXT NOT NULL
);
