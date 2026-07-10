#include "indexer/Fft.h"

#include <cmath>
#include <numbers>
#include <utility>

#if defined(__SSE2__)
#include <xmmintrin.h>
#endif

namespace {

constexpr std::size_t kRadix4Digits = 5; // 1024 == 4^5

constexpr std::uint16_t reverseBase4(std::size_t value)
{
    std::uint16_t reversed = 0;
    for (std::size_t digit = 0; digit < kRadix4Digits; ++digit) {
        reversed = static_cast<std::uint16_t>((reversed << 2U) | (value & 3U));
        value >>= 2U;
    }
    return reversed;
}

struct DigitSwap {
    std::uint16_t first;
    std::uint16_t second;
};

consteval auto makeDigitSwaps()
{
    // A five-digit base-4 number has 4^3 palindromes. Every other index
    // belongs to one two-element reversal cycle.
    constexpr std::size_t swapCount = (1'024 - 64) / 2;
    std::array<DigitSwap, swapCount> swaps{};
    std::size_t next = 0;
    for (std::size_t index = 1; index < 1'024; ++index) {
        const std::uint16_t reversed = reverseBase4(index);
        if (index < reversed) {
            swaps[next++] = {static_cast<std::uint16_t>(index), reversed};
        }
    }
    return swaps;
}

inline constexpr auto kDigitSwaps = makeDigitSwaps();

[[gnu::always_inline]] inline void radix4NoTwiddle(
    float &ar, float &ai, float &br, float &bi,
    float &cr, float &ci, float &dr, float &di)
{
    const float sumAcReal = ar + cr;
    const float sumAcImag = ai + ci;
    const float diffAcReal = ar - cr;
    const float diffAcImag = ai - ci;
    const float sumBdReal = br + dr;
    const float sumBdImag = bi + di;
    const float diffBdReal = br - dr;
    const float diffBdImag = bi - di;

    ar = sumAcReal + sumBdReal;
    ai = sumAcImag + sumBdImag;
    br = diffAcReal + diffBdImag;
    bi = diffAcImag - diffBdReal;
    cr = sumAcReal - sumBdReal;
    ci = sumAcImag - sumBdImag;
    dr = diffAcReal - diffBdImag;
    di = diffAcImag + diffBdReal;
}

void multiplyTwiddle(float &real, float &imag, float twiddleReal, float twiddleImag)
{
    const float productReal = real * twiddleReal - imag * twiddleImag;
    const float productImag = real * twiddleImag + imag * twiddleReal;
    real = productReal;
    imag = productImag;
}

#if defined(__SSE2__)
[[gnu::always_inline]] inline void radix4Vector(
    __m128 &ar, __m128 &ai, __m128 &br, __m128 &bi,
    __m128 &cr, __m128 &ci, __m128 &dr, __m128 &di) noexcept
{
    const __m128 sumAcReal = _mm_add_ps(ar, cr);
    const __m128 sumAcImag = _mm_add_ps(ai, ci);
    const __m128 diffAcReal = _mm_sub_ps(ar, cr);
    const __m128 diffAcImag = _mm_sub_ps(ai, ci);
    const __m128 sumBdReal = _mm_add_ps(br, dr);
    const __m128 sumBdImag = _mm_add_ps(bi, di);
    const __m128 diffBdReal = _mm_sub_ps(br, dr);
    const __m128 diffBdImag = _mm_sub_ps(bi, di);

    ar = _mm_add_ps(sumAcReal, sumBdReal);
    ai = _mm_add_ps(sumAcImag, sumBdImag);
    br = _mm_add_ps(diffAcReal, diffBdImag);
    bi = _mm_sub_ps(diffAcImag, diffBdReal);
    cr = _mm_sub_ps(sumAcReal, sumBdReal);
    ci = _mm_sub_ps(sumAcImag, sumBdImag);
    dr = _mm_sub_ps(diffAcReal, diffBdImag);
    di = _mm_add_ps(diffAcImag, diffBdReal);
}

[[gnu::always_inline]] inline void multiplyTwiddleVector(
    __m128 &real, __m128 &imag, __m128 twiddleReal, __m128 twiddleImag) noexcept
{
    const __m128 productReal =
        _mm_sub_ps(_mm_mul_ps(real, twiddleReal), _mm_mul_ps(imag, twiddleImag));
    const __m128 productImag =
        _mm_add_ps(_mm_mul_ps(real, twiddleImag), _mm_mul_ps(imag, twiddleReal));
    real = productReal;
    imag = productImag;
}
#endif

template<std::size_t Length>
inline constexpr std::size_t kStageTwiddleOffset = [] {
    std::size_t offset = 0;
    for (std::size_t length = Fft::RealFft2048::kPackedSize; length > Length;
         length >>= 2U) {
        offset += 3 * (length / 4);
    }
    return offset;
}();

template<std::size_t Length>
[[gnu::always_inline]] inline void radix4Stage(
    Fft::RealFft2048::Workspace &workspace,
    const std::array<float, Fft::RealFft2048::kStageTwiddleCount> &twiddleReal,
    const std::array<float, Fft::RealFft2048::kStageTwiddleCount> &twiddleImag) noexcept
{
    constexpr std::size_t quarter = Length / 4;
    constexpr std::size_t twiddleOffset = kStageTwiddleOffset<Length>;
    for (std::size_t base = 0; base < Fft::RealFft2048::kPackedSize; base += Length) {
        radix4NoTwiddle(workspace.real[base], workspace.imag[base],
                        workspace.real[base + quarter], workspace.imag[base + quarter],
                        workspace.real[base + 2 * quarter], workspace.imag[base + 2 * quarter],
                        workspace.real[base + 3 * quarter], workspace.imag[base + 3 * quarter]);

        std::size_t j = 1;
#if defined(__SSE2__)
        for (; j + 3 < quarter; j += 4) {
            const std::size_t a = base + j;
            const std::size_t b = a + quarter;
            const std::size_t c = b + quarter;
            const std::size_t d = c + quarter;
            __m128 ar = _mm_loadu_ps(workspace.real.data() + a);
            __m128 ai = _mm_loadu_ps(workspace.imag.data() + a);
            __m128 br = _mm_loadu_ps(workspace.real.data() + b);
            __m128 bi = _mm_loadu_ps(workspace.imag.data() + b);
            __m128 cr = _mm_loadu_ps(workspace.real.data() + c);
            __m128 ci = _mm_loadu_ps(workspace.imag.data() + c);
            __m128 dr = _mm_loadu_ps(workspace.real.data() + d);
            __m128 di = _mm_loadu_ps(workspace.imag.data() + d);
            radix4Vector(ar, ai, br, bi, cr, ci, dr, di);

            multiplyTwiddleVector(
                br, bi,
                _mm_loadu_ps(twiddleReal.data() + twiddleOffset + j),
                _mm_loadu_ps(twiddleImag.data() + twiddleOffset + j));
            multiplyTwiddleVector(
                cr, ci,
                _mm_loadu_ps(twiddleReal.data() + twiddleOffset + quarter + j),
                _mm_loadu_ps(twiddleImag.data() + twiddleOffset + quarter + j));
            multiplyTwiddleVector(
                dr, di,
                _mm_loadu_ps(twiddleReal.data() + twiddleOffset + 2 * quarter + j),
                _mm_loadu_ps(twiddleImag.data() + twiddleOffset + 2 * quarter + j));

            _mm_storeu_ps(workspace.real.data() + a, ar);
            _mm_storeu_ps(workspace.imag.data() + a, ai);
            _mm_storeu_ps(workspace.real.data() + b, br);
            _mm_storeu_ps(workspace.imag.data() + b, bi);
            _mm_storeu_ps(workspace.real.data() + c, cr);
            _mm_storeu_ps(workspace.imag.data() + c, ci);
            _mm_storeu_ps(workspace.real.data() + d, dr);
            _mm_storeu_ps(workspace.imag.data() + d, di);
        }
#endif
        for (; j < quarter; ++j) {
            const std::size_t a = base + j;
            const std::size_t b = a + quarter;
            const std::size_t c = b + quarter;
            const std::size_t d = c + quarter;
            radix4NoTwiddle(workspace.real[a], workspace.imag[a],
                            workspace.real[b], workspace.imag[b],
                            workspace.real[c], workspace.imag[c],
                            workspace.real[d], workspace.imag[d]);

            multiplyTwiddle(workspace.real[b], workspace.imag[b],
                            twiddleReal[twiddleOffset + j],
                            twiddleImag[twiddleOffset + j]);
            multiplyTwiddle(workspace.real[c], workspace.imag[c],
                            twiddleReal[twiddleOffset + quarter + j],
                            twiddleImag[twiddleOffset + quarter + j]);
            multiplyTwiddle(workspace.real[d], workspace.imag[d],
                            twiddleReal[twiddleOffset + 2 * quarter + j],
                            twiddleImag[twiddleOffset + 2 * quarter + j]);
        }
    }
}

} // namespace

namespace Fft {

void complexRadix2Reference(std::vector<std::complex<double>> &buffer)
{
    const std::size_t n = buffer.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1U;
        for (; (j & bit) != 0U; bit >>= 1U) {
            j ^= bit;
        }
        j |= bit;
        if (i < j) {
            std::swap(buffer[i], buffer[j]);
        }
    }
    for (std::size_t length = 2; length <= n; length <<= 1U) {
        const double angle = -2.0 * std::numbers::pi / static_cast<double>(length);
        const std::complex<double> root(std::cos(angle), std::sin(angle));
        for (std::size_t start = 0; start < n; start += length) {
            std::complex<double> twiddle(1.0, 0.0);
            for (std::size_t k = 0; k < length / 2; ++k) {
                const std::complex<double> even = buffer[start + k];
                const std::complex<double> odd = buffer[start + k + length / 2] * twiddle;
                buffer[start + k] = even + odd;
                buffer[start + k + length / 2] = even - odd;
                twiddle *= root;
            }
        }
    }
}

RealFft2048::RealFft2048() noexcept
{
    constexpr double twoPi = 2.0 * std::numbers::pi;
    std::size_t twiddleOffset = 0;
    for (std::size_t length = kPackedSize; length > 4; length >>= 2U) {
        const std::size_t quarter = length / 4;
        const std::size_t step = kPackedSize / length;
        for (std::size_t factor = 1; factor <= 3; ++factor) {
            for (std::size_t j = 0; j < quarter; ++j) {
                const std::size_t k = factor * j * step;
                const double angle =
                    -twoPi * static_cast<double>(k) / static_cast<double>(kPackedSize);
                m_stageTwiddleReal[twiddleOffset] = static_cast<float>(std::cos(angle));
                m_stageTwiddleImag[twiddleOffset] = static_cast<float>(std::sin(angle));
                ++twiddleOffset;
            }
        }
    }
    for (std::size_t k = 0; k < kBins; ++k) {
        const double angle = -twoPi * static_cast<double>(k) / static_cast<double>(kSize);
        m_realTwiddleReal[k] = static_cast<float>(std::cos(angle));
        m_realTwiddleImag[k] = static_cast<float>(std::sin(angle));
    }
}

void RealFft2048::transformPacked(std::span<const float, kSize> input,
                                  Workspace &workspace) const noexcept
{
#if defined(__SSE2__)
    // The source is interleaved real/imag pairs while the transform uses
    // structure-of-arrays. Four packed samples become two aligned vectors;
    // this is an exact data shuffle, so it cannot alter floating-point results.
    for (std::size_t i = 0; i < kPackedSize; i += 4) {
        const __m128 first = _mm_loadu_ps(input.data() + 2 * i);
        const __m128 second = _mm_loadu_ps(input.data() + 2 * i + 4);
        const __m128 real = _mm_shuffle_ps(first, second, _MM_SHUFFLE(2, 0, 2, 0));
        const __m128 imag = _mm_shuffle_ps(first, second, _MM_SHUFFLE(3, 1, 3, 1));
        _mm_store_ps(workspace.real.data() + i, real);
        _mm_store_ps(workspace.imag.data() + i, imag);
    }
#else
    for (std::size_t i = 0; i < kPackedSize; ++i) {
        workspace.real[i] = input[2 * i];
        workspace.imag[i] = input[2 * i + 1];
    }
#endif

    // Each stage is a separate compile-time instance. The production size is
    // fixed, so this removes stage divisions and lets the compiler specialize
    // the address arithmetic without adding a planner or runtime dispatch.
    radix4Stage<1'024>(workspace, m_stageTwiddleReal, m_stageTwiddleImag);
    radix4Stage<256>(workspace, m_stageTwiddleReal, m_stageTwiddleImag);
    radix4Stage<64>(workspace, m_stageTwiddleReal, m_stageTwiddleImag);
    radix4Stage<16>(workspace, m_stageTwiddleReal, m_stageTwiddleImag);

    for (std::size_t base = 0; base < kPackedSize; base += 4) {
        radix4NoTwiddle(workspace.real[base], workspace.imag[base],
                        workspace.real[base + 1], workspace.imag[base + 1],
                        workspace.real[base + 2], workspace.imag[base + 2],
                        workspace.real[base + 3], workspace.imag[base + 3]);
    }

    // Pay the base-4 digit reversal once so the two reconstruction consumers
    // walk natural-order bins. Sequential forward/reverse streams are cheaper
    // than two table-indexed random loads per bin in the power loop.
    for (const DigitSwap swap : kDigitSwaps) {
        std::swap(workspace.real[swap.first], workspace.real[swap.second]);
        std::swap(workspace.imag[swap.first], workspace.imag[swap.second]);
    }
}

[[gnu::always_inline]] inline ComplexFloat RealFft2048::reconstructBin(
    const Workspace &workspace, std::size_t bin) const noexcept
{
    const float aReal = workspace.real[bin];
    const float aImag = workspace.imag[bin];
    const float bReal = workspace.real[kPackedSize - bin];
    const float bImag = -workspace.imag[kPackedSize - bin];

    const float evenReal = 0.5F * (aReal + bReal);
    const float evenImag = 0.5F * (aImag + bImag);
    const float oddReal = 0.5F * (aImag - bImag);
    const float oddImag = -0.5F * (aReal - bReal);
    const float twiddledOddReal =
        m_realTwiddleReal[bin] * oddReal - m_realTwiddleImag[bin] * oddImag;
    const float twiddledOddImag =
        m_realTwiddleReal[bin] * oddImag + m_realTwiddleImag[bin] * oddReal;
    return {evenReal + twiddledOddReal, evenImag + twiddledOddImag};
}

void RealFft2048::forward(std::span<const float, kSize> input,
                          std::span<ComplexFloat, kBins> output,
                          Workspace &workspace) const noexcept
{
    transformPacked(input, workspace);
    const float packedZeroReal = workspace.real[0];
    const float packedZeroImag = workspace.imag[0];
    output[0] = {packedZeroReal + packedZeroImag, 0.0F};
    for (std::size_t bin = 1; bin < kPackedSize; ++bin) {
        output[bin] = reconstructBin(workspace, bin);
    }
    output[kPackedSize] = {packedZeroReal - packedZeroImag, 0.0F};
}

void RealFft2048::forwardPower(std::span<const float, kSize> input,
                               std::span<double, kBins> power,
                               Workspace &workspace) const noexcept
{
    transformPacked(input, workspace);
    const double dc = static_cast<double>(workspace.real[0] + workspace.imag[0]);
    power[0] = dc * dc;
    for (std::size_t bin = 1; bin < kPackedSize; ++bin) {
        const ComplexFloat value = reconstructBin(workspace, bin);
        const double real = static_cast<double>(value.real);
        const double imag = static_cast<double>(value.imag);
        power[bin] = real * real + imag * imag;
    }
    const double nyquist = static_cast<double>(workspace.real[0] - workspace.imag[0]);
    power[kPackedSize] = nyquist * nyquist;
}

} // namespace Fft
