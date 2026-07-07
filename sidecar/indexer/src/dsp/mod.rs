//! Clean-room scalar feature extraction over the canonical decode
//! (mono f32 PCM at 22 050 Hz — the same stream the identity hash consumes).
//!
//! Algorithms are implemented from public specifications and papers only:
//! ITU-R BS.1770-4 K-weighted gated loudness (biquad re-derivation for
//! arbitrary sample rates per Brecht De Man's published coefficients),
//! onset-strength/tempo estimation following Ellis (2007) as popularized by
//! librosa's documented parameterization, and textbook spectral statistics.
//! No GPL code was consulted or translated.

mod loudness;
mod mel;
mod onset;
mod stats;
mod stft;
mod tempo;

/// Bump whenever any extraction algorithm or constant changes; stored in the
/// features table so stale rows can be recomputed selectively.
pub const DSP_VERSION: &str = "muzaiten-dsp-v1";

pub const SAMPLE_RATE_HZ: u32 = 22_050;

const N_FFT: usize = 2_048;
const HOP: usize = 512;
const N_MELS: usize = 128;
/// Analysis needs enough audio for one autocorrelation window plus gating
/// context; anything shorter reports no features rather than noise.
const MIN_DURATION_SECS: f64 = 5.0;

#[derive(Clone, Debug, PartialEq)]
pub struct ScalarFeatures {
    /// Estimated global tempo. None when the onset envelope is too short or
    /// carries no periodic energy (drones, field recordings, silence).
    pub tempo_bpm: Option<f64>,
    /// BS.1770-4 integrated loudness. None for effectively silent audio
    /// (no gating block above the absolute threshold).
    pub loudness_lufs: Option<f64>,
    /// Standard deviation of gated 400 ms block loudness, in dB — a cheap
    /// dynamics proxy (NOT EBU Tech 3342 LRA).
    pub loudness_std_db: Option<f64>,
    pub spectral_centroid_mean_hz: f64,
    pub spectral_centroid_std_hz: f64,
    /// Mean per-frame spectral flatness of the power spectrum, 0..1.
    pub spectral_flatness_mean: f64,
    /// Fraction of adjacent sample pairs that change sign, 0..1.
    pub zero_crossing_rate: f64,
    /// Detected onsets per second over the whole signal.
    pub onset_rate_hz: f64,
    /// Unit-scale perceived-intensity blend, versioned by DSP_VERSION.
    /// None when loudness is None.
    pub energy: Option<f64>,
}

/// Analyze the canonical decode of one track. `samples` must be mono PCM at
/// [`SAMPLE_RATE_HZ`]. Returns None for input too short to analyze.
pub fn analyze(samples: &[f32], sample_rate: u32) -> Option<ScalarFeatures> {
    let rate = f64::from(sample_rate);
    if (samples.len() as f64) < MIN_DURATION_SECS * rate {
        return None;
    }

    let spectrogram = stft::power_spectrogram(samples, N_FFT, HOP);
    if spectrogram.frames.is_empty() {
        return None;
    }

    let mel_bank = mel::MelBank::slaney(N_MELS, N_FFT, rate);
    let envelope = onset::onset_envelope(&spectrogram, &mel_bank);
    let frame_rate = rate / HOP as f64;

    let tempo_bpm = tempo::estimate_bpm(&envelope, frame_rate);
    let onset_rate_hz = onset::onset_rate(&envelope, frame_rate);
    let loudness = loudness::gated_loudness(samples, rate);
    let spectral = stats::spectral_stats(&spectrogram, rate);
    let zero_crossing_rate = stats::zero_crossing_rate(samples);

    let energy = loudness
        .integrated_lufs
        .map(|lufs| energy_v1(lufs, onset_rate_hz));

    Some(ScalarFeatures {
        tempo_bpm,
        loudness_lufs: loudness.integrated_lufs,
        loudness_std_db: loudness.block_std_db,
        spectral_centroid_mean_hz: spectral.centroid_mean_hz,
        spectral_centroid_std_hz: spectral.centroid_std_hz,
        spectral_flatness_mean: spectral.flatness_mean,
        zero_crossing_rate,
        onset_rate_hz,
        energy,
    })
}

