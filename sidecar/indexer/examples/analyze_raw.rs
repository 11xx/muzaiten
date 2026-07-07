//! Debug/validation entry point: read canonical PCM (f32le mono 22 050 Hz)
//! from stdin, print the extracted scalar features as JSON. Pairs with
//! `ffmpeg -i x -f f32le -ac 1 -ar 22050 -` for oracle cross-checks
//! (e.g. against `ffmpeg -af ebur128` loudness).

use std::io::Read;

fn main() {
    let mut raw = Vec::new();
    std::io::stdin()
        .read_to_end(&mut raw)
        .expect("read raw PCM from stdin");
    let samples: Vec<f32> = raw
        .chunks_exact(4)
        .map(|b| f32::from_le_bytes([b[0], b[1], b[2], b[3]]))
        .collect();

    match muzaiten_index::dsp::analyze(&samples, muzaiten_index::dsp::SAMPLE_RATE_HZ) {
        Some(features) => println!("{features:#?}"),
        None => println!("no features (input too short)"),
    }
}
