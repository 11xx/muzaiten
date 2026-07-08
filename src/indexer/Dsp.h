#pragma once

// Clean-room scalar feature extraction over the canonical decode (mono
// float PCM at 22 050 Hz — the same stream the identity hash consumes).
//
// Algorithms are implemented from public specifications and papers only:
// ITU-R BS.1770-4 K-weighted gated loudness (biquad re-derivation for
// arbitrary sample rates per Brecht De Man's published coefficients),
// onset-strength/tempo estimation following Ellis (2007) as popularized by
// librosa's documented parameterization, and textbook spectral statistics.
// No GPL code was consulted or translated. Ported from the original Rust
// implementation (archived at ~/code/muzaiten-dsp-rs) with identical
// constants and semantics.
//
// Deliberately Qt-free: pure math over std containers, usable from any
// binary and trivially compilable standalone for oracle cross-checks.

#include <optional>
#include <vector>

namespace Dsp {

// Bump whenever any extraction algorithm or constant changes; stored in the
// features table so stale rows can be recomputed selectively.
inline constexpr const char *kDspVersion = "muzaiten-dsp-v1";

inline constexpr unsigned kSampleRateHz = 22'050;

struct ScalarFeatures {
    // Estimated global tempo. Empty when the onset envelope is too short or
    // carries no periodic energy (drones, field recordings, silence).
    std::optional<double> tempoBpm;
    // BS.1770-4 integrated loudness. Empty for effectively silent audio
    // (no gating block above the absolute threshold).
    std::optional<double> loudnessLufs;
    // Standard deviation of gated 400 ms block loudness, in dB — a cheap
    // dynamics proxy (NOT EBU Tech 3342 LRA).
    std::optional<double> loudnessStdDb;
    double spectralCentroidMeanHz = 0.0;
    double spectralCentroidStdHz = 0.0;
    // Mean per-frame spectral flatness of the power spectrum, 0..1.
    double spectralFlatnessMean = 0.0;
    // Fraction of adjacent sample pairs that change sign, 0..1.
    double zeroCrossingRate = 0.0;
    // Detected onsets per second over the whole signal.
    double onsetRateHz = 0.0;
    // Unit-scale perceived-intensity blend, versioned by kDspVersion.
    // Empty when loudness is empty.
    std::optional<double> energy;
};

// Analyze the canonical decode of one track. Returns nullopt for input too
// short to analyze (< 5 s).
std::optional<ScalarFeatures> analyze(const std::vector<float> &samples, unsigned sampleRate);

// The building blocks below are exposed for tests and oracle tooling.

struct PowerSpectrogram {
    // frames[t][k] = |X_t(k)|^2 for k in 0..=nFft/2.
    std::vector<std::vector<double>> frames;
    std::size_t nFft = 0;

    std::size_t bins() const { return nFft / 2 + 1; }
};

PowerSpectrogram powerSpectrogram(const std::vector<float> &samples, std::size_t nFft,
                                  std::size_t hop);

// Slaney-style mel filterbank (linear below 1 kHz, logarithmic above, area
// normalized), built from the published Auditory Toolbox formulation.
class MelBank {
public:
    static MelBank slaney(std::size_t nMels, std::size_t nFft, double sampleRate);

    std::vector<double> apply(const std::vector<double> &powerFrame) const;

    struct SparseSpan {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t offset = 0;
    };

    // weights[m][k] over the nFft/2+1 linear bins.
    std::vector<std::vector<double>> weights;
    std::vector<SparseSpan> sparseSpans;
    std::vector<double> sparseWeights;
};

// Spectral-flux onset strength: per-band dB difference between consecutive
// frames, half-wave rectified, averaged across bands. Index-aligned with
// spectrogram frames (frame 0 has no predecessor and is emitted as 0).
std::vector<double> onsetEnvelope(const PowerSpectrogram &spectrogram, const MelBank &melBank);

// Onsets per second: local maxima clearing a moving-average threshold.
double onsetRate(const std::vector<double> &envelope, double frameRate);

// Global tempo following Ellis (2007): windowed autocorrelation of the
// onset envelope, averaged, weighted by a 120 BPM-centered log-normal
// prior. May fold extreme tempi to the half/double octave.
std::optional<double> estimateBpm(const std::vector<double> &envelope, double frameRate);

struct Loudness {
    std::optional<double> integratedLufs;
    // Std deviation of block loudness over blocks passing the absolute
    // gate; a dynamics proxy, not EBU Tech 3342 LRA.
    std::optional<double> blockStdDb;
};

// ITU-R BS.1770-4 integrated gated loudness for mono audio.
Loudness gatedLoudness(const std::vector<float> &samples, double sampleRate);

struct SpectralStats {
    double centroidMeanHz = 0.0;
    double centroidStdHz = 0.0;
    double flatnessMean = 0.0;
};

SpectralStats spectralStats(const PowerSpectrogram &spectrogram, double sampleRate);

double zeroCrossingRate(const std::vector<float> &samples);

} // namespace Dsp
