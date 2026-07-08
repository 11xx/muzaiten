#include "indexer/Dsp.h"

#include <QTest>

#include <cmath>
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

QTEST_APPLESS_MAIN(DspTest)
#include "test_dsp.moc"
