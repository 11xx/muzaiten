#include "indexer/Dsp.h"
#include "indexer/Fft.h"

#include <QTest>

#include <array>
#include <cmath>
#include <complex>
#include <numbers>

// Ported 1:1 from the Rust original's unit tests (archived at
// ~/code/muzaiten-dsp-rs); the C++ implementation reproduced the Rust
// output on a pink-noise fixture to 9 decimal places at port time.

namespace {

// Deterministic xorshift noise in [-1, 1]; no external RNG dependency.
std::vector<float> makeNoise(std::size_t len, quint64 seed)
{
    quint64 state = std::max<quint64>(seed, 1);
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

// Impulse train at the given tempo: a 1-sample click every beat.
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

// Beats land at fractional frame positions, so split each impulse between
// the two neighboring frames the way a real (smooth) onset envelope spreads
// its peaks.
std::vector<double> impulseEnvelope(double bpm, double frameRate, double secs)
{
    const auto frames = static_cast<std::size_t>(secs * frameRate);
    const double period = 60.0 / bpm * frameRate;
    std::vector<double> envelope(frames, 0.0);
    for (double beat = 0.0; static_cast<std::size_t>(std::floor(beat)) + 1 < frames;
         beat += period) {
        const auto index = static_cast<std::size_t>(std::floor(beat));
        const double fraction = beat - std::floor(beat);
        envelope[index] += 1.0 - fraction;
        envelope[index + 1] += fraction;
    }
    return envelope;
}

constexpr double kFrameRate = static_cast<double>(Dsp::kSampleRateHz) / 512.0;

// Music-like deterministic mix; kept byte-identical with tests/bench_dsp.cpp
// so the bench's --goldens output and this file's oracle describe the same
// audio.
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

// Tone, hard silence gap, tone; kept byte-identical with bench_dsp.cpp. The
// only fixture whose frame stream mixes silent and non-silent frames, so it
// pins the kSilentFramePower skip path the other fixtures never branch on.
std::vector<float> makeGappedTone()
{
    const std::vector<float> tone = makeSine(880.0, 0.6, 4.0);
    std::vector<float> out(tone);
    out.insert(out.end(), Dsp::kSampleRateHz * 2, 0.0F);
    out.insert(out.end(), tone.begin(), tone.end());
    return out;
}

// The v1 oracle uses one shared tolerance: tight enough that any semantic
// change to the analyzer trips it, loose enough to absorb FMA/libm noise
// across build regimes. Values near zero (cancellation residue) collapse
// into the absolute floor by design.
bool nearGolden(double actual, double expected)
{
    return std::abs(actual - expected) <= std::max(1e-9, 1e-9 * std::abs(expected));
}

bool nearGolden(const std::optional<double> &actual, const std::optional<double> &expected)
{
    if (actual.has_value() != expected.has_value()) {
        return false;
    }
    return !expected.has_value() || nearGolden(*actual, *expected);
}

struct GoldenRow {
    const char *fixture;
    std::optional<double> tempoBpm;
    std::optional<double> loudnessLufs;
    std::optional<double> loudnessStdDb;
    double centroidMeanHz;
    double centroidStdHz;
    double flatnessMean;
    double zcr;
    double onsetRateHz;
    std::optional<double> energy;
};

} // namespace

class DspTest : public QObject
{
    Q_OBJECT

private slots:
    void tooShortInputYieldsNothing();
    void silenceHasNoLoudnessEnergyOrTempo();
    void loudDenseAudioOutranksQuietSparseAudioOnEnergy();
    void clickTrackTempoIsRecovered();
    void clickTrackOnsetRateMatchesClickDensity();
    void sineEnergyLandsInTheMatchingStftBin();
    void melFiltersAreNonnegativeAndCoverTheBand();
    void sparseMelApplyMatchesDenseReferenceExactly();
    void fullScaleSineLoudnessIsNearTheoreticalValue();
    void halvingAmplitudeDropsLoudnessSixDb();
    void steadyToneHasNegligibleBlockSpread();
    void tempoRecoveredAcrossTheCommonRange();
    void extremeTempoIsRecoveredUpToOctaveEquivalence();
    void emptyOrFlatEnvelopeHasNoTempo();
    void flatEnvelopeCountsNoOnsets();
    void sineCentroidSitsAtItsFrequencyAndIsTonal();
    void noiseIsFlatterAndBrighterThanALowSine();
    void sineZeroCrossingRateTracksFrequency();
    void cleanRoomRealFftMatchesV1Reference();
    void v1GoldenScalarsMatchPinnedOracle();
};

void DspTest::tooShortInputYieldsNothing()
{
    QVERIFY(!Dsp::analyze(makeSine(440.0, 0.5, 1.0), Dsp::kSampleRateHz).has_value());
}

void DspTest::silenceHasNoLoudnessEnergyOrTempo()
{
    const auto features =
        Dsp::analyze(std::vector<float>(Dsp::kSampleRateHz * 10, 0.0F), Dsp::kSampleRateHz);
    QVERIFY(features.has_value());
    QVERIFY(!features->loudnessLufs.has_value());
    QVERIFY(!features->energy.has_value());
    QVERIFY(!features->tempoBpm.has_value());
    QCOMPARE(features->onsetRateHz, 0.0);
}

void DspTest::loudDenseAudioOutranksQuietSparseAudioOnEnergy()
{
    const auto loud = Dsp::analyze(makeNoise(Dsp::kSampleRateHz * 10, 7), Dsp::kSampleRateHz);
    std::vector<float> quietSamples = makeNoise(Dsp::kSampleRateHz * 10, 7);
    for (float &sample : quietSamples) {
        sample *= 0.01F;
    }
    const auto quiet = Dsp::analyze(quietSamples, Dsp::kSampleRateHz);
    QVERIFY(loud.has_value() && quiet.has_value());
    QVERIFY(loud->energy.value() > quiet->energy.value());
}

void DspTest::clickTrackTempoIsRecovered()
{
    for (double bpm : {90.0, 120.0, 150.0}) {
        const auto features = Dsp::analyze(makeClicks(bpm, 30.0), Dsp::kSampleRateHz);
        QVERIFY(features.has_value());
        QVERIFY(features->tempoBpm.has_value());
        QVERIFY2(std::abs(*features->tempoBpm - bpm) <= 3.0,
                 qPrintable(QStringLiteral("expected ~%1 BPM, estimated %2")
                                .arg(bpm)
                                .arg(*features->tempoBpm)));
    }
}

void DspTest::clickTrackOnsetRateMatchesClickDensity()
{
    const auto features = Dsp::analyze(makeClicks(120.0, 30.0), Dsp::kSampleRateHz);
    QVERIFY(features.has_value());
    QVERIFY2(std::abs(features->onsetRateHz - 2.0) <= 0.4,
             qPrintable(QStringLiteral("120 BPM clicks are 2 onsets/s, got %1")
                            .arg(features->onsetRateHz)));
}

void DspTest::sineEnergyLandsInTheMatchingStftBin()
{
    const Dsp::PowerSpectrogram spec = Dsp::powerSpectrogram(makeSine(1000.0, 0.8, 2.0), 2048, 512);
    const auto expectedBin =
        static_cast<std::size_t>(std::round(1000.0 / (22050.0 / 2048.0)));

    const std::vector<double> &mid = spec.frames.at(spec.frames.size() / 2);
    std::size_t peakBin = 0;
    for (std::size_t k = 1; k < mid.size(); ++k) {
        if (mid[k] > mid[peakBin]) {
            peakBin = k;
        }
    }
    QVERIFY2(peakBin >= expectedBin - 1 && peakBin <= expectedBin + 1,
             qPrintable(QStringLiteral("peak bin %1, expected ~%2").arg(peakBin).arg(expectedBin)));
}

void DspTest::melFiltersAreNonnegativeAndCoverTheBand()
{
    const Dsp::MelBank bank = Dsp::MelBank::slaney(128, 2048, 22050.0);
    QCOMPARE(bank.weights.size(), static_cast<std::size_t>(128));
    for (const std::vector<double> &filter : bank.weights) {
        bool anyPositive = false;
        for (double w : filter) {
            QVERIFY(w >= 0.0);
            anyPositive = anyPositive || w > 0.0;
        }
        QVERIFY2(anyPositive, "empty mel filter");
    }
}

void DspTest::sparseMelApplyMatchesDenseReferenceExactly()
{
    const Dsp::MelBank bank = Dsp::MelBank::slaney(128, 2048, 22050.0);
    const Dsp::PowerSpectrogram spec = Dsp::powerSpectrogram(makeNoise(Dsp::kSampleRateHz * 5, 11), 2048, 512);
    QVERIFY(!spec.frames.empty());

    const std::vector<double> &frame = spec.frames.at(spec.frames.size() / 2);
    std::vector<double> dense(bank.weights.size(), 0.0);
    for (std::size_t m = 0; m < bank.weights.size(); ++m) {
        const std::vector<double> &filter = bank.weights[m];
        double sum = 0.0;
        for (std::size_t k = 0; k < filter.size() && k < frame.size(); ++k) {
            sum += filter[k] * frame[k];
        }
        dense[m] = sum;
    }

    const std::vector<double> sparse = bank.apply(frame);
    QCOMPARE(sparse.size(), dense.size());
    for (std::size_t i = 0; i < dense.size(); ++i) {
        QCOMPARE(sparse[i], dense[i]);
    }
}

void DspTest::fullScaleSineLoudnessIsNearTheoreticalValue()
{
    // Mean square of a full-scale sine is 0.5 -> 10log10(0.5) = -3.01, plus
    // the -0.691 offset; K-weighting is near-unity around 1 kHz.
    const Dsp::Loudness loudness = Dsp::gatedLoudness(makeSine(997.0, 1.0, 10.0), 22050.0);
    QVERIFY(loudness.integratedLufs.has_value());
    QVERIFY2(*loudness.integratedLufs >= -4.5 && *loudness.integratedLufs <= -2.8,
             qPrintable(QStringLiteral("got %1 LUFS").arg(*loudness.integratedLufs)));
}

void DspTest::halvingAmplitudeDropsLoudnessSixDb()
{
    const Dsp::Loudness loud = Dsp::gatedLoudness(makeSine(997.0, 0.8, 10.0), 22050.0);
    const Dsp::Loudness soft = Dsp::gatedLoudness(makeSine(997.0, 0.4, 10.0), 22050.0);
    const double delta = *loud.integratedLufs - *soft.integratedLufs;
    QVERIFY2(std::abs(delta - 6.02) < 0.1, qPrintable(QStringLiteral("delta %1").arg(delta)));
}

void DspTest::steadyToneHasNegligibleBlockSpread()
{
    const Dsp::Loudness loudness = Dsp::gatedLoudness(makeSine(997.0, 0.5, 10.0), 22050.0);
    QVERIFY(loudness.blockStdDb.value() < 0.2);
}

void DspTest::tempoRecoveredAcrossTheCommonRange()
{
    for (double bpm : {70.0, 90.0, 120.0, 150.0}) {
        const auto estimate = Dsp::estimateBpm(impulseEnvelope(bpm, kFrameRate, 60.0), kFrameRate);
        QVERIFY(estimate.has_value());
        QVERIFY2(std::abs(*estimate - bpm) <= 3.0,
                 qPrintable(QStringLiteral("expected ~%1, got %2").arg(bpm).arg(*estimate)));
    }
}

void DspTest::extremeTempoIsRecoveredUpToOctaveEquivalence()
{
    // Past ~160 BPM the 120-centered log-normal prior may fold an estimate
    // onto its half octave — the documented, expected behavior of a global
    // tempo estimator. Pin that contract rather than exact recovery.
    const auto estimate = Dsp::estimateBpm(impulseEnvelope(180.0, kFrameRate, 60.0), kFrameRate);
    QVERIFY(estimate.has_value());
    const double octaveError = std::abs(std::log2(*estimate) - std::log2(180.0));
    const double folded = std::abs(octaveError - 1.0);
    QVERIFY2(std::min(octaveError, folded) < 0.05,
             qPrintable(QStringLiteral("expected 180 or 90 BPM, got %1").arg(*estimate)));
}

void DspTest::emptyOrFlatEnvelopeHasNoTempo()
{
    QVERIFY(!Dsp::estimateBpm({}, kFrameRate).has_value());
    QVERIFY(!Dsp::estimateBpm(std::vector<double>(2000, 0.0), kFrameRate).has_value());
}

void DspTest::flatEnvelopeCountsNoOnsets()
{
    QCOMPARE(Dsp::onsetRate(std::vector<double>(500, 0.0), 43.0), 0.0);
    QCOMPARE(Dsp::onsetRate(std::vector<double>(500, 1.0), 43.0), 0.0);
}

void DspTest::sineCentroidSitsAtItsFrequencyAndIsTonal()
{
    const Dsp::PowerSpectrogram spec = Dsp::powerSpectrogram(makeSine(1000.0, 0.7, 5.0), 2048, 512);
    const Dsp::SpectralStats stats = Dsp::spectralStats(spec, 22050.0);
    QVERIFY2(std::abs(stats.centroidMeanHz - 1000.0) < 120.0,
             qPrintable(QStringLiteral("centroid %1").arg(stats.centroidMeanHz)));
    QVERIFY2(stats.flatnessMean < 0.05,
             qPrintable(QStringLiteral("flatness %1").arg(stats.flatnessMean)));
}

void DspTest::noiseIsFlatterAndBrighterThanALowSine()
{
    const Dsp::SpectralStats noiseStats = Dsp::spectralStats(
        Dsp::powerSpectrogram(makeNoise(Dsp::kSampleRateHz * 5, 3), 2048, 512), 22050.0);
    const Dsp::SpectralStats sineStats = Dsp::spectralStats(
        Dsp::powerSpectrogram(makeSine(200.0, 0.7, 5.0), 2048, 512), 22050.0);
    QVERIFY(noiseStats.flatnessMean > 0.2);
    QVERIFY(noiseStats.centroidMeanHz > sineStats.centroidMeanHz);
}

void DspTest::sineZeroCrossingRateTracksFrequency()
{
    // A sine at f crosses zero 2f times per second: rate ~ 2f / sr.
    const double rate = Dsp::zeroCrossingRate(makeSine(1000.0, 0.7, 5.0));
    const double expected = 2.0 * 1000.0 / Dsp::kSampleRateHz;
    QVERIFY2(std::abs(rate - expected) < 0.1 * expected,
             qPrintable(QStringLiteral("zcr %1").arg(rate)));
}

void DspTest::cleanRoomRealFftMatchesV1Reference()
{
    using Frame = std::array<float, Fft::RealFft2048::kSize>;
    std::vector<std::pair<QString, Frame>> fixtures;

    fixtures.emplace_back(QStringLiteral("silence"), Frame{});

    Frame firstImpulse{};
    firstImpulse.front() = 1.0F;
    fixtures.emplace_back(QStringLiteral("first-impulse"), firstImpulse);

    Frame lastImpulse{};
    lastImpulse.back() = 1.0F;
    fixtures.emplace_back(QStringLiteral("last-impulse"), lastImpulse);

    Frame dc{};
    dc.fill(0.25F);
    fixtures.emplace_back(QStringLiteral("dc"), dc);

    Frame nyquist{};
    for (std::size_t i = 0; i < nyquist.size(); ++i) {
        nyquist[i] = i % 2 == 0 ? 0.5F : -0.5F;
    }
    fixtures.emplace_back(QStringLiteral("nyquist"), nyquist);

    Frame mixed{};
    const std::vector<float> noise = makeNoise(mixed.size(), 73);
    for (std::size_t i = 0; i < mixed.size(); ++i) {
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(i) /
                             static_cast<double>(mixed.size());
        mixed[i] = static_cast<float>(0.7 * std::sin(43.0 * phase) +
                                      0.2 * std::cos(317.0 * phase) + 0.03 * noise[i]);
    }
    fixtures.emplace_back(QStringLiteral("mixed"), mixed);

    Fft::RealFft2048 plan;
    Fft::RealFft2048::Workspace workspace;
    std::array<Fft::ComplexFloat, Fft::RealFft2048::kBins> output{};
    std::array<double, Fft::RealFft2048::kBins> power{};

    for (const auto &[name, input] : fixtures) {
        std::vector<std::complex<double>> reference(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            reference[i] = {static_cast<double>(input[i]), 0.0};
        }
        Fft::complexRadix2Reference(reference);
        plan.forward(input, output, workspace);
        plan.forwardPower(input, power, workspace);

        for (std::size_t bin = 0; bin < output.size(); ++bin) {
            const std::complex<double> candidate(output[bin].real, output[bin].imag);
            const double binScale = std::max(1.0, std::abs(reference[bin]));
            QVERIFY2(std::abs(candidate - reference[bin]) <= 5e-4 * binScale,
                     qPrintable(QStringLiteral("%1 bin %2 candidate=(%3,%4) reference=(%5,%6)")
                                    .arg(name)
                                    .arg(bin)
                                    .arg(candidate.real(), 0, 'g', 12)
                                    .arg(candidate.imag(), 0, 'g', 12)
                                    .arg(reference[bin].real(), 0, 'g', 12)
                                    .arg(reference[bin].imag(), 0, 'g', 12)));
            const double referencePower = std::norm(reference[bin]);
            QVERIFY2(std::abs(power[bin] - referencePower) <= 1e-3 * std::max(1.0, referencePower),
                     qPrintable(QStringLiteral("%1 power bin %2 candidate=%3 reference=%4")
                                    .arg(name)
                                    .arg(bin)
                                    .arg(power[bin], 0, 'g', 12)
                                    .arg(referencePower, 0, 'g', 12)));
        }
    }
}

void DspTest::v1GoldenScalarsMatchPinnedOracle()
{
    // muzaiten-dsp-v1 oracle, captured 2026-07-09 at master@8e46549 via
    // `bench_dsp --goldens` (RelWithDebInfo, generic x86-64, gcc, no FMA
    // contraction pinned yet). Regime-scoped reference: bit-exactness across
    // refactors is proven by diffing the bench's full-precision output; this
    // test guards against *semantic* drift with a tolerance that absorbs
    // FMA/libm noise. Never widen a tolerance to pass — a failure here means
    // scalar meaning moved and the DSP version must say so.
    std::vector<float> quietNoise = makeNoise(Dsp::kSampleRateHz * 10, 7);
    for (float &sample : quietNoise) {
        sample *= 0.01F;
    }
    const std::vector<std::pair<GoldenRow, std::vector<float>>> cases = {
        {{"silence-10s", std::nullopt, std::nullopt, std::nullopt, 0.0, 0.0, 0.0, 0.0, 0.0,
          std::nullopt},
         std::vector<float>(Dsp::kSampleRateHz * 10, 0.0F)},
        {{"tone-gap-tone-10s", 30.046329941860463, -7.8136234982408661, 4.7033343975730224,
          879.78140147729061, 2.1343771918360384, 2.6010183789833646e-05,
          0.063850629708071233, 0.1998441125290023, 0.55705047087044957},
         makeGappedTone()},
        {{"sine440-10s", 112.34714673913044, -9.6644690533454369, 0.00017316047801089145,
          439.97390961613837, 0.34491538285773432, 6.8534369758426706e-07,
          0.039904942879559542, 0.0, 0.50671061893309133},
         makeSine(440.0, 0.5, 10.0)},
        {{"quiet-noise-10s", 123.046875, -41.847875202932698, 0.038684863534371436,
          5498.6712804053541, 134.40748654491614, 0.56008263486076393, 0.49851019732515794,
          9.7923615139211133, 0.40000000000000002},
         quietNoise},
        {{"noise-10s", 123.046875, -1.8478750090323097, 0.038684864062500948,
          5498.6712803356886, 134.4074865790883, 0.56008263477821707, 0.49851019732515794,
          9.7923615139211133, 1.0},
         makeNoise(Dsp::kSampleRateHz * 10, 7)},
        {{"clicks120-30s", 117.45383522727273, -36.523361991074935, 4.2632564145606011e-14,
          5512.5000000000118, 6.8923732343424907e-11, 1.0000000000000036, 0.0,
          1.9666547745743035, 0.13111031830495357},
         makeClicks(120.0, 30.0)},
        {{"mix-30s", 123.046875, -13.067526800504618, 0.011740588257490931,
          633.21220193502609, 10.671009353426417, 0.010179760571680022,
          0.073301698112922323, 7.5332877805727554, 0.8386494639899077},
         makeMix(30.0)},
    };

    for (const auto &[golden, samples] : cases) {
        const auto features = Dsp::analyze(samples, Dsp::kSampleRateHz);
        QVERIFY2(features.has_value(), golden.fixture);
        const auto check = [&](const char *field, bool ok) {
            QVERIFY2(ok, qPrintable(QStringLiteral("%1: %2 moved off the v1 oracle")
                                        .arg(QLatin1String(golden.fixture), QLatin1String(field))));
        };
        check("tempo_bpm", nearGolden(features->tempoBpm, golden.tempoBpm));
        check("loudness_lufs", nearGolden(features->loudnessLufs, golden.loudnessLufs));
        check("loudness_std_db", nearGolden(features->loudnessStdDb, golden.loudnessStdDb));
        check("centroid_mean_hz", nearGolden(features->spectralCentroidMeanHz, golden.centroidMeanHz));
        check("centroid_std_hz", nearGolden(features->spectralCentroidStdHz, golden.centroidStdHz));
        check("flatness_mean", nearGolden(features->spectralFlatnessMean, golden.flatnessMean));
        check("zcr", nearGolden(features->zeroCrossingRate, golden.zcr));
        check("onset_rate_hz", nearGolden(features->onsetRateHz, golden.onsetRateHz));
        check("energy", nearGolden(features->energy, golden.energy));
    }

    // Short-input contract stays pinned alongside the numeric oracle.
    QVERIFY(!Dsp::analyze(makeSine(440.0, 0.5, 1.0), Dsp::kSampleRateHz).has_value());
}

QTEST_APPLESS_MAIN(DspTest)
#include "test_dsp.moc"
