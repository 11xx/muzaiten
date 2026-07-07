//! Global tempo estimation following Ellis (2007): autocorrelate the onset
//! strength envelope over ~8 s windows, average, then pick the lag whose BPM
//! maximizes autocorrelation weighted by a log-normal prior centered at
//! 120 BPM. The prior resolves octave ambiguity the same way librosa's
//! documented default parameterization does.

const AC_WINDOW_SECS: f64 = 8.0;
const START_BPM: f64 = 120.0;
const STD_BPM_OCTAVES: f64 = 1.0;
const MAX_BPM: f64 = 320.0;
const MIN_BPM: f64 = 30.0;

pub fn estimate_bpm(envelope: &[f64], frame_rate: f64) -> Option<f64> {
    let window = (AC_WINDOW_SECS * frame_rate) as usize;
    if window < 8 || envelope.len() < window {
        return None;
    }
    if envelope.iter().all(|v| *v <= 0.0) {
        return None;
    }

    // Mean of per-window normalized autocorrelations over non-overlapping
    // windows (a windowed tempogram aggregated with mean; non-overlapping
    // windows are a documented-equivalent cheapening of the per-frame slide).
    let mut mean_ac = vec![0.0f64; window];
    let mut windows = 0usize;
    let mut start = 0usize;
    while start + window <= envelope.len() {
        let frame = &envelope[start..start + window];
        let ac = autocorrelate(frame);
        let peak = ac.iter().copied().fold(f64::MIN_POSITIVE, f64::max);
        for (accum, value) in mean_ac.iter_mut().zip(&ac) {
            *accum += value / peak;
        }
        windows += 1;
        start += window;
    }
    if windows == 0 {
        return None;
    }
    for value in &mut mean_ac {
        *value /= windows as f64;
    }

    let mut best: Option<(f64, f64)> = None; // (score, bpm)
    for (lag, &ac) in mean_ac.iter().enumerate().skip(1) {
        let bpm = 60.0 * frame_rate / lag as f64;
        if !(MIN_BPM..MAX_BPM).contains(&bpm) {
            continue;
        }
        let octaves = (bpm.log2() - START_BPM.log2()) / STD_BPM_OCTAVES;
        let log_prior = -0.5 * octaves * octaves;
        // log1p compresses the autocorrelation into the prior's scale while
        // preserving peak ordering.
        let score = (1e6 * ac.max(0.0)).ln_1p() + log_prior;
        if best.is_none_or(|(top, _)| score > top) {
            best = Some((score, bpm));
        }
    }
    best.map(|(_, bpm)| bpm)
}

fn autocorrelate(frame: &[f64]) -> Vec<f64> {
    let n = frame.len();
    let mut ac = vec![0.0; n];
    for lag in 0..n {
        let mut sum = 0.0;
        for i in 0..n - lag {
            sum += frame[i] * frame[i + lag];
        }
        ac[lag] = sum;
    }
    ac
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Beats land at fractional frame positions, so split each impulse
    /// between the two neighboring frames the way a real (smooth) onset
    /// envelope spreads its peaks. Single-bin truncated impulses alias
    /// energy onto the double period and test the fixture, not the code.
    fn impulse_envelope(bpm: f64, frame_rate: f64, secs: f64) -> Vec<f64> {
        let frames = (secs * frame_rate) as usize;
        let period = 60.0 / bpm * frame_rate;
        let mut envelope = vec![0.0; frames];
        let mut beat = 0.0f64;
        while (beat.floor() as usize) + 1 < frames {
            let index = beat.floor() as usize;
            let fraction = beat - beat.floor();
            envelope[index] += 1.0 - fraction;
            envelope[index + 1] += fraction;
            beat += period;
        }
        envelope
    }

    #[test]
    fn recovers_tempo_across_the_common_range() {
        let frame_rate = 22_050.0 / 512.0;
        for bpm in [70.0, 90.0, 120.0, 150.0] {
            let envelope = impulse_envelope(bpm, frame_rate, 60.0);
            let estimate = estimate_bpm(&envelope, frame_rate).unwrap();
            assert!(
                (estimate - bpm).abs() <= 3.0,
                "expected ~{bpm}, got {estimate}"
            );
        }
    }

    /// Past ~160 BPM the 120-centered log-normal prior may fold an estimate
    /// onto its half octave — the documented, expected behavior of a global
    /// tempo estimator (octave-equivalent counts as correct in Ellis's own
    /// evaluation). Pin that contract rather than exact recovery.
    #[test]
    fn extreme_tempo_is_recovered_up_to_octave_equivalence() {
        let frame_rate = 22_050.0 / 512.0;
        let envelope = impulse_envelope(180.0, frame_rate, 60.0);
        let estimate = estimate_bpm(&envelope, frame_rate).unwrap();
        let octave_error = (estimate.log2() - 180.0f64.log2()).abs();
        let folded = (octave_error - 1.0).abs();
        assert!(
            octave_error.min(folded) < 0.05,
            "expected 180 or 90 BPM, got {estimate}"
        );
    }

    #[test]
    fn empty_or_flat_envelope_has_no_tempo() {
        let frame_rate = 22_050.0 / 512.0;
        assert_eq!(estimate_bpm(&[], frame_rate), None);
        assert_eq!(estimate_bpm(&vec![0.0; 2_000], frame_rate), None);
    }
}
