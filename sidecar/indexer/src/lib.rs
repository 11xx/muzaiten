use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result, bail};
use rayon::ThreadPoolBuilder;
use rayon::prelude::*;
use rusqlite::{Connection, OpenFlags, OptionalExtension, params};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};

const SCHEMA_VERSION: &str = "1";
const SAMPLE_RATE_HZ: f64 = 22_050.0;
const CHROMAPRINT_BER_THRESHOLD: f64 = 0.15;
const CHROMAPRINT_OFFSET_FRAMES: i32 = 3;
const DURATION_BUCKET_MS: i64 = 2_000;
const BLISS_STATUS: &str = "blocked_license_gpl_3_only";

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Stage {
    Identity,
    Features,
    All,
}

#[derive(Debug)]
pub struct ScanOptions {
    pub library: PathBuf,
    pub features: PathBuf,
    pub stage: Stage,
    pub limit: Option<usize>,
    pub jobs: Option<usize>,
}

#[derive(Debug, Serialize)]
pub struct ScanSummary {
    pub scanned: usize,
    pub skipped: usize,
    pub failed: usize,
    pub groups: usize,
    pub bliss_status: &'static str,
}

#[derive(Clone, Debug)]
struct Candidate {
    path: PathBuf,
    mtime: i64,
    size: i64,
}

#[derive(Debug)]
struct FileAnalysis {
    candidate: Candidate,
    duration_ms: Option<i64>,
    decode_hash: Option<String>,
    chromaprint_fp: Option<Vec<u8>>,
    status: &'static str,
}

#[derive(Debug)]
struct GroupRow {
    path: String,
    duration_ms: i64,
    decode_hash: String,
    chromaprint: Vec<i32>,
}

#[derive(Debug, Deserialize)]
struct FpcalcOutput {
    fingerprint: Vec<i32>,
}

pub fn scan(options: ScanOptions) -> Result<ScanSummary> {
    let mut features = open_features(&options.features)?;
    init_schema(&features)?;

    if options.stage == Stage::Features {
        let groups = regroup_content(&mut features)?;
        return Ok(ScanSummary {
            scanned: 0,
            skipped: 0,
            failed: 0,
            groups,
            bliss_status: BLISS_STATUS,
        });
    }

    let candidates = load_candidates(&options.library, options.limit)?;
    let (pending, skipped) = split_pending(&features, candidates)?;
    let analyses = analyze_pending(&pending, options.jobs)?;

    let mut failed = 0;
    for analysis in &analyses {
        if analysis.status != "ok" {
            failed += 1;
        }
        upsert_file(&features, analysis)?;
    }

    let groups = regroup_content(&mut features)?;
    Ok(ScanSummary {
        scanned: analyses.len(),
        skipped,
        failed,
        groups,
        bliss_status: BLISS_STATUS,
    })
}

pub fn status_json(features_path: &Path) -> Result<Value> {
    let features = open_features(features_path)?;
    init_schema(&features)?;

    let mut status_counts = serde_json::Map::new();
    let mut query =
        features.prepare("SELECT status, COUNT(*) FROM files GROUP BY status ORDER BY status")?;
    let rows = query.query_map([], |row| {
        Ok((row.get::<_, String>(0)?, row.get::<_, i64>(1)?))
    })?;
    let mut total = 0;
    for row in rows {
        let (status, count) = row?;
        total += count;
        status_counts.insert(status, json!(count));
    }
    let groups: i64 =
        features.query_row("SELECT COUNT(*) FROM content_groups", [], |row| row.get(0))?;
    let feature_rows: i64 =
        features.query_row("SELECT COUNT(*) FROM features", [], |row| row.get(0))?;
    Ok(json!({
        "schema_version": SCHEMA_VERSION,
        "files": total,
        "statuses": status_counts,
        "groups": groups,
        "features": feature_rows,
        "pending": 0,
        "bliss_status": BLISS_STATUS,
    }))
}

fn open_features(path: &Path) -> Result<Connection> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("creating {}", parent.display()))?;
    }
    let conn = Connection::open(path).with_context(|| format!("opening {}", path.display()))?;
    conn.pragma_update(None, "journal_mode", "WAL")?;
    conn.pragma_update(None, "synchronous", "NORMAL")?;
    conn.pragma_update(None, "busy_timeout", 5_000)?;
    Ok(conn)
}

