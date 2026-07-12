#pragma once

#include <algorithm>
#include <atomic>
#include <vector>

// Adaptive decode-concurrency gate for the analysis worker pools. Decode
// latency on seek-sensitive media (spinning disks, network mounts) grows
// superlinearly with concurrent readers: measured on the reference NFS
// library, 16 parallel full-track decodes average 4.2 s each while compute
// (hash + DSP) needs only ~0.25 s. Workers whose ordinal is at or above
// target() park instead of claiming work; the completion thread feeds
// per-decode wall times back in, and the target shrinks or grows on the
// windowed median with a wide dead band so it settles instead of
// oscillating.
class DecodeGate
{
public:
    DecodeGate(int initialTarget, int floorTarget, int capTarget)
        : m_floor(std::max(1, floorTarget))
        , m_cap(std::max(m_floor, capTarget))
        , m_target(std::clamp(initialTarget, m_floor, m_cap))
        , m_initial(m_target.load())
    {
        m_window.reserve(kWindowSize);
    }

    int target() const { return m_target.load(std::memory_order_relaxed); }
    int initialTarget() const { return m_initial; }

    // Called from the single completion thread only. Returns true when the
    // target changed so the caller can wake parked workers.
    bool recordDecodeMs(double decodeMs)
    {
        if (decodeMs <= 0.0) {
            return false;
        }
        m_window.push_back(decodeMs);
        if (m_window.size() < kWindowSize) {
            return false;
        }
        auto mid = m_window.begin() + static_cast<std::ptrdiff_t>(m_window.size() / 2);
        std::nth_element(m_window.begin(), mid, m_window.end());
        const double p50 = *mid;
        m_window.clear();

        const int current = m_target.load(std::memory_order_relaxed);
        if (p50 > kSlowDecodeMs && current > m_floor) {
            m_target.store(current - 1, std::memory_order_relaxed);
            return true;
        }
        if (p50 < kFastDecodeMs && current < m_cap) {
            m_target.store(current + 1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

private:
    // Full-track canonical decode of a typical song costs a few hundred ms
    // in a vacuum; multi-second medians mean readers are fighting the
    // medium. The band in between is deliberately wide: within it the gate
    // holds its level rather than hunting.
    static constexpr double kSlowDecodeMs = 2500.0;
    static constexpr double kFastDecodeMs = 500.0;
    static constexpr std::size_t kWindowSize = 8;

    int m_floor;
    int m_cap;
    std::atomic<int> m_target;
    int m_initial;
    std::vector<double> m_window;
};
