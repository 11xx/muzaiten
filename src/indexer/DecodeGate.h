#pragma once

#include <algorithm>
#include <atomic>
#include <vector>

// Decode-concurrency brake for the analysis worker pools. Measured on the
// reference NFS library: 16 concurrent full-track decodes average 4.2 s
// each (vs ~0.3 s solo), yet aggregate throughput at width 16 is still
// 2.2x that of width 2, so latency alone must NOT drive the pool narrower
// under normal contention. The gate therefore starts at the full worker
// count on every medium and only intervenes on pathological collapse
// (windowed median beyond several times the contended-but-progressing
// range: dying spindles, saturated links, tiny-file seek storms), growing
// back once the collapse passes. Workers whose ordinal is at or above
// target() park instead of claiming work; the completion thread feeds
// per-decode wall times back in.
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
    // Contended-but-progressing decodes on the reference network mount
    // median around 4 s at full width and that width still wins on
    // throughput, so the brake only engages well beyond it. Below the grow
    // threshold the medium is clearly keeping up and the pool recovers
    // toward the cap.
    static constexpr double kSlowDecodeMs = 10000.0;
    static constexpr double kFastDecodeMs = 2000.0;
    static constexpr std::size_t kWindowSize = 8;

    int m_floor;
    int m_cap;
    std::atomic<int> m_target;
    int m_initial;
    std::vector<double> m_window;
};