fn init_schema(conn: &Connection) -> Result<()> {
    conn.execute_batch(
        "
        CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE IF NOT EXISTS files(
            path TEXT PRIMARY KEY,
            mtime INTEGER NOT NULL,
            size INTEGER NOT NULL,
            duration_ms INTEGER,
            decode_hash TEXT,
            chromaprint_fp BLOB,
            content_group_id INTEGER,
            analyzed_at INTEGER NOT NULL,
            status TEXT NOT NULL DEFAULT 'ok'
        );
        CREATE TABLE IF NOT EXISTS content_groups(id INTEGER PRIMARY KEY AUTOINCREMENT);
        CREATE TABLE IF NOT EXISTS features(
            content_group_id INTEGER PRIMARY KEY,
            bliss_vector BLOB NOT NULL,
            tempo_bpm REAL,
            loudness REAL,
            energy REAL,
            brightness REAL,
            extractor TEXT NOT NULL,
            version TEXT NOT NULL
        );
        ",
    )?;
    let created_at = now_secs();
    conn.execute(
        "INSERT OR IGNORE INTO meta(key, value) VALUES('created_at', ?)",
        params![created_at.to_string()],
    )?;
    conn.execute(
        "INSERT INTO meta(key, value) VALUES('schema_version', ?)
         ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        params![SCHEMA_VERSION],
    )?;
    conn.execute(
        "INSERT INTO meta(key, value) VALUES('indexer_version', ?)
         ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        params![env!("CARGO_PKG_VERSION")],
    )?;
    conn.execute(
        "INSERT INTO meta(key, value) VALUES('bliss_version', ?)
         ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        params![BLISS_STATUS],
    )?;
    Ok(())
}

fn load_candidates(library: &Path, limit: Option<usize>) -> Result<Vec<Candidate>> {
    let conn = Connection::open_with_flags(library, OpenFlags::SQLITE_OPEN_READ_ONLY)
        .with_context(|| format!("opening library {}", library.display()))?;
    let mut sql = String::from(
        "SELECT path, file_mtime, file_size FROM tracks
         WHERE COALESCE(missing, 0) = 0 AND path IS NOT NULL AND path <> ''
         ORDER BY path",
    );
    if let Some(limit) = limit {
        sql.push_str(&format!(" LIMIT {}", limit));
    }
    let mut query = conn.prepare(&sql)?;
    let rows = query.query_map([], |row| {
        Ok(Candidate {
            path: PathBuf::from(row.get::<_, String>(0)?),
            mtime: row.get(1)?,
            size: row.get(2)?,
        })
    })?;
    rows.collect::<rusqlite::Result<Vec<_>>>()
        .map_err(Into::into)
}

fn split_pending(conn: &Connection, candidates: Vec<Candidate>) -> Result<(Vec<Candidate>, usize)> {
    let mut pending = Vec::new();
    let mut skipped = 0;
    let mut query = conn.prepare("SELECT mtime, size FROM files WHERE path = ?")?;
    for candidate in candidates {
        let existing = query
            .query_row(params![candidate.path.to_string_lossy().as_ref()], |row| {
                Ok((row.get::<_, i64>(0)?, row.get::<_, i64>(1)?))
            })
            .optional()?;
        if existing == Some((candidate.mtime, candidate.size)) {
            skipped += 1;
        } else {
            pending.push(candidate);
        }
    }
    Ok((pending, skipped))
}

fn analyze_pending(pending: &[Candidate], jobs: Option<usize>) -> Result<Vec<FileAnalysis>> {
    if let Some(jobs) = jobs.filter(|jobs| *jobs > 0) {
        let pool = ThreadPoolBuilder::new().num_threads(jobs).build()?;
        Ok(pool.install(|| pending.par_iter().map(analyze_candidate).collect()))
    } else {
        Ok(pending.par_iter().map(analyze_candidate).collect())
    }
}

fn analyze_candidate(candidate: &Candidate) -> FileAnalysis {
    match decode_hash(candidate) {
        Ok((duration_ms, decode_hash)) => match fpcalc_fingerprint(&candidate.path) {
            Ok(fingerprint) => FileAnalysis {
                candidate: candidate.clone(),
                duration_ms: Some(duration_ms),
                decode_hash: Some(decode_hash),
                chromaprint_fp: Some(encode_fingerprint(&fingerprint)),
                status: "ok",
            },
            Err(_) => FileAnalysis {
                candidate: candidate.clone(),
                duration_ms: Some(duration_ms),
                decode_hash: Some(decode_hash),
                chromaprint_fp: None,
                status: "fp_failed",
            },
        },
        Err(_) => FileAnalysis {
            candidate: candidate.clone(),
            duration_ms: None,
            decode_hash: None,
            chromaprint_fp: None,
            status: "decode_failed",
        },
    }
}

