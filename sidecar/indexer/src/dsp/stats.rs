//! Textbook spectral statistics over the power spectrogram, plus the global
//! zero-crossing rate.

use super::stft::PowerSpectrogram;

const AMIN: f64 = 1e-10;
/// Frames whose total power is below this are treated as silence and skipped
/// so centroid/flatness describe the audible program, not the noise floor.
const SILENT_FRAME_POWER: f64 = 1e-9;

pub struct SpectralStats {
    pub centroid_mean_hz: f64,
    pub centroid_std_hz: f64,
    pub flatness_mean: f64,
}

pub fn spectral_stats(spectrogram: &PowerSpectrogram, sample_rate: f64) -> SpectralStats {
    let bins = spectrogram.bins();
    let bin_freqs: Vec<f64> = (0..bins)
        .map(|k| k as f64 * sample_rate / spectrogram.n_fft as f64)
        .collect();

    let mut centroids = Vec::new();
    let mut flatness_sum = 0.0;
    let mut flatness_frames = 0usize;

    for frame in &spectrogram.frames {
        let total: f64 = frame.iter().sum();
        if total < SILENT_FRAME_POWER {
            continue;
        }

        let weighted: f64 = frame.iter().zip(&bin_freqs).map(|(p, f)| p * f).sum();
        centroids.push(weighted / total);

        let log_mean: f64 =
            frame.iter().map(|p| p.max(AMIN).ln()).sum::<f64>() / frame.len() as f64;
        let arith_mean = total / frame.len() as f64;
        flatness_sum += log_mean.exp() / arith_mean.max(AMIN);
        flatness_frames += 1;
    }

    if centroids.is_empty() {
        return SpectralStats {
            centroid_mean_hz: 0.0,
            centroid_std_hz: 0.0,
            flatness_mean: 0.0,
        };
    }

    let mean = centroids.iter().sum::<f64>() / centroids.len() as f64;
    let variance =
        centroids.iter().map(|c| (c - mean).powi(2)).sum::<f64>() / centroids.len() as f64;

    SpectralStats {
        centroid_mean_hz: mean,
        centroid_std_hz: variance.sqrt(),
        flatness_mean: flatness_sum / flatness_frames as f64,
    }
}

pub fn zero_crossing_rate(samples: &[f32]) -> f64 {
    if samples.len() < 2 {
        return 0.0;
    }
    let crossings = samples
        .windows(2)
        .filter(|pair| (pair[0] >= 0.0) != (pair[1] >= 0.0))
        .count();
    crossings as f64 / (samples.len() - 1) as f64
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dsp::tests::{noise, sine};
    use crate::dsp::{HOP, N_FFT, SAMPLE_RATE_HZ};

    fn stats_of(samples: &[f32]) -> SpectralStats {
        let spec = crate::dsp::stft::power_spectrogram(samples, N_FFT, HOP);
        spectral_stats(&spec, f64::from(SAMPLE_RATE_HZ))
    }

    #[test]
    fn sine_centroid_sits_at_its_frequency_and_is_tonal() {
        let stats = stats_of(&sine(1_000.0, 0.7, 5.0));
        assert!(
            (stats.centroid_mean_hz - 1_000.0).abs() < 120.0,
            "centroid {}",
            stats.centroid_mean_hz
        );
        assert!(
            stats.flatness_mean < 0.05,
            "flatness {}",
            stats.flatness_mean
        );
    }

    #[test]
    fn noise_is_flatter_and_brighter_than_a_low_sine() {
        let noise_stats = stats_of(&noise(SAMPLE_RATE_HZ as usize * 5, 3));
        let sine_stats = stats_of(&sine(200.0, 0.7, 5.0));
        assert!(noise_stats.flatness_mean > 0.2);
        assert!(noise_stats.centroid_mean_hz > sine_stats.centroid_mean_hz);
    }

    #[test]
    fn sine_zero_crossing_rate_tracks_frequency() {
        // A sine at f crosses zero 2f times per second: rate ~ 2f / sr.
        let rate = zero_crossing_rate(&sine(1_000.0, 0.7, 5.0));
        let expected = 2.0 * 1_000.0 / f64::from(SAMPLE_RATE_HZ);
        assert!((rate - expected).abs() < 0.1 * expected, "zcr {rate}");
    }
}
