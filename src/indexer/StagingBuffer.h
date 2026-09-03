#pragma once

#include <QString>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

// Byte-budgeted read-ahead for the analysis scan. Reader threads stage the
// compressed bytes of upcoming candidates in memory so a decode slot feeds
// ffmpeg from a buffer instead of blocking on the medium; the budget bounds
// how far the readers run ahead of the decoders.
//
// Grants are issued in ticket order, one ticket per candidate. The bytes held
// at any moment therefore belong to a contiguous run of the next candidates,
// and a decode slot waiting on candidate N can never be starved by staged
// bytes of candidates after N (with a single decode slot that would be a
// deadlock: the budget full of files nobody will decode before N). A file
// larger than the whole budget is granted once the buffer is empty and holds
// it alone. A zero-byte acquire takes a ticket's turn without holding
// anything, for candidates that are deliberately left unstaged.
class StagingBuffer
{
public:
    explicit StagingBuffer(std::int64_t budgetBytes)
        : m_budget(budgetBytes < 0 ? 0 : budgetBytes)
    {
    }

    // Blocks until `ticket` is the next in sequence and `bytes` fits beside
    // what is staged (or the buffer is empty). Returns false once close() has
    // been called; the caller then holds nothing and must not release.
    bool acquire(std::size_t ticket, std::int64_t bytes)
    {
        std::unique_lock lock(m_mutex);
        m_changed.wait(lock, [&]() {
            return m_closed || (ticket == m_nextTicket && fits(bytes));
        });
        if (m_closed) {
            return false;
        }
        m_staged += bytes;
        ++m_nextTicket;
        m_changed.notify_all();
        return true;
    }

    // Returns `bytes` to the budget and wakes waiting readers.
    void release(std::int64_t bytes)
    {
        {
            std::lock_guard lock(m_mutex);
            m_staged -= bytes;
        }
        m_changed.notify_all();
    }

    // Wakes every waiter and makes acquire() fail from then on.
    void close()
    {
        {
            std::lock_guard lock(m_mutex);
            m_closed = true;
        }
        m_changed.notify_all();
    }

    std::int64_t budget() const { return m_budget; }

    std::int64_t staged() const
    {
        std::lock_guard lock(m_mutex);
        return m_staged;
    }

private:
    bool fits(std::int64_t bytes) const
    {
        return m_staged == 0 || m_staged + bytes <= m_budget;
    }

    std::int64_t m_budget;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::int64_t m_staged = 0;
    std::size_t m_nextTicket = 0;
    bool m_closed = false;
};

// Parses a byte count with an optional K, M, or G suffix (binary multiples,
// case insensitive, optional trailing B). Returns nullopt for anything else,
// including negatives, fractions, and values that overflow.
inline std::optional<std::int64_t> parseByteSize(const QString &text)
{
    QString value = text.trimmed().toUpper();
    if (value.endsWith(QLatin1Char('B'))) {
        value.chop(1);
    }
    std::int64_t multiplier = 1;
    if (value.endsWith(QLatin1Char('K'))) {
        multiplier = std::int64_t{1} << 10;
    } else if (value.endsWith(QLatin1Char('M'))) {
        multiplier = std::int64_t{1} << 20;
    } else if (value.endsWith(QLatin1Char('G'))) {
        multiplier = std::int64_t{1} << 30;
    }
    if (multiplier != 1) {
        value.chop(1);
    }
    if (value.isEmpty()) {
        return std::nullopt;
    }
    for (const QChar character : value) {
        if (character < QLatin1Char('0') || character > QLatin1Char('9')) {
            return std::nullopt;
        }
    }
    bool ok = false;
    const std::int64_t count = value.toLongLong(&ok, 10);
    if (!ok || count > std::numeric_limits<std::int64_t>::max() / multiplier) {
        return std::nullopt;
    }
    return count * multiplier;
}
