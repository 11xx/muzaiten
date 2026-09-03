#include "indexer/StagingBuffer.h"

#include <QtTest>

#include <atomic>
#include <chrono>
#include <thread>

class TestStagingBuffer : public QObject
{
    Q_OBJECT

private slots:
    void budgetHoldsThirdFileUntilRelease();
    void grantsInTicketOrder();
    void oversizeFilePassesAlone();
    void closeWakesWaiters();
    void parsesByteSizes();
};

namespace {

// A reader blocked in acquire(), observable from the test thread. The
// destructor closes the buffer so a broken buffer fails the test instead of
// hanging it: an acquire() that ignores close() leaves the thread joinable
// and std::thread's destructor terminates the process.
class Waiter final
{
public:
    Waiter(StagingBuffer &buffer, std::size_t ticket, std::int64_t bytes)
        : m_buffer(buffer)
        , m_thread([this, ticket, bytes]() {
            m_granted = m_buffer.acquire(ticket, bytes);
            m_finished = true;
        })
    {
    }

    ~Waiter()
    {
        m_buffer.close();
        m_thread.join();
    }

    bool finished() const { return m_finished.load(); }
    bool granted() const { return m_granted.load(); }

    // A waiter that must stay blocked has no event to wait for; give it a
    // real chance to misbehave before asserting that it did not.
    bool stillBlockedAfterGrace() const
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return !finished();
    }

private:
    StagingBuffer &m_buffer;
    std::atomic_bool m_finished = false;
    std::atomic_bool m_granted = false;
    std::thread m_thread;
};

} // namespace

void TestStagingBuffer::budgetHoldsThirdFileUntilRelease()
{
    StagingBuffer buffer(100);
    QVERIFY(buffer.acquire(0, 40));
    QVERIFY(buffer.acquire(1, 40));
    QCOMPARE(buffer.staged(), 80);

    Waiter third(buffer, 2, 40);
    QVERIFY(third.stillBlockedAfterGrace());
    QCOMPARE(buffer.staged(), 80);

    buffer.release(40);
    QTRY_VERIFY_WITH_TIMEOUT(third.finished(), 5000);
    QVERIFY(third.granted());
    QCOMPARE(buffer.staged(), 80);
}

void TestStagingBuffer::grantsInTicketOrder()
{
    StagingBuffer buffer(100);
    // Ticket 1 fits trivially yet must wait for ticket 0's turn.
    Waiter second(buffer, 1, 10);
    QVERIFY(second.stillBlockedAfterGrace());
    QCOMPARE(buffer.staged(), 0);

    QVERIFY(buffer.acquire(0, 10));
    QTRY_VERIFY_WITH_TIMEOUT(second.finished(), 5000);
    QVERIFY(second.granted());
    QCOMPARE(buffer.staged(), 20);

    // A zero-byte acquire takes its turn so later tickets are not stranded.
    QVERIFY(buffer.acquire(2, 0));
    QVERIFY(buffer.acquire(3, 5));
    QCOMPARE(buffer.staged(), 25);
}

void TestStagingBuffer::oversizeFilePassesAlone()
{
    StagingBuffer buffer(100);
    QVERIFY(buffer.acquire(0, 250));
    QCOMPARE(buffer.staged(), 250);

    Waiter next(buffer, 1, 1);
    QVERIFY(next.stillBlockedAfterGrace());

    buffer.release(250);
    QTRY_VERIFY_WITH_TIMEOUT(next.finished(), 5000);
    QVERIFY(next.granted());
    QCOMPARE(buffer.staged(), 1);
}

void TestStagingBuffer::closeWakesWaiters()
{
    StagingBuffer buffer(10);
    QVERIFY(buffer.acquire(0, 10));
    Waiter blocked(buffer, 1, 10);
    QVERIFY(blocked.stillBlockedAfterGrace());

    buffer.close();
    QTRY_VERIFY_WITH_TIMEOUT(blocked.finished(), 5000);
    QVERIFY(!blocked.granted());
    QCOMPARE(buffer.staged(), 10);
    QVERIFY(!buffer.acquire(2, 0));
}

void TestStagingBuffer::parsesByteSizes()
{
    QCOMPARE(parseByteSize(QStringLiteral("0")), std::optional<std::int64_t>(0));
    QCOMPARE(parseByteSize(QStringLiteral("4096")), std::optional<std::int64_t>(4096));
    QCOMPARE(parseByteSize(QStringLiteral("12k")), std::optional<std::int64_t>(12 * 1024));
    QCOMPARE(parseByteSize(QStringLiteral("256M")), std::optional<std::int64_t>(std::int64_t{256} << 20));
    QCOMPARE(parseByteSize(QStringLiteral(" 2GB ")), std::optional<std::int64_t>(std::int64_t{2} << 30));
    QCOMPARE(parseByteSize(QStringLiteral("1gb")), std::optional<std::int64_t>(std::int64_t{1} << 30));
    QVERIFY(!parseByteSize(QString()).has_value());
    QVERIFY(!parseByteSize(QStringLiteral("M")).has_value());
    QVERIFY(!parseByteSize(QStringLiteral("-5")).has_value());
    QVERIFY(!parseByteSize(QStringLiteral("1.5G")).has_value());
    QVERIFY(!parseByteSize(QStringLiteral("10T")).has_value());
    QVERIFY(!parseByteSize(QStringLiteral("99999999999999999999G")).has_value());
}

QTEST_MAIN(TestStagingBuffer)
#include "test_staging_buffer.moc"
