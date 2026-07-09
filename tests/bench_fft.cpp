// Same-process comparison of the v1 complex<double> FFT and muzaiten's
// first-party fixed-2048 packed real FFT. Includes input packing and output
// power so the ratio reflects analyzer-call overhead, not an isolated kernel.

#include "indexer/Fft.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::array<float, Fft::RealFft2048::kSize> makeFixture()
{
    std::array<float, Fft::RealFft2048::kSize> input{};
    unsigned long long state = 42;
    for (std::size_t i = 0; i < input.size(); ++i) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        const double noise = static_cast<double>(state >> 11U) / static_cast<double>(1ULL << 53U) * 2.0 - 1.0;
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(i) /
                             static_cast<double>(input.size());
        input[i] = static_cast<float>(0.6 * std::sin(37.0 * phase) +
                                      0.25 * std::cos(311.0 * phase) + 0.05 * noise);
    }
    return input;
}

struct Measurement {
    double nsPerTransform = 0.0;
    double checksum = 0.0;
};

template<typename Transform>
Measurement measure(int repetitions, Transform transform)
{
    const auto started = Clock::now();
    double checksum = 0.0;
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        checksum += transform(static_cast<std::size_t>(repetition));
    }
    const double elapsedNs =
        std::chrono::duration<double, std::nano>(Clock::now() - started).count();
    return {elapsedNs / static_cast<double>(repetitions), checksum};
}

int printBins()
{
    const auto input = makeFixture();
    Fft::RealFft2048 plan;
    Fft::RealFft2048::Workspace workspace;
    std::array<Fft::ComplexFloat, Fft::RealFft2048::kBins> output{};
    std::array<double, Fft::RealFft2048::kBins> power{};
    plan.forward(input, output, workspace);
    plan.forwardPower(input, power, workspace);
    for (std::size_t bin = 0; bin < output.size(); ++bin) {
        std::printf("%zu %08x %08x %.17g\n", bin,
                    std::bit_cast<std::uint32_t>(output[bin].real),
                    std::bit_cast<std::uint32_t>(output[bin].imag), power[bin]);
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--bins") == 0) {
        return printBins();
    }

    enum class Mode {
        Compare,
        ReferenceOnly,
        ProbeOnly,
    };
    Mode mode = Mode::Compare;
    int firstNumber = 1;
    if (argc >= 2 && std::strcmp(argv[1], "--reference-only") == 0) {
        mode = Mode::ReferenceOnly;
        firstNumber = 2;
    } else if (argc >= 2 && std::strcmp(argv[1], "--probe-only") == 0) {
        mode = Mode::ProbeOnly;
        firstNumber = 2;
    }

    int repetitions = 20'000;
    int trials = 7;
    if (argc > firstNumber) {
        repetitions = std::max(1, std::atoi(argv[firstNumber]));
    }
    if (argc > firstNumber + 1) {
        trials = std::max(1, std::atoi(argv[firstNumber + 1]));
    }

    const auto input = makeFixture();
    Fft::RealFft2048 plan;
    Fft::RealFft2048::Workspace workspace;
    std::array<Fft::ComplexFloat, Fft::RealFft2048::kBins> output{};
    std::array<double, Fft::RealFft2048::kBins> power{};
    std::vector<std::complex<double>> reference(Fft::RealFft2048::kSize);

    for (std::size_t i = 0; i < input.size(); ++i) {
        reference[i] = {static_cast<double>(input[i]), 0.0};
    }
    Fft::complexRadix2Reference(reference);
    plan.forward(input, output, workspace);
    plan.forwardPower(input, power, workspace);

    double maxRelativeBinError = 0.0;
    double maxRelativePowerError = 0.0;
    for (std::size_t bin = 0; bin < output.size(); ++bin) {
        const std::complex<double> candidate(output[bin].real, output[bin].imag);
        const double scale = std::max(1.0, std::abs(reference[bin]));
        maxRelativeBinError = std::max(maxRelativeBinError, std::abs(candidate - reference[bin]) / scale);
        const double referencePower = std::norm(reference[bin]);
        maxRelativePowerError = std::max(
            maxRelativePowerError,
            std::abs(power[bin] - referencePower) / std::max(1.0, referencePower));
    }

    double bestReference = 0.0;
    double bestProbe = 0.0;
    double checksum = 0.0;
    for (int trial = 0; trial < trials; ++trial) {
        if (mode != Mode::ProbeOnly) {
            const Measurement referenceTime = measure(repetitions, [&](std::size_t repetition) {
                for (std::size_t i = 0; i < input.size(); ++i) {
                    reference[i] = {static_cast<double>(input[i]), 0.0};
                }
                Fft::complexRadix2Reference(reference);
                return std::norm(reference[(repetition * 17U) % Fft::RealFft2048::kBins]);
            });
            bestReference = trial == 0 ? referenceTime.nsPerTransform
                                       : std::min(bestReference, referenceTime.nsPerTransform);
            checksum += referenceTime.checksum;
        }
        if (mode != Mode::ReferenceOnly) {
            const Measurement probeTime = measure(repetitions, [&](std::size_t repetition) {
                plan.forwardPower(input, power, workspace);
                return power[(repetition * 17U) % Fft::RealFft2048::kBins];
            });
            bestProbe = trial == 0 ? probeTime.nsPerTransform
                                   : std::min(bestProbe, probeTime.nsPerTransform);
            checksum += probeTime.checksum;
        }
    }

    std::printf("bench_fft: %d transforms x %d trials (best; current machine load applies)\n",
                repetitions, trials);
    if (mode != Mode::ProbeOnly) {
        std::printf("  reference complex<double> radix-2: %8.1f ns/transform\n", bestReference);
    }
    if (mode != Mode::ReferenceOnly) {
        std::printf("  clean real<float> radix-4+power: %8.1f ns/transform\n", bestProbe);
    }
    if (mode == Mode::Compare) {
        std::printf("  speedup: %.2fx\n", bestReference / bestProbe);
    }
    std::printf("  max relative complex-bin error: %.9g\n", maxRelativeBinError);
    std::printf("  max relative power error: %.9g\n", maxRelativePowerError);
    std::printf("  checksum: %.17g\n", checksum);
    return maxRelativeBinError <= 5e-4 && maxRelativePowerError <= 1e-3 ? 0 : 1;
}
