// DSP stage benchmark and v1 oracle printer (dev tool; built by default,
// deliberately NOT registered with CTest — timing under `make test` load
// would be meaningless and the goldens live in test_dsp).
//
// Modes:
//   bench_dsp [--seconds N] [--reps N]   stage timings + peak RSS
//   bench_dsp --goldens                  print scalar fields at full
//                                        precision for the deterministic
//                                        oracle fixtures (diff this output
//                                        across refactors to prove
//                                        bit-identical behavior; the shipped
//                                        tolerance test is test_dsp's
//                                        v1GoldenScalarsMatchPinnedOracle)
//
// Fixture generators are copied verbatim from tests/test_dsp.cpp — the
// golden test regenerates the same audio, so the two must stay identical.

#include "indexer/Dsp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include <sys/resource.h>

namespace {

// Deterministic xorshift noise in [-1, 1]; identical to test_dsp.cpp.
std::vector<float> makeNoise(std::size_t len, unsigned long long seed)
{
    unsigned long long state = std::max<unsigned long long>(seed, 1);
    std::vector<float> out;
    out.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const double unit = static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
        out.push_back(static_cast<float>(unit * 2.0 - 1.0));
    }
    return out;
}

std::vector<float> makeSine(double freqHz, double amplitude, double secs)
{
    const auto n = static_cast<std::size_t>(secs * Dsp::kSampleRateHz);
    std::vector<float> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / Dsp::kSampleRateHz;
        out.push_back(static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * freqHz * t)));
    }
    return out;
}

std::vector<float> makeClicks(double bpm, double secs)
{
    const auto n = static_cast<std::size_t>(secs * Dsp::kSampleRateHz);
    const auto period = static_cast<std::size_t>(std::round(60.0 / bpm * Dsp::kSampleRateHz));
    std::vector<float> out(n, 0.0F);
    for (std::size_t i = 0; i < n; i += period) {
        out[i] = 1.0F;
    }
    return out;
}

// Music-like deterministic mix: beat + low/high tones + noise bed. Used both
// as the timing fixture (long) and a golden fixture (short).
std::vector<float> makeMix(double secs)
{
    const std::vector<float> clicks = makeClicks(123.0, secs);
    const std::vector<float> low = makeSine(220.0, 0.3, secs);
    const std::vector<float> high = makeSine(3520.0, 0.1, secs);
    const std::vector<float> bed = makeNoise(clicks.size(), 42);
    std::vector<float> out(clicks.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = 0.5F * clicks[i] + low[i] + high[i] + 0.05F * bed[i];
    }
    return out;
}

double msSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

long peakRssMib()
{
    struct rusage usage = {};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024; // ru_maxrss is KiB on Linux
}

void printField(const char *name, const std::optional<double> &value)
{
    if (value) {
        std::printf(" %s=%.17g", name, *value);
    } else {
        std::printf(" %s=none", name);
    }
}

void printGolden(const char *fixture, const std::vector<float> &samples)
{
    const std::optional<Dsp::ScalarFeatures> features = Dsp::analyze(samples, Dsp::kSampleRateHz);
    std::printf("fixture=%s", fixture);
    if (!features) {
        std::printf(" no_features\n");
        return;
    }
    printField("tempo_bpm", features->tempoBpm);
    printField("loudness_lufs", features->loudnessLufs);
    printField("loudness_std_db", features->loudnessStdDb);
    std::printf(" centroid_mean_hz=%.17g", features->spectralCentroidMeanHz);
    std::printf(" centroid_std_hz=%.17g", features->spectralCentroidStdHz);
    std::printf(" flatness_mean=%.17g", features->spectralFlatnessMean);
    std::printf(" zcr=%.17g", features->zeroCrossingRate);
    std::printf(" onset_rate_hz=%.17g", features->onsetRateHz);
    printField("energy", features->energy);
    std::printf("\n");
}

// Tone, hard silence gap, tone: the only fixture whose frame stream mixes
// silent and non-silent frames, pinning the kSilentFramePower skip path.
std::vector<float> makeGappedTone()
{
    const std::vector<float> tone = makeSine(880.0, 0.6, 4.0);
    std::vector<float> out(tone);
    out.insert(out.end(), Dsp::kSampleRateHz * 2, 0.0F);
    out.insert(out.end(), tone.begin(), tone.end());
    return out;
}

int runGoldens()
{
    printGolden("silence-10s", std::vector<float>(Dsp::kSampleRateHz * 10, 0.0F));
    printGolden("tone-gap-tone-10s", makeGappedTone());
    printGolden("sine440-10s", makeSine(440.0, 0.5, 10.0));
    std::vector<float> quiet = makeNoise(Dsp::kSampleRateHz * 10, 7);
    for (float &sample : quiet) {
        sample *= 0.01F;
    }
    printGolden("quiet-noise-10s", quiet);
    printGolden("noise-10s", makeNoise(Dsp::kSampleRateHz * 10, 7));
    printGolden("clicks120-30s", makeClicks(120.0, 30.0));
    printGolden("mix-30s", makeMix(30.0));
    printGolden("tooshort-1s", makeSine(440.0, 0.5, 1.0));
    return 0;
}