fn decode_hash(candidate: &Candidate) -> Result<(i64, String)> {
    let output = Command::new("ffmpeg")
        .arg("-hide_banner")
        .arg("-loglevel")
        .arg("error")
        .arg("-i")
        .arg(&candidate.path)
        .arg("-vn")
        .arg("-f")
        .arg("f32le")
        .arg("-ac")
        .arg("1")
        .arg("-ar")
        .arg("22050")
        .arg("-")
        .output()
        .with_context(|| format!("spawning ffmpeg for {}", candidate.path.display()))?;
    if !output.status.success() {
        bail!("ffmpeg failed for {}", candidate.path.display());
    }
    if output.stdout.is_empty() || output.stdout.len() % 4 != 0 {
        bail!(
            "ffmpeg produced invalid f32le PCM for {}",
            candidate.path.display()
        );
    }
    let sample_count = output.stdout.len() / 4;
    let duration_ms = ((sample_count as f64 / SAMPLE_RATE_HZ) * 1000.0).round() as i64;
    let hash = Sha256::digest(&output.stdout);
    Ok((duration_ms, hex::encode(hash)))
}

fn fpcalc_fingerprint(path: &Path) -> Result<Vec<i32>> {
    let output = Command::new("fpcalc")
        .arg("-raw")
        .arg("-json")
        .arg("-length")
        .arg("120")
        .arg(path)
        .output()
        .with_context(|| format!("spawning fpcalc for {}", path.display()))?;
    if !output.status.success() {
        bail!("fpcalc failed for {}", path.display());
    }
    let parsed: FpcalcOutput = serde_json::from_slice(&output.stdout)?;
    if parsed.fingerprint.is_empty() {
        bail!(
            "fpcalc produced an empty fingerprint for {}",
            path.display()
        );
    }
    Ok(parsed.fingerprint)
}

fn encode_fingerprint(fingerprint: &[i32]) -> Vec<u8> {
    let mut blob = Vec::with_capacity(fingerprint.len() * 4);
    for value in fingerprint {
        blob.extend_from_slice(&value.to_le_bytes());
    }
    blob
}

