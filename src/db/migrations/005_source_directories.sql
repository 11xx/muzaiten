ALTER TABLE scan_roots ADD COLUMN name TEXT;
ALTER TABLE scan_roots ADD COLUMN scan_enabled INTEGER NOT NULL DEFAULT 1;
ALTER TABLE scan_roots ADD COLUMN library_enabled INTEGER NOT NULL DEFAULT 1;
ALTER TABLE scan_roots ADD COLUMN updated_at TEXT;
ALTER TABLE scan_roots ADD COLUMN last_error TEXT;

CREATE INDEX IF NOT EXISTS idx_scan_roots_scan_enabled ON scan_roots(scan_enabled);
CREATE INDEX IF NOT EXISTS idx_scan_roots_library_enabled ON scan_roots(library_enabled);
CREATE INDEX IF NOT EXISTS idx_tracks_path ON tracks(path);
CREATE INDEX IF NOT EXISTS idx_tracks_parent_dir ON tracks(parent_dir);

INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(5, datetime('now'));
