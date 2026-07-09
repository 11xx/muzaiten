#include "indexer/Fft.h"

#include <cmath>
#include <numbers>
#include <utility>

namespace {

constexpr std::size_t kRadix4Digits = 5; // 1024 == 4^5

std::uint16_t reverseBase4(std::size_t value)
{
    std::uint16_t reversed = 0;
    for (std::size_t digit = 0; digit < kRadix4Digits; ++digit) {
        reversed = static_cast<std::uint16_t>((reversed << 2U) | (value & 3U));
        value >>= 2U;
    }
    return reversed;
}

void radix4NoTwiddle(float &ar, float &ai, float &br, float &bi,
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
    for (std::size_t k = 0; k < kPackedSize; ++k) {
        const double angle = -twoPi * static_cast<double>(k) / static_cast<double>(kPackedSize);
        m_complexTwiddleReal[k] = static_cast<float>(std::cos(angle));
        m_complexTwiddleImag[k] = static_cast<float>(std::sin(angle));
        m_digitReversed[k] = reverseBase4(k);
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
    for (std::size_t i = 0; i < kPackedSize; ++i) {
        workspace.real[i] = input[2 * i];
        workspace.imag[i] = input[2 * i + 1];
    }

    // Radix-4 DIF leaves output in base-4 digit-reversed order. Reconstruction
    // reads through m_digitReversed directly, avoiding a separate permutation.
    for (std::size_t length = kPackedSize; length > 4; length >>= 2U) {
        const std::size_t quarter = length / 4;
        const std::size_t twiddleStep = kPackedSize / length;
        for (std::size_t base = 0; base < kPackedSize; base += length) {
            radix4NoTwiddle(workspace.real[base], workspace.imag[base],
                            workspace.real[base + quarter], workspace.imag[base + quarter],
                            workspace.real[base + 2 * quarter], workspace.imag[base + 2 * quarter],
                            workspace.real[base + 3 * quarter], workspace.imag[base + 3 * quarter]);

            for (std::size_t j = 1; j < quarter; ++j) {
                const std::size_t a = base + j;
                const std::size_t b = a + quarter;
                const std::size_t c = b + quarter;
                const std::size_t d = c + quarter;
                radix4NoTwiddle(workspace.real[a], workspace.imag[a],
                                workspace.real[b], workspace.imag[b],
                                workspace.real[c], workspace.imag[c],
                                workspace.real[d], workspace.imag[d]);

                const std::size_t twiddle = j * twiddleStep;
                multiplyTwiddle(workspace.real[b], workspace.imag[b],
                                m_complexTwiddleReal[twiddle], m_complexTwiddleImag[twiddle]);
                multiplyTwiddle(workspace.real[c], workspace.imag[c],
                                m_complexTwiddleReal[2 * twiddle], m_complexTwiddleImag[2 * twiddle]);
                multiplyTwiddle(workspace.real[d], workspace.imag[d],
                                m_complexTwiddleReal[3 * twiddle], m_complexTwiddleImag[3 * twiddle]);
            }
        }
    }

    for (std::size_t base = 0; base < kPackedSize; base += 4) {
        radix4NoTwiddle(workspace.real[base], workspace.imag[base],
                        workspace.real[base + 1], workspace.imag[base + 1],
                        workspace.real[base + 2], workspace.imag[base + 2],
                        workspace.real[base + 3], workspace.imag[base + 3]);
    }
}

ComplexFloat RealFft2048::reconstructBin(const Workspace &workspace, std::size_t bin) const noexcept
{
    const std::size_t packedIndex = m_digitReversed[bin];
    const std::size_t mirrorIndex = m_digitReversed[kPackedSize - bin];
    const float aReal = workspace.real[packedIndex];
    const float aImag = workspace.imag[packedIndex];
    const float bReal = workspace.real[mirrorIndex];
    const float bImag = -workspace.imag[mirrorIndex];

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
