#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Fft {

// The v1 reference transform. Kept visible to the probe benchmark and oracle
// tests; the analyzer still uses it until the deliberate v2 boundary.
void complexRadix2Reference(std::vector<std::complex<double>> &buffer);

struct ComplexFloat {
    float real = 0.0F;
    float imag = 0.0F;
};

// First-party, fixed-size real FFT for muzaiten's only production transform.
//
// Clean-room provenance: derived from the DFT even/odd packing identity and a
// radix-4 decimation-in-frequency factorization. No third-party FFT source code
// was consulted or copied. The fixed 2048-point contract deliberately avoids
// planner, arbitrary-size, cache, allocation, and internal-threading overhead.
class RealFft2048 final {
public:
    static constexpr std::size_t kSize = 2'048;
    static constexpr std::size_t kPackedSize = kSize / 2;
    static constexpr std::size_t kBins = kSize / 2 + 1;
    static constexpr std::size_t kStageTwiddleCount = 1'020;

    struct Workspace {
        alignas(64) std::array<float, kPackedSize> real{};
        alignas(64) std::array<float, kPackedSize> imag{};
    };

    RealFft2048() noexcept;

    void forward(std::span<const float, kSize> input,
                 std::span<ComplexFloat, kBins> output,
                 Workspace &workspace) const noexcept;

    // Fused hot-path form: reconstruct only the nonredundant real spectrum and
    // immediately square it into the double-precision power values consumed by
    // the scalar analyzer.
    void forwardPower(std::span<const float, kSize> input,
                      std::span<double, kBins> power,
                      Workspace &workspace) const noexcept;

private:
    void transformPacked(std::span<const float, kSize> input,
                         Workspace &workspace) const noexcept;
    [[gnu::always_inline]] ComplexFloat reconstructBin(
        const Workspace &workspace, std::size_t bin) const noexcept;

    alignas(64) std::array<float, kStageTwiddleCount> m_stageTwiddleReal{};
    alignas(64) std::array<float, kStageTwiddleCount> m_stageTwiddleImag{};
    alignas(64) std::array<float, kBins> m_realTwiddleReal{};
    alignas(64) std::array<float, kBins> m_realTwiddleImag{};
};

} // namespace Fft
