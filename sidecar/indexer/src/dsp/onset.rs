//! Spectral-flux onset strength envelope over a mel spectrogram, and a
//! moving-threshold onset counter. The envelope follows the standard
//! formulation: per-band dB difference between consecutive frames,
//! half-wave rectified, averaged across bands.

use super::mel::MelBank;
use super::stft::PowerSpectrogram;

const AMIN: f64 = 1e-10;
const TOP_DB: f64 = 80.0;

/// onset_envelope[t] for t in 1..frames (first frame has no predecessor and
/// is emitted as 0 to keep envelope indices aligned with spectrogram frames).
pub fn onset_envelope(spectrogram: &PowerSpectrogram, mel_bank: &MelBank) -> Vec<f64> {
    let mel_db: Vec<Vec<f64>> = {
        let mel_power: Vec<Vec<f64>> = spectrogram
            .frames
            .iter()
            .map(|frame| mel_bank.apply(frame))
            .collect();
        let max_power = mel_power.iter().flatten().copied().fold(AMIN, f64::max);
        let floor_db = 10.0 * max_power.log10() - TOP_DB;
        mel_power
            .iter()
            .map(|frame| {
                frame
                    .iter()
                    .map(|p| (10.0 * p.max(AMIN).log10()).max(floor_db))
                    .collect()
            })
            .collect()
    };

    let mut envelope = vec![0.0];
    for t in 1..mel_db.len() {
        let flux: f64 = mel_db[t]
            .iter()
            .zip(&mel_db[t - 1])
            .map(|(now, before)| (now - before).max(0.0))
            .sum::<f64>()
            / mel_db[t].len() as f64;
        envelope.push(flux);
    }
    envelope
}

/// Count onsets as local maxima that clear a moving-average threshold, and
/// return them per second. Window and delta are deliberately simple; the
/// value feeds the energy blend and fixtures, not beat tracking.
pub fn onset_rate(envelope: &[f64], frame_rate: f64) -> f64 {
    if envelope.len() < 3 || frame_rate <= 0.0 {
        return 0.0;
    }
    let global_max = envelope.iter().copied().fold(0.0, f64::max);
    if global_max <= 0.0 {
        return 0.0;
    }

    let half_window = frame_rate as usize; // ~1 s of context each side
    let delta = 0.05 * global_max;
    let mut count = 0usize;
    for t in 1..envelope.len() - 1 {
        if envelope[t] <= envelope[t - 1] || envelope[t] < envelope[t + 1] {
            continue;
        }
        let lo = t.saturating_sub(half_window);
        let hi = (t + half_window + 1).min(envelope.len());
        let local_mean: f64 = envelope[lo..hi].iter().sum::<f64>() / (hi - lo) as f64;
        if envelope[t] > local_mean + delta {
            count += 1;
        }
    }
    count as f64 / (envelope.len() as f64 / frame_rate)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dsp::tests::clicks;
    use crate::dsp::{HOP, N_FFT, N_MELS, SAMPLE_RATE_HZ};

    #[test]
    fn clicks_produce_periodic_envelope_peaks() {
        let samples = clicks(120.0, 10.0);
        let spec = crate::dsp::stft::power_spectrogram(&samples, N_FFT, HOP);
        let bank = MelBank::slaney(N_MELS, N_FFT, f64::from(SAMPLE_RATE_HZ));
        let envelope = onset_envelope(&spec, &bank);

        let frame_rate = f64::from(SAMPLE_RATE_HZ) / HOP as f64;
        let rate = onset_rate(&envelope, frame_rate);
        assert!((rate - 2.0).abs() < 0.4, "expected ~2 onsets/s, got {rate}");
    }

    #[test]
    fn flat_envelope_counts_no_onsets() {
        assert_eq!(onset_rate(&vec![0.0; 500], 43.0), 0.0);
        assert_eq!(onset_rate(&vec![1.0; 500], 43.0), 0.0);
    }
}
