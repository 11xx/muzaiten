use std::path::Path;
use std::process::Command;
use std::time::UNIX_EPOCH;

use anyhow::{Context, Result};
use muzaiten_index::{ScanOptions, Stage, scan, status_json};
use rusqlite::{Connection, params};
use tempfile::tempdir;

#[test]
fn generated_fixture_matrix_groups_identity_and_chromaprint_tiers() -> Result<()> {
    require_tool("ffmpeg")?;
    require_tool("fpcalc")?;

    let dir = tempdir()?;
    let audio_dir = dir.path().join("audio");
    std::fs::create_dir_all(&audio_dir)?;

    let sine_flac = audio_dir.join("sine.flac");
    let sine_wav = audio_dir.join("sine.wav");
    let sine_mp3 = audio_dir.join("sine.mp3");
    let padded = audio_dir.join("padded.flac");
    let sweep = audio_dir.join("sweep.flac");
    let chord = audio_dir.join("chord.flac");

    ffmpeg(&[
        "-f",
        "lavfi",
        "-i",
        "sine=frequency=440:duration=5",
        path_str(&sine_flac)?,
    ])?;
    ffmpeg(&["-i", path_str(&sine_flac)?, path_str(&sine_wav)?])?;
    ffmpeg(&[
        "-i",
        path_str(&sine_flac)?,
        "-b:a",
        "128k",
        path_str(&sine_mp3)?,
    ])?;
    ffmpeg(&[
        "-f",
        "lavfi",
        "-i",
        "sine=frequency=440:duration=8",
        path_str(&padded)?,
    ])?;
    ffmpeg(&[
        "-f",
        "lavfi",
        "-i",
        "aevalsrc=sin(2*PI*(220+120*t)*t):duration=5:sample_rate=44100",
        path_str(&sweep)?,
    ])?;
    ffmpeg(&[
        "-f",
        "lavfi",
        "-i",
        "aevalsrc=(sin(2*PI*330*t)+0.6*sin(2*PI*495*t)+0.4*sin(2*PI*660*t))/2:duration=5:sample_rate=44100",
        path_str(&chord)?,
    ])?;

    let library = dir.path().join("library.sqlite");
    let features = dir.path().join("features.sqlite");
    create_library(
        &library,
        &[&sine_flac, &sine_wav, &sine_mp3, &padded, &sweep, &chord],
    )?;

    let summary = scan(ScanOptions {
        library: library.clone(),
        features: features.clone(),
        stage: Stage::All,
        limit: None,
        jobs: Some(2),
    })?;
    assert_eq!(summary.scanned, 6);
    assert_eq!(summary.skipped, 0);
    assert_eq!(summary.failed, 0);
    assert!(summary.groups >= 4);

    let conn = Connection::open(&features)?;
    let sine_group = group_for(&conn, &sine_flac)?;
    assert_eq!(sine_group, group_for(&conn, &sine_wav)?);
    assert_eq!(sine_group, group_for(&conn, &sine_mp3)?);
    assert_ne!(sine_group, group_for(&conn, &padded)?);
    assert_ne!(sine_group, group_for(&conn, &sweep)?);
    assert_ne!(sine_group, group_for(&conn, &chord)?);
    assert_ne!(group_for(&conn, &sweep)?, group_for(&conn, &chord)?);
    assert_eq!(
        decode_hash_for(&conn, &sine_flac)?,
        decode_hash_for(&conn, &sine_wav)?
    );

    let feature_rows: i64 =
        conn.query_row("SELECT COUNT(*) FROM features", [], |row| row.get(0))?;
    assert_eq!(feature_rows, 0);

    let status = status_json(&features)?;
    assert_eq!(status["statuses"]["ok"], 6);
    assert_eq!(status["features"], 0);

    let rerun = scan(ScanOptions {
        library: library.clone(),
        features: features.clone(),
        stage: Stage::All,
        limit: None,
        jobs: Some(2),
    })?;
    assert_eq!(rerun.scanned, 0);
    assert_eq!(rerun.skipped, 6);

    bump_library_mtime(&library, &sine_wav)?;
    let partial = scan(ScanOptions {
        library,
        features,
        stage: Stage::All,
        limit: None,
        jobs: Some(2),
    })?;
    assert_eq!(partial.scanned, 1);
    assert_eq!(partial.skipped, 5);
    Ok(())
}

fn require_tool(name: &str) -> Result<()> {
    let status = Command::new(name).arg("-version").status();
    match status {
        Ok(status) if status.success() => Ok(()),
        Ok(_) => anyhow::bail!("{name} -version failed"),
        Err(error) => anyhow::bail!("{name} unavailable: {error}"),
    }
}

fn ffmpeg(args: &[&str]) -> Result<()> {
    let status = Command::new("ffmpeg")
        .arg("-hide_banner")
        .arg("-loglevel")
        .arg("error")
        .arg("-y")
        .args(args)
        .status()
        .context("running ffmpeg")?;
    if !status.success() {
        anyhow::bail!("ffmpeg fixture generation failed");
    }
    Ok(())
}

fn create_library(path: &Path, files: &[&Path]) -> Result<()> {
    let conn = Connection::open(path)?;
    conn.execute_batch(
        "
        CREATE TABLE tracks(
            path TEXT PRIMARY KEY,
            file_mtime INTEGER NOT NULL,
            file_size INTEGER NOT NULL,
            missing INTEGER NOT NULL DEFAULT 0
        );
        ",
    )?;
    for file in files {
        let (mtime, size) = file_stats(file)?;
        conn.execute(
            "INSERT INTO tracks(path, file_mtime, file_size, missing) VALUES(?, ?, ?, 0)",
            params![file.to_string_lossy().as_ref(), mtime, size],
        )?;
    }
    Ok(())
}

fn file_stats(path: &Path) -> Result<(i64, i64)> {
    let metadata = std::fs::metadata(path)?;
    let mtime = metadata
        .modified()?
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs() as i64;
    Ok((mtime, metadata.len() as i64))
}

fn bump_library_mtime(library: &Path, file: &Path) -> Result<()> {
    let conn = Connection::open(library)?;
    conn.execute(
        "UPDATE tracks SET file_mtime = file_mtime + 1 WHERE path = ?",
        params![file.to_string_lossy().as_ref()],
    )?;
    Ok(())
}

fn group_for(conn: &Connection, path: &Path) -> Result<i64> {
    conn.query_row(
        "SELECT content_group_id FROM files WHERE path = ?",
        params![path.to_string_lossy().as_ref()],
        |row| row.get(0),
    )
    .map_err(Into::into)
}

fn decode_hash_for(conn: &Connection, path: &Path) -> Result<String> {
    conn.query_row(
        "SELECT decode_hash FROM files WHERE path = ?",
        params![path.to_string_lossy().as_ref()],
        |row| row.get(0),
    )
    .map_err(Into::into)
}

fn path_str(path: &Path) -> Result<&str> {
    path.to_str()
        .ok_or_else(|| anyhow::anyhow!("non-UTF8 test path {}", path.display()))
}
