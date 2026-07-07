//! Slaney-style mel filterbank (linear below 1 kHz, logarithmic above, area
//! normalized), built from the published Auditory Toolbox formulation.

pub struct MelBank {
    /// weights[m][k] over the n_fft/2+1 linear bins.
    pub weights: Vec<Vec<f64>>,
}

impl MelBank {
    pub fn slaney(n_mels: usize, n_fft: usize, sample_rate: f64) -> Self {
        let n_bins = n_fft / 2 + 1;
        let fmax = sample_rate / 2.0;

        let mel_points: Vec<f64> = {
            let mel_min = hz_to_mel(0.0);
            let mel_max = hz_to_mel(fmax);
            (0..n_mels + 2)
                .map(|i| mel_min + (mel_max - mel_min) * i as f64 / (n_mels + 1) as f64)
                .map(mel_to_hz)
                .collect()
        };
        let bin_freqs: Vec<f64> = (0..n_bins)
            .map(|k| k as f64 * sample_rate / n_fft as f64)
            .collect();

        let mut weights = vec![vec![0.0; n_bins]; n_mels];
        for m in 0..n_mels {
            let (lower, center, upper) = (mel_points[m], mel_points[m + 1], mel_points[m + 2]);
            // Slaney area normalization keeps per-filter energy comparable.
            let enorm = 2.0 / (upper - lower);
            for (k, &freq) in bin_freqs.iter().enumerate() {
                let rising = (freq - lower) / (center - lower);
                let falling = (upper - freq) / (upper - center);
                let weight = rising.min(falling).max(0.0);
                weights[m][k] = weight * enorm;
            }
        }
        MelBank { weights }
    }

    pub fn apply(&self, power_frame: &[f64]) -> Vec<f64> {
        self.weights
            .iter()
            .map(|filter| {
                filter
                    .iter()
                    .zip(power_frame)
                    .map(|(w, p)| w * p)
                    .sum::<f64>()
            })
            .collect()
    }
}

const MIN_LOG_HZ: f64 = 1_000.0;
const LINEAR_SLOPE: f64 = 3.0 / 200.0;
const MIN_LOG_MEL: f64 = MIN_LOG_HZ * LINEAR_SLOPE;

fn log_step() -> f64 {
    (6.4f64).ln() / 27.0
}

fn hz_to_mel(hz: f64) -> f64 {
    if hz < MIN_LOG_HZ {
        hz * LINEAR_SLOPE
    } else {
        MIN_LOG_MEL + (hz / MIN_LOG_HZ).ln() / log_step()
    }
}

fn mel_to_hz(mel: f64) -> f64 {
    if mel < MIN_LOG_MEL {
        mel / LINEAR_SLOPE
    } else {
        MIN_LOG_HZ * ((mel - MIN_LOG_MEL) * log_step()).exp()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mel_scale_round_trips() {
        for hz in [0.0, 440.0, 999.0, 1_000.0, 4_000.0, 11_025.0] {
            let round = mel_to_hz(hz_to_mel(hz));
            assert!((round - hz).abs() < 1e-6, "{hz} -> {round}");
        }
    }

    #[test]
    fn filters_are_nonnegative_and_cover_the_band() {
        let bank = MelBank::slaney(128, 2_048, 22_050.0);
        assert_eq!(bank.weights.len(), 128);
        for filter in &bank.weights {
            assert!(filter.iter().all(|w| *w >= 0.0));
            assert!(filter.iter().any(|w| *w > 0.0), "empty filter");
        }
    }
}