fn decode_fingerprint(blob: &[u8]) -> Vec<i32> {
    blob.chunks_exact(4)
        .map(|chunk| i32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
        .collect()
}

fn upsert_file(conn: &Connection, analysis: &FileAnalysis) -> Result<()> {
    let path = analysis.candidate.path.to_string_lossy().into_owned();
    conn.execute(
        "
        INSERT INTO files(path, mtime, size, duration_ms, decode_hash, chromaprint_fp,
                          content_group_id, analyzed_at, status)
        VALUES(?, ?, ?, ?, ?, ?, NULL, ?, ?)
        ON CONFLICT(path) DO UPDATE SET
            mtime=excluded.mtime,
            size=excluded.size,
            duration_ms=excluded.duration_ms,
            decode_hash=excluded.decode_hash,
            chromaprint_fp=excluded.chromaprint_fp,
            content_group_id=NULL,
            analyzed_at=excluded.analyzed_at,
            status=excluded.status
        ",
        params![
            path,
            analysis.candidate.mtime,
            analysis.candidate.size,
            analysis.duration_ms,
            analysis.decode_hash.as_deref(),
            analysis.chromaprint_fp.as_deref(),
            now_secs(),
            analysis.status,
        ],
    )?;
    Ok(())
}

fn regroup_content(conn: &mut Connection) -> Result<usize> {
    let rows = load_group_rows(conn)?;
    let mut uf = UnionFind::new(rows.len());
    let mut by_hash: HashMap<&str, usize> = HashMap::new();
    for (idx, row) in rows.iter().enumerate() {
        if let Some(previous) = by_hash.insert(row.decode_hash.as_str(), idx) {
            uf.union(previous, idx);
        }
    }
    for left in 0..rows.len() {
        for right in (left + 1)..rows.len() {
            if (rows[left].duration_ms - rows[right].duration_ms).abs() > DURATION_BUCKET_MS {
                continue;
            }
            let ber = best_bit_error_rate(&rows[left].chromaprint, &rows[right].chromaprint);
            if ber < CHROMAPRINT_BER_THRESHOLD {
                uf.union(left, right);
            }
        }
    }

    let mut groups: BTreeMap<usize, Vec<usize>> = BTreeMap::new();
    for idx in 0..rows.len() {
        groups.entry(uf.find(idx)).or_default().push(idx);
    }

    let tx = conn.transaction()?;
    tx.execute("DELETE FROM features", [])?;
    tx.execute("DELETE FROM content_groups", [])?;
    tx.execute("UPDATE files SET content_group_id = NULL", [])?;
    for members in groups.values() {
        tx.execute("INSERT INTO content_groups DEFAULT VALUES", [])?;
        let group_id = tx.last_insert_rowid();
        for member in members {
            tx.execute(
                "UPDATE files SET content_group_id = ? WHERE path = ?",
                params![group_id, rows[*member].path],
            )?;
        }
    }
    tx.commit()?;
    Ok(groups.len())
}

fn load_group_rows(conn: &Connection) -> Result<Vec<GroupRow>> {
    let mut query = conn.prepare(
        "
        SELECT path, duration_ms, decode_hash, chromaprint_fp
        FROM files
        WHERE status = 'ok'
          AND duration_ms IS NOT NULL
          AND decode_hash IS NOT NULL
          AND chromaprint_fp IS NOT NULL
        ORDER BY path
        ",
    )?;
    let rows = query.query_map([], |row| {
        let blob: Vec<u8> = row.get(3)?;
        Ok(GroupRow {
            path: row.get(0)?,
            duration_ms: row.get(1)?,
            decode_hash: row.get(2)?,
            chromaprint: decode_fingerprint(&blob),
        })
    })?;
    rows.collect::<rusqlite::Result<Vec<_>>>()
        .map_err(Into::into)
}

fn best_bit_error_rate(left: &[i32], right: &[i32]) -> f64 {
    let mut best = f64::INFINITY;
    for offset in -CHROMAPRINT_OFFSET_FRAMES..=CHROMAPRINT_OFFSET_FRAMES {
        best = best.min(bit_error_rate(left, right, offset));
    }
    best
}

fn bit_error_rate(left: &[i32], right: &[i32], offset: i32) -> f64 {
    let (left_start, right_start) = if offset >= 0 {
        (offset as usize, 0)
    } else {
        (0, (-offset) as usize)
    };
    if left_start >= left.len() || right_start >= right.len() {
        return f64::INFINITY;
    }
    let overlap = (left.len() - left_start).min(right.len() - right_start);
    if overlap == 0 {
        return f64::INFINITY;
    }
    let errors: u32 = (0..overlap)
        .map(|idx| ((left[left_start + idx] ^ right[right_start + idx]) as u32).count_ones())
        .sum();
    errors as f64 / (overlap as f64 * 32.0)
}

fn now_secs() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs() as i64
}

#[derive(Debug)]
struct UnionFind {
    parent: Vec<usize>,
}

impl UnionFind {
    fn new(size: usize) -> Self {
        Self {
            parent: (0..size).collect(),
        }
    }

    fn find(&mut self, idx: usize) -> usize {
        let parent = self.parent[idx];
        if parent == idx {
            idx
        } else {
            let root = self.find(parent);
            self.parent[idx] = root;
            root
        }
    }

    fn union(&mut self, left: usize, right: usize) {
        let left_root = self.find(left);
        let right_root = self.find(right);
        if left_root != right_root {
            self.parent[right_root] = left_root;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::tempdir;

    #[test]
    fn schema_initializes_empty_database() -> Result<()> {
        let dir = tempdir()?;
        let db = dir.path().join("features.sqlite");
        let status = status_json(&db)?;
        assert_eq!(status["schema_version"], "1");
        assert_eq!(status["files"], 0);
        assert_eq!(status["groups"], 0);
        assert_eq!(status["features"], 0);
        assert_eq!(status["bliss_status"], BLISS_STATUS);
        Ok(())
    }

    #[test]
    fn bit_error_rate_uses_small_alignment_window() {
        let left = [0b1111_i32, 0b1010, 0b0101, 0b0000];
        let right = [0b0011_i32, 0b1111, 0b1010, 0b0101];
        assert_eq!(best_bit_error_rate(&left, &right), 0.0);
    }
}