/// energy v1: 0.6 × loudness (−35 LUFS → 0, −5 LUFS → 1) + 0.4 × onset
/// density (6 onsets/s → 1). The blend is deliberately simple and lives
/// behind DSP_VERSION so it can be retuned against a real corpus later.
fn energy_v1(lufs: f64, onset_rate_hz: f64) -> f64 {
    let loud = ((lufs + 35.0) / 30.0).clamp(0.0, 1.0);
    let onsets = (onset_rate_hz / 6.0).clamp(0.0, 1.0);
    0.6 * loud + 0.4 * onsets
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Deterministic xorshift noise in [-1, 1]; no rand dependency.
    pub(crate) fn noise(len: usize, seed: u64) -> Vec<f32> {
        let mut state = seed.max(1);
        let mut out = Vec::with_capacity(len);
        for _ in 0..len {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            let unit = (state >> 11) as f64 / (1u64 << 53) as f64;
            out.push((unit * 2.0 - 1.0) as f32);
        }
        out
    }

    pub(crate) fn sine(freq_hz: f64, amplitude: f64, secs: f64) -> Vec<f32> {
        let n = (secs * SAMPLE_RATE_HZ as f64) as usize;
        (0..n)
            .map(|i| {
                let t = i as f64 / SAMPLE_RATE_HZ as f64;
                (amplitude * (2.0 * std::f64::consts::PI * freq_hz * t).sin()) as f32
            })
            .collect()
    }

    /// Impulse train at the given tempo: a 1-sample click every beat.
    pub(crate) fn clicks(bpm: f64, secs: f64) -> Vec<f32> {
        let n = (secs * SAMPLE_RATE_HZ as f64) as usize;
        let period = (60.0 / bpm * SAMPLE_RATE_HZ as f64).round() as usize;
        let mut out = vec![0.0f32; n];
        let mut i = 0;
        while i < n {
            out[i] = 1.0;
            i += period;
        }
        out
    }

    #[test]
    fn too_short_input_yields_none() {
        assert!(analyze(&sine(440.0, 0.5, 1.0), SAMPLE_RATE_HZ).is_none());
    }

    #[test]
    fn silence_has_no_loudness_energy_or_tempo() {
        let features = analyze(&vec![0.0f32; SAMPLE_RATE_HZ as usize * 10], SAMPLE_RATE_HZ)
            .expect("long enough to analyze");
        assert_eq!(features.loudness_lufs, None);
        assert_eq!(features.energy, None);
        assert_eq!(features.tempo_bpm, None);
        assert_eq!(features.onset_rate_hz, 0.0);
    }

    #[test]
    fn loud_dense_audio_outranks_quiet_sparse_audio_on_energy() {
        let loud = analyze(&noise(SAMPLE_RATE_HZ as usize * 10, 7), SAMPLE_RATE_HZ).unwrap();
        let quiet: Vec<f32> = noise(SAMPLE_RATE_HZ as usize * 10, 7)
            .iter()
            .map(|s| s * 0.01)
            .collect();
        let quiet = analyze(&quiet, SAMPLE_RATE_HZ).unwrap();
        assert!(loud.energy.unwrap() > quiet.energy.unwrap());
    }

    #[test]
    fn click_track_tempo_is_recovered() {
        for bpm in [90.0, 120.0, 150.0] {
            let features = analyze(&clicks(bpm, 30.0), SAMPLE_RATE_HZ).unwrap();
            let estimate = features.tempo_bpm.expect("clicks have a tempo");
            assert!(
                (estimate - bpm).abs() <= 3.0,
                "expected ~{bpm} BPM, estimated {estimate}"
            );
        }
    }

    #[test]
    fn click_track_onset_rate_matches_click_density() {
        let features = analyze(&clicks(120.0, 30.0), SAMPLE_RATE_HZ).unwrap();
        assert!(
            (features.onset_rate_hz - 2.0).abs() <= 0.4,
            "120 BPM clicks are 2 onsets/s, got {}",
            features.onset_rate_hz
        );
    }
}
