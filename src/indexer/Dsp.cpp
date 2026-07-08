#include "indexer/Dsp.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>

namespace {

constexpr std::size_t kNFft = 2'048;
constexpr std::size_t kHop = 512;
constexpr std::size_t kNMels = 128;
// Analysis needs enough audio for one autocorrelation window plus gating
// context; anything shorter reports no features rather than noise.
constexpr double kMinDurationSecs = 5.0;

constexpr double kAmin = 1e-10;
constexpr double kTopDb = 80.0;
// Frames whose total power is below this are treated as silence and skipped
// so centroid/flatness describe the audible program, not the noise floor.
constexpr double kSilentFramePower = 1e-9;

constexpr double kAcWindowSecs = 8.0;
constexpr double kStartBpm = 120.0;
constexpr double kStdBpmOctaves = 1.0;
constexpr double kMaxBpm = 320.0;
constexpr double kMinBpm = 30.0;

constexpr double kBlockSecs = 0.400;
constexpr double kBlockOverlap = 0.75;
constexpr double kAbsoluteGateLufs = -70.0;
constexpr double kRelativeGateLu = -10.0;
constexpr double kLufsOffset = -0.691;

// In-place iterative radix-2 FFT; kNFft is a power of two by construction,
// which is the whole reason no FFT library is needed.
void fftRadix2(std::vector<std::complex<double>> &buffer)
{
    const std::size_t n = buffer.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j |= bit;
        if (i < j) {
            std::swap(buffer[i], buffer[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * std::numbers::pi / static_cast<double>(len);
        const std::complex<double> root(std::cos(angle), std::sin(angle));
        for (std::size_t start = 0; start < n; start += len) {
            std::complex<double> twiddle(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> even = buffer[start + k];
                const std::complex<double> odd = buffer[start + k + len / 2] * twiddle;
                buffer[start + k] = even + odd;
                buffer[start + k + len / 2] = even - odd;
                twiddle *= root;
            }
        }
    }
}

std::vector<double> hannWindow(std::size_t n)
{
    std::vector<double> window(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n);
        window[i] = 0.5 - 0.5 * std::cos(phase);
    }
    return window;
}

std::vector<float> reflectPad(const std::vector<float> &samples, std::size_t pad)
{
    if (samples.size() < 2) {
        return samples;
    }
    const std::size_t edge = std::min(pad, samples.size() - 1);
    std::vector<float> out;
    out.reserve(samples.size() + 2 * edge);
    for (std::size_t i = edge; i >= 1; --i) {
        out.push_back(samples[i]);
    }
    out.insert(out.end(), samples.begin(), samples.end());
    for (std::size_t i = 0; i < edge; ++i) {
        out.push_back(samples[samples.size() - 2 - i]);
    }
    return out;
}

constexpr double kMinLogHz = 1'000.0;
constexpr double kLinearSlope = 3.0 / 200.0;
constexpr double kMinLogMel = kMinLogHz * kLinearSlope;

double melLogStep()
{
    return std::log(6.4) / 27.0;
}

double hzToMel(double hz)
{
    if (hz < kMinLogHz) {
        return hz * kLinearSlope;
    }
    return kMinLogMel + std::log(hz / kMinLogHz) / melLogStep();
}

double melToHz(double mel)
{
    if (mel < kMinLogMel) {
        return mel / kLinearSlope;
    }
    return kMinLogHz * std::exp((mel - kMinLogMel) * melLogStep());
}

struct Biquad {
    double b0 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

// K-weighting stage 1: +3.99984 dB high shelf at 1681.97 Hz, Q 0.70718
// (De Man's spec-matched parameters).
Biquad kWeightHighShelf(double rate)
{
    const double gDb = 3.999'843'854;
    const double q = 0.707'175'237;
    const double fc = 1'681.974'451;

    const double k = std::tan(std::numbers::pi * fc / rate);
    const double vh = std::pow(10.0, gDb / 20.0);
    const double vb = std::pow(vh, 0.499'666'774);
    const double a0 = 1.0 + k / q + k * k;
    return {
        (vh + vb * k / q + k * k) / a0,
        2.0 * (k * k - vh) / a0,
        (vh - vb * k / q + k * k) / a0,
        2.0 * (k * k - 1.0) / a0,
        (1.0 - k / q + k * k) / a0,
    };
}

// K-weighting stage 2: high pass at 38.135 Hz, Q 0.50033.
Biquad kWeightHighPass(double rate)
{
    const double q = 0.500'327'037;
    const double fc = 38.135'470'88;

    const double k = std::tan(std::numbers::pi * fc / rate);
    const double a0 = 1.0 + k / q + k * k;
    return {
        1.0,
        -2.0,
        1.0,
        2.0 * (k * k - 1.0) / a0,
        (1.0 - k / q + k * k) / a0,
    };
}

void applyBiquad(std::vector<double> &samples, const Biquad &f)
{
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
    for (double &sample : samples) {
        const double x0 = sample;
        const double y0 = f.b0 * x0 + f.b1 * x1 + f.b2 * x2 - f.a1 * y1 - f.a2 * y2;
        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;
        sample = y0;
    }
}

std::vector<double> autocorrelate(const double *frame, std::size_t n)
{
    std::vector<double> ac(n, 0.0);
    for (std::size_t lag = 0; lag < n; ++lag) {
        double sum = 0.0;
        for (std::size_t i = 0; i < n - lag; ++i) {
            sum += frame[i] * frame[i + lag];
        }
        ac[lag] = sum;
    }
    return ac;
}

// energy v1: 0.6 × loudness (−35 LUFS → 0, −5 LUFS → 1) + 0.4 × onset
// density (6 onsets/s → 1). The blend is deliberately simple and lives
// behind kDspVersion so it can be retuned against a real corpus later.
double energyV1(double lufs, double onsetRateHz)
{
    const double loud = std::clamp((lufs + 35.0) / 30.0, 0.0, 1.0);
    const double onsets = std::clamp(onsetRateHz / 6.0, 0.0, 1.0);
    return 0.6 * loud + 0.4 * onsets;
}

} // namespace

namespace Dsp {

PowerSpectrogram powerSpectrogram(const std::vector<float> &samples, std::size_t nFft,
                                  std::size_t hop)
{
    const std::vector<float> padded = reflectPad(samples, nFft / 2);
    const std::vector<double> window = hannWindow(nFft);

    PowerSpectrogram spectrogram;
    spectrogram.nFft = nFft;
    std::vector<std::complex<double>> buffer(nFft);
    for (std::size_t start = 0; start + nFft <= padded.size(); start += hop) {
        for (std::size_t i = 0; i < nFft; ++i) {
            buffer[i] = {static_cast<double>(padded[start + i]) * window[i], 0.0};
        }
        fftRadix2(buffer);
        std::vector<double> frame(nFft / 2 + 1);
        for (std::size_t k = 0; k <= nFft / 2; ++k) {
            frame[k] = std::norm(buffer[k]);
        }
        spectrogram.frames.push_back(std::move(frame));
    }
    return spectrogram;
}

MelBank MelBank::slaney(std::size_t nMels, std::size_t nFft, double sampleRate)
{
    const std::size_t nBins = nFft / 2 + 1;
    const double fmax = sampleRate / 2.0;

    std::vector<double> melPoints(nMels + 2);
    const double melMin = hzToMel(0.0);
    const double melMax = hzToMel(fmax);
    for (std::size_t i = 0; i < melPoints.size(); ++i) {
        const double mel =
            melMin + (melMax - melMin) * static_cast<double>(i) / static_cast<double>(nMels + 1);
        melPoints[i] = melToHz(mel);
    }

    MelBank bank;
    bank.weights.assign(nMels, std::vector<double>(nBins, 0.0));
    bank.sparseSpans.reserve(nMels);
    for (std::size_t m = 0; m < nMels; ++m) {
        const double lower = melPoints[m];
        const double center = melPoints[m + 1];
        const double upper = melPoints[m + 2];
        // Slaney area normalization keeps per-filter energy comparable.
        const double enorm = 2.0 / (upper - lower);
        for (std::size_t k = 0; k < nBins; ++k) {
            const double freq = static_cast<double>(k) * sampleRate / static_cast<double>(nFft);
            const double rising = (freq - lower) / (center - lower);
            const double falling = (upper - freq) / (upper - center);
            bank.weights[m][k] = std::max(0.0, std::min(rising, falling)) * enorm;
        }
        std::size_t begin = 0;
        while (begin < nBins && bank.weights[m][begin] == 0.0) {
            ++begin;
        }
        std::size_t end = nBins;
        while (end > begin && bank.weights[m][end - 1] == 0.0) {
            --end;
        }
        const std::size_t offset = bank.sparseWeights.size();
        for (std::size_t k = begin; k < end; ++k) {
            bank.sparseWeights.push_back(bank.weights[m][k]);
        }
        bank.sparseSpans.push_back({begin, end, offset});
    }
    return bank;
}

std::vector<double> MelBank::apply(const std::vector<double> &powerFrame) const
{
    std::vector<double> out(weights.size(), 0.0);
    if (sparseSpans.size() == weights.size()) {
        for (std::size_t m = 0; m < sparseSpans.size(); ++m) {
            const SparseSpan &span = sparseSpans[m];
            double sum = 0.0;
            const std::size_t end = std::min(span.end, powerFrame.size());
            for (std::size_t k = span.begin; k < end; ++k) {
                sum += sparseWeights[span.offset + (k - span.begin)] * powerFrame[k];
            }
            out[m] = sum;
        }
        return out;
    }
    for (std::size_t m = 0; m < weights.size(); ++m) {
        double sum = 0.0;
        const std::vector<double> &filter = weights[m];
        for (std::size_t k = 0; k < filter.size() && k < powerFrame.size(); ++k) {
            sum += filter[k] * powerFrame[k];
        }
        out[m] = sum;
    }
    return out;
}

const MelBank &cachedMelBank(unsigned sampleRate)
{
    thread_local std::optional<std::pair<unsigned, MelBank>> cached;
    if (!cached || cached->first != sampleRate) {
        cached = std::make_pair(sampleRate, MelBank::slaney(kNMels, kNFft, static_cast<double>(sampleRate)));
    }
    return cached->second;
}

std::vector<double> onsetEnvelope(const PowerSpectrogram &spectrogram, const MelBank &melBank)
{
    std::vector<std::vector<double>> melDb;
    melDb.reserve(spectrogram.frames.size());
    double maxPower = kAmin;
    for (const std::vector<double> &frame : spectrogram.frames) {
        melDb.push_back(melBank.apply(frame));
        for (double p : melDb.back()) {
            maxPower = std::max(maxPower, p);
        }
    }
    const double floorDb = 10.0 * std::log10(maxPower) - kTopDb;
    for (std::vector<double> &frame : melDb) {
        for (double &p : frame) {
            p = std::max(10.0 * std::log10(std::max(p, kAmin)), floorDb);
        }
    }

    std::vector<double> envelope{0.0};
    for (std::size_t t = 1; t < melDb.size(); ++t) {
        double flux = 0.0;
        for (std::size_t m = 0; m < melDb[t].size(); ++m) {
            flux += std::max(melDb[t][m] - melDb[t - 1][m], 0.0);
        }
        envelope.push_back(flux / static_cast<double>(melDb[t].size()));
    }
    return envelope;
}

double onsetRate(const std::vector<double> &envelope, double frameRate)
{
    if (envelope.size() < 3 || frameRate <= 0.0) {
        return 0.0;
    }
    double globalMax = 0.0;
    for (double v : envelope) {
        globalMax = std::max(globalMax, v);
    }
    if (globalMax <= 0.0) {
        return 0.0;
    }

    const auto halfWindow = static_cast<std::size_t>(frameRate); // ~1 s of context each side
    const double delta = 0.05 * globalMax;
    std::size_t count = 0;
    for (std::size_t t = 1; t + 1 < envelope.size(); ++t) {
        if (envelope[t] <= envelope[t - 1] || envelope[t] < envelope[t + 1]) {
            continue;
        }
        const std::size_t lo = t > halfWindow ? t - halfWindow : 0;
        const std::size_t hi = std::min(t + halfWindow + 1, envelope.size());
        double localSum = 0.0;
        for (std::size_t i = lo; i < hi; ++i) {
            localSum += envelope[i];
        }
        const double localMean = localSum / static_cast<double>(hi - lo);
        if (envelope[t] > localMean + delta) {
            ++count;
        }
    }
    return static_cast<double>(count) / (static_cast<double>(envelope.size()) / frameRate);
}

std::optional<double> estimateBpm(const std::vector<double> &envelope, double frameRate)
{
    const auto window = static_cast<std::size_t>(kAcWindowSecs * frameRate);
    if (window < 8 || envelope.size() < window) {
        return std::nullopt;
    }
    if (std::ranges::all_of(envelope, [](double v) { return v <= 0.0; })) {
        return std::nullopt;
    }

    // Mean of per-window normalized autocorrelations over non-overlapping
    // windows (a windowed tempogram aggregated with mean; non-overlapping
    // windows are a documented-equivalent cheapening of the per-frame slide).
    std::vector<double> meanAc(window, 0.0);
    std::size_t windows = 0;
    for (std::size_t start = 0; start + window <= envelope.size(); start += window) {
        const std::vector<double> ac = autocorrelate(envelope.data() + start, window);
        double peak = std::numeric_limits<double>::min();
        for (double v : ac) {
            peak = std::max(peak, v);
        }
        for (std::size_t lag = 0; lag < window; ++lag) {
            meanAc[lag] += ac[lag] / peak;
        }
        ++windows;
    }
    if (windows == 0) {
        return std::nullopt;
    }
    for (double &v : meanAc) {
        v /= static_cast<double>(windows);
    }

    bool found = false;
    double bestScore = 0.0;
    double bestBpm = 0.0;
    for (std::size_t lag = 1; lag < meanAc.size(); ++lag) {
        const double bpm = 60.0 * frameRate / static_cast<double>(lag);
        if (bpm < kMinBpm || bpm >= kMaxBpm) {
            continue;
        }
        const double octaves = (std::log2(bpm) - std::log2(kStartBpm)) / kStdBpmOctaves;
        const double logPrior = -0.5 * octaves * octaves;
        // log1p compresses the autocorrelation into the prior's scale while
        // preserving peak ordering.
        const double score = std::log1p(1e6 * std::max(meanAc[lag], 0.0)) + logPrior;
        if (!found || score > bestScore) {
            found = true;
            bestScore = score;
            bestBpm = bpm;
        }
    }
    if (!found) {
        return std::nullopt;
    }
    return bestBpm;
}

Loudness gatedLoudness(const std::vector<float> &samples, double sampleRate)
{
    std::vector<double> weighted(samples.begin(), samples.end());
    applyBiquad(weighted, kWeightHighShelf(sampleRate));
    applyBiquad(weighted, kWeightHighPass(sampleRate));

    const auto blockLen = static_cast<std::size_t>(kBlockSecs * sampleRate);
    const auto step = static_cast<std::size_t>((1.0 - kBlockOverlap) * kBlockSecs * sampleRate);
    if (blockLen == 0 || step == 0 || weighted.size() < blockLen) {
        return {};
    }

    // Mean-square power z_j per 400 ms block (BS.1770 eq. 1), mono gain 1.0.
    std::vector<double> blockPower;
    for (std::size_t start = 0; start + blockLen <= weighted.size(); start += step) {
        double sumSq = 0.0;
        for (std::size_t i = start; i < start + blockLen; ++i) {
            sumSq += weighted[i] * weighted[i];
        }
        blockPower.push_back(sumSq / static_cast<double>(blockLen));
    }

    std::vector<double> blockLufs;
    blockLufs.reserve(blockPower.size());
    for (double z : blockPower) {
        blockLufs.push_back(kLufsOffset
                            + 10.0 * std::log10(std::max(z, std::numeric_limits<double>::min())));
    }

    // Absolute gate at -70 LUFS (eq. 5).
    std::vector<std::size_t> absGated;
    for (std::size_t j = 0; j < blockPower.size(); ++j) {
        if (blockLufs[j] >= kAbsoluteGateLufs) {
            absGated.push_back(j);
        }
    }
    if (absGated.empty()) {
        return {};
    }

    // Relative gate 10 LU under the abs-gated mean power (eq. 6).
    double absMean = 0.0;
    for (std::size_t j : absGated) {
        absMean += blockPower[j];
    }
    absMean /= static_cast<double>(absGated.size());
    const double relativeGate = kLufsOffset + 10.0 * std::log10(absMean) + kRelativeGateLu;

    double gatedMean = 0.0;
    std::size_t gatedCount = 0;
    for (std::size_t j : absGated) {
        if (blockLufs[j] > relativeGate) {
            gatedMean += blockPower[j];
            ++gatedCount;
        }
    }
    if (gatedCount == 0) {
        return {};
    }
    gatedMean /= static_cast<double>(gatedCount);

    double meanLufs = 0.0;
    for (std::size_t j : absGated) {
        meanLufs += blockLufs[j];
    }
    meanLufs /= static_cast<double>(absGated.size());
    double variance = 0.0;
    for (std::size_t j : absGated) {
        const double d = blockLufs[j] - meanLufs;
        variance += d * d;
    }
    variance /= static_cast<double>(absGated.size());

    return {
        kLufsOffset + 10.0 * std::log10(gatedMean),
        std::sqrt(variance),
    };
}

SpectralStats spectralStats(const PowerSpectrogram &spectrogram, double sampleRate)
{
    const std::size_t bins = spectrogram.bins();
    std::vector<double> binFreqs(bins);
    for (std::size_t k = 0; k < bins; ++k) {
        binFreqs[k] = static_cast<double>(k) * sampleRate / static_cast<double>(spectrogram.nFft);
    }

    std::vector<double> centroids;
    double flatnessSum = 0.0;
    std::size_t flatnessFrames = 0;

    for (const std::vector<double> &frame : spectrogram.frames) {
        double total = 0.0;
        for (double p : frame) {
            total += p;
        }
        if (total < kSilentFramePower) {
            continue;
        }

        double weightedFreq = 0.0;
        double logSum = 0.0;
        for (std::size_t k = 0; k < frame.size(); ++k) {
            weightedFreq += frame[k] * binFreqs[k];
            logSum += std::log(std::max(frame[k], kAmin));
        }
        centroids.push_back(weightedFreq / total);

        const double logMean = logSum / static_cast<double>(frame.size());
        const double arithMean = total / static_cast<double>(frame.size());
        flatnessSum += std::exp(logMean) / std::max(arithMean, kAmin);
        ++flatnessFrames;
    }

    if (centroids.empty()) {
        return {};
    }

    double mean = 0.0;
    for (double c : centroids) {
        mean += c;
    }
    mean /= static_cast<double>(centroids.size());
    double variance = 0.0;
    for (double c : centroids) {
        variance += (c - mean) * (c - mean);
    }
    variance /= static_cast<double>(centroids.size());

    return {
        mean,
        std::sqrt(variance),
        flatnessSum / static_cast<double>(flatnessFrames),
    };
}

double zeroCrossingRate(const std::vector<float> &samples)
{
    if (samples.size() < 2) {
        return 0.0;
    }
    std::size_t crossings = 0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        if ((samples[i - 1] >= 0.0F) != (samples[i] >= 0.0F)) {
            ++crossings;
        }
    }
    return static_cast<double>(crossings) / static_cast<double>(samples.size() - 1);
}

std::optional<ScalarFeatures> analyze(const std::vector<float> &samples, unsigned sampleRate)
{
    const double rate = sampleRate;
    if (static_cast<double>(samples.size()) < kMinDurationSecs * rate) {
        return std::nullopt;
    }

    const PowerSpectrogram spectrogram = powerSpectrogram(samples, kNFft, kHop);
    if (spectrogram.frames.empty()) {
        return std::nullopt;
    }

    const MelBank &melBank = cachedMelBank(sampleRate);
    const std::vector<double> envelope = onsetEnvelope(spectrogram, melBank);
    const double frameRate = rate / static_cast<double>(kHop);

    ScalarFeatures features;
    features.tempoBpm = estimateBpm(envelope, frameRate);
    features.onsetRateHz = onsetRate(envelope, frameRate);

    const Loudness loudness = gatedLoudness(samples, rate);
    features.loudnessLufs = loudness.integratedLufs;
    features.loudnessStdDb = loudness.blockStdDb;

    const SpectralStats spectral = spectralStats(spectrogram, rate);
    features.spectralCentroidMeanHz = spectral.centroidMeanHz;
    features.spectralCentroidStdHz = spectral.centroidStdHz;
    features.spectralFlatnessMean = spectral.flatnessMean;
    features.zeroCrossingRate = zeroCrossingRate(samples);

    if (loudness.integratedLufs) {
        features.energy = energyV1(*loudness.integratedLufs, features.onsetRateHz);
    }
    return features;
}

} // namespace Dsp
