//! ITU-R BS.1770-4 integrated loudness for mono audio.
//!
//! The spec publishes K-weighting biquad coefficients only for 48 kHz; this
//! uses Brecht De Man's published spec-compliant re-derivation (bilinear
//! transform from the analog prototype), which is valid at any sample rate —
//! the same approach pyloudnorm (MIT) implements.

pub struct Loudness {
    pub integrated_lufs: Option<f64>,
    /// Standard deviation of block loudness over blocks passing the absolute
    /// gate; a dynamics proxy, not EBU Tech 3342 LRA.
    pub block_std_db: Option<f64>,
}

const BLOCK_SECS: f64 = 0.400;
const BLOCK_OVERLAP: f64 = 0.75;
const ABSOLUTE_GATE_LUFS: f64 = -70.0;
const RELATIVE_GATE_LU: f64 = -10.0;
const LUFS_OFFSET: f64 = -0.691;

pub fn gated_loudness(samples: &[f32], sample_rate: f64) -> Loudness {
    let mut weighted: Vec<f64> = samples.iter().map(|s| f64::from(*s)).collect();
    apply_biquad(&mut weighted, high_shelf(sample_rate));
    apply_biquad(&mut weighted, high_pass(sample_rate));

    let block_len = (BLOCK_SECS * sample_rate) as usize;
    let step = ((1.0 - BLOCK_OVERLAP) * BLOCK_SECS * sample_rate) as usize;
    if block_len == 0 || step == 0 || weighted.len() < block_len {
        return Loudness {
            integrated_lufs: None,
            block_std_db: None,
        };
    }

    // Mean-square power z_j per 400 ms block (BS.1770 eq. 1), mono gain 1.0.
    let mut block_power = Vec::new();
    let mut start = 0;
    while start + block_len <= weighted.len() {
        let sum_sq: f64 = weighted[start..start + block_len]
            .iter()
            .map(|x| x * x)
            .sum();
        block_power.push(sum_sq / block_len as f64);
        start += step;
    }

    let block_lufs: Vec<f64> = block_power
        .iter()
        .map(|z| LUFS_OFFSET + 10.0 * z.max(f64::MIN_POSITIVE).log10())
        .collect();

    // Absolute gate at -70 LUFS (eq. 5).
    let abs_gated: Vec<usize> = (0..block_power.len())
        .filter(|j| block_lufs[*j] >= ABSOLUTE_GATE_LUFS)
        .collect();
    if abs_gated.is_empty() {
        return Loudness {
            integrated_lufs: None,
            block_std_db: None,
        };
    }

    // Relative gate 10 LU under the abs-gated mean power (eq. 6).
    let abs_mean: f64 =
        abs_gated.iter().map(|j| block_power[*j]).sum::<f64>() / abs_gated.len() as f64;
    let relative_gate = LUFS_OFFSET + 10.0 * abs_mean.log10() + RELATIVE_GATE_LU;

    let gated: Vec<usize> = abs_gated
        .iter()
        .copied()
        .filter(|j| block_lufs[*j] > relative_gate)
        .collect();
    if gated.is_empty() {
        return Loudness {
            integrated_lufs: None,
            block_std_db: None,
        };
    }

    let gated_mean: f64 = gated.iter().map(|j| block_power[*j]).sum::<f64>() / gated.len() as f64;
    let integrated = LUFS_OFFSET + 10.0 * gated_mean.log10();

    let abs_gated_lufs: Vec<f64> = abs_gated.iter().map(|j| block_lufs[*j]).collect();
    let mean_lufs = abs_gated_lufs.iter().sum::<f64>() / abs_gated_lufs.len() as f64;
    let variance = abs_gated_lufs
        .iter()
        .map(|l| (l - mean_lufs).powi(2))
        .sum::<f64>()
        / abs_gated_lufs.len() as f64;

    Loudness {
        integrated_lufs: Some(integrated),
        block_std_db: Some(variance.sqrt()),
    }
}

struct Biquad {
    b0: f64,
    b1: f64,
    b2: f64,
    a1: f64,
    a2: f64,
}

/// K-weighting stage 1: +3.99984 dB high shelf at 1681.97 Hz, Q 0.70718
/// (De Man's spec-matched parameters).
fn high_shelf(rate: f64) -> Biquad {
    let g_db = 3.999_843_854;
    let q = 0.707_175_237;
    let fc = 1_681.974_451;

    let k = (std::f64::consts::PI * fc / rate).tan();
    let vh = 10f64.powf(g_db / 20.0);
    let vb = vh.powf(0.499_666_774);
    let a0 = 1.0 + k / q + k * k;
    Biquad {
        b0: (vh + vb * k / q + k * k) / a0,
        b1: 2.0 * (k * k - vh) / a0,
        b2: (vh - vb * k / q + k * k) / a0,
        a1: 2.0 * (k * k - 1.0) / a0,
        a2: (1.0 - k / q + k * k) / a0,
    }
}

/// K-weighting stage 2: high pass at 38.135 Hz, Q 0.50033.
fn high_pass(rate: f64) -> Biquad {
    let q = 0.500_327_037;
    let fc = 38.135_470_88;

    let k = (std::f64::consts::PI * fc / rate).tan();
    let a0 = 1.0 + k / q + k * k;
    Biquad {
        b0: 1.0,
        b1: -2.0,
        b2: 1.0,
        a1: 2.0 * (k * k - 1.0) / a0,
        a2: (1.0 - k / q + k * k) / a0,
    }
}

fn apply_biquad(samples: &mut [f64], f: Biquad) {
    let (mut x1, mut x2, mut y1, mut y2) = (0.0, 0.0, 0.0, 0.0);
    for sample in samples.iter_mut() {
        let x0 = *sample;
        let y0 = f.b0 * x0 + f.b1 * x1 + f.b2 * x2 - f.a1 * y1 - f.a2 * y2;
        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;
        *sample = y0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dsp::tests::sine;

    #[test]
    fn full_scale_997hz_sine_is_near_minus_3_7_lufs() {
        // Mean square of a full-scale sine is 0.5 -> 10log10(0.5) = -3.01,
        // plus the -0.691 offset; K-weighting is near-unity around 1 kHz.
        let loudness = gated_loudness(&sine(997.0, 1.0, 10.0), 22_050.0);
        let lufs = loudness.integrated_lufs.unwrap();
        assert!((-4.5..=-2.8).contains(&lufs), "got {lufs} LUFS");
    }

    #[test]
    fn halving_amplitude_drops_loudness_six_db() {
        let loud = gated_loudness(&sine(997.0, 0.8, 10.0), 22_050.0);
        let soft = gated_loudness(&sine(997.0, 0.4, 10.0), 22_050.0);
        let delta = loud.integrated_lufs.unwrap() - soft.integrated_lufs.unwrap();
        assert!((delta - 6.02).abs() < 0.1, "delta {delta}");
    }

    #[test]
    fn silence_gates_out_entirely() {
        let loudness = gated_loudness(&vec![0.0f32; 22_050 * 10], 22_050.0);
        assert_eq!(loudness.integrated_lufs, None);
        assert_eq!(loudness.block_std_db, None);
    }

    #[test]
    fn steady_tone_has_negligible_block_spread() {
        let loudness = gated_loudness(&sine(997.0, 0.5, 10.0), 22_050.0);
        assert!(loudness.block_std_db.unwrap() < 0.2);
    }
}
