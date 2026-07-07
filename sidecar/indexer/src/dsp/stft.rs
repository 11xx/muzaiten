//! Short-time Fourier transform producing a power spectrogram.
//! Hann window, centered frames via reflect padding, matching the framing
//! conventions documented by librosa so fixtures can be cross-validated
//! against it offline.

use rustfft::num_complex::Complex;
use rustfft::{Fft, FftPlanner};
use std::sync::Arc;

pub struct PowerSpectrogram {
    /// frames[t][k] = |X_t(k)|^2 for k in 0..=n_fft/2.
    pub frames: Vec<Vec<f64>>,
    pub n_fft: usize,
}

impl PowerSpectrogram {
    pub fn bins(&self) -> usize {
        self.n_fft / 2 + 1
    }
}

pub fn power_spectrogram(samples: &[f32], n_fft: usize, hop: usize) -> PowerSpectrogram {
    let padded = reflect_pad(samples, n_fft / 2);
    let window = hann(n_fft);
    let fft: Arc<dyn Fft<f64>> = FftPlanner::new().plan_fft_forward(n_fft);

    let mut frames = Vec::new();
    let mut buffer = vec![Complex::new(0.0, 0.0); n_fft];
    let mut start = 0;
    while start + n_fft <= padded.len() {
        for (i, value) in buffer.iter_mut().enumerate() {
            *value = Complex::new(f64::from(padded[start + i]) * window[i], 0.0);
        }
        fft.process(&mut buffer);
        frames.push(
            buffer[..n_fft / 2 + 1]
                .iter()
                .map(|c| c.norm_sqr())
                .collect(),
        );
        start += hop;
    }
    PowerSpectrogram { frames, n_fft }
}

fn hann(n: usize) -> Vec<f64> {
    (0..n)
        .map(|i| {
            let phase = 2.0 * std::f64::consts::PI * i as f64 / n as f64;
            0.5 - 0.5 * phase.cos()
        })
        .collect()
}

fn reflect_pad(samples: &[f32], pad: usize) -> Vec<f32> {
    if samples.len() < 2 {
        return samples.to_vec();
    }
    let mut out = Vec::with_capacity(samples.len() + 2 * pad);
    for i in (1..=pad.min(samples.len() - 1)).rev() {
        out.push(samples[i]);
    }
    out.extend_from_slice(samples);
    for i in 0..pad.min(samples.len() - 1) {
        out.push(samples[samples.len() - 2 - i]);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sine_energy_lands_in_the_matching_bin() {
        let samples = crate::dsp::tests::sine(1_000.0, 0.8, 2.0);
        let spec = power_spectrogram(&samples, 2_048, 512);
        let expected_bin = (1_000.0f64 / (22_050.0 / 2_048.0)).round() as usize;

        let mid = &spec.frames[spec.frames.len() / 2];
        let peak_bin = mid
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.total_cmp(b.1))
            .map(|(k, _)| k)
            .unwrap();
        assert!(
            peak_bin.abs_diff(expected_bin) <= 1,
            "peak bin {peak_bin}, expected ~{expected_bin}"
        );
    }

    #[test]
    fn reflect_padding_mirrors_edges() {
        let padded = reflect_pad(&[1.0, 2.0, 3.0, 4.0], 2);
        assert_eq!(padded, vec![3.0, 2.0, 1.0, 2.0, 3.0, 4.0, 3.0, 2.0]);
    }
}