struct StageTimes {
    double melBankMs = 0.0;
    double stftMs = 0.0;
    double melOnsetMs = 0.0;
    double tempoMs = 0.0;
    double loudnessMs = 0.0;
    double spectralZcrMs = 0.0;
    double analyzeMs = 0.0;
};

StageTimes timeStages(const std::vector<float> &samples)
{
    // Mirrors the Dsp::analyze pipeline through the public building blocks so
    // each bucket can be timed; nFft/hop/nMels match the analyzer constants.
    constexpr std::size_t kNFft = 2'048;
    constexpr std::size_t kHop = 512;
    StageTimes times;

    auto start = std::chrono::steady_clock::now();
    const Dsp::MelBank bank = Dsp::MelBank::slaney(128, kNFft, Dsp::kSampleRateHz);
    times.melBankMs = msSince(start);

    start = std::chrono::steady_clock::now();
    const Dsp::PowerSpectrogram spectrogram = Dsp::powerSpectrogram(samples, kNFft, kHop);
    times.stftMs = msSince(start);

    start = std::chrono::steady_clock::now();
    const std::vector<double> envelope = Dsp::onsetEnvelope(spectrogram, bank);
    times.melOnsetMs = msSince(start);

    const double frameRate = static_cast<double>(Dsp::kSampleRateHz) / kHop;
    start = std::chrono::steady_clock::now();
    (void)Dsp::estimateBpm(envelope, frameRate);
    (void)Dsp::onsetRate(envelope, frameRate);
    times.tempoMs = msSince(start);

    start = std::chrono::steady_clock::now();
    (void)Dsp::gatedLoudness(samples, Dsp::kSampleRateHz);
    times.loudnessMs = msSince(start);

    start = std::chrono::steady_clock::now();
    (void)Dsp::spectralStats(spectrogram, Dsp::kSampleRateHz);
    (void)Dsp::zeroCrossingRate(samples);
    times.spectralZcrMs = msSince(start);

    start = std::chrono::steady_clock::now();
    (void)Dsp::analyze(samples, Dsp::kSampleRateHz);
    times.analyzeMs = msSince(start);

    return times;
}

int runBench(double seconds, int reps)
{
    std::printf("bench_dsp: %.0f s mixed synthetic track at %u Hz, %d rep(s)\n", seconds,
                Dsp::kSampleRateHz, reps);
    const std::vector<float> samples = makeMix(seconds);

    StageTimes best;
    best.analyzeMs = -1.0;
    for (int rep = 0; rep < reps; ++rep) {
        const StageTimes times = timeStages(samples);
        std::printf("rep %d: stft=%.0fms mel+onset=%.0fms tempo=%.0fms loudness=%.0fms "
                    "spectral+zcr=%.0fms melbank=%.0fms analyze=%.0fms\n",
                    rep, times.stftMs, times.melOnsetMs, times.tempoMs, times.loudnessMs,
                    times.spectralZcrMs, times.melBankMs, times.analyzeMs);
        if (best.analyzeMs < 0.0 || times.analyzeMs < best.analyzeMs) {
            best = times;
        }
    }

    const double stageSum =
        best.stftMs + best.melOnsetMs + best.tempoMs + best.loudnessMs + best.spectralZcrMs;
    std::printf("\nbest-of-%d (stage buckets, %.0f s track):\n", reps, seconds);
    const auto row = [stageSum](const char *name, double ms) {
        std::printf("  %-14s %8.1f ms  %5.1f%%\n", name, ms,
                    stageSum > 0.0 ? 100.0 * ms / stageSum : 0.0);
    };
    row("stft", best.stftMs);
    row("mel+onset", best.melOnsetMs);
    row("tempo", best.tempoMs);
    row("loudness", best.loudnessMs);
    row("spectral+zcr", best.spectralZcrMs);
    std::printf("  %-14s %8.1f ms  (one-time, thread-cached in production)\n", "melbank",
                best.melBankMs);
    std::printf("  %-14s %8.1f ms  (whole pipeline, one call)\n", "analyze", best.analyzeMs);
    std::printf("  peak RSS %ld MiB\n", peakRssMib());
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    double seconds = 240.0;
    int reps = 3;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--goldens") == 0) {
            return runGoldens();
        }
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
            reps = std::max(1, std::atoi(argv[++i]));
        } else {
            std::fprintf(stderr, "usage: %s [--goldens] [--seconds N] [--reps N]\n", argv[0]);
            return 2;
        }
    }
    return runBench(seconds, reps);
}
