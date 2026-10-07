#include "app/AppPaths.h"
#include "db/Database.h"
#include "search/IndexCache.h"
#include "search/SearchWorker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <memory>

using namespace Search;

namespace {

struct WriterGate {
    QSemaphore entered;
    QSemaphore release;
    std::atomic<int> calls{0};
    bool holdPrepared = false;

    SearchWorker::CacheWriter writer()
    {
        return [this](const QString &path, const CacheSignature &signature,
                      const QVector<SearchRecord> &records, const std::function<bool()> &cancelled) {
            ++calls;
            std::unique_ptr<IndexCache::PreparedWrite> prepared;
            if (holdPrepared) prepared = IndexCache::prepare(path, signature, records, cancelled);
            entered.release();
            release.acquire();
            if (!holdPrepared) prepared = IndexCache::prepare(path, signature, records, cancelled);
            return prepared;
        };
    }
};

class WorkerObserver : public QObject {
    Q_OBJECT
public:
    explicit WorkerObserver(SearchWorker *worker)
    {
        connect(worker, &SearchWorker::cacheDecision, this, &WorkerObserver::cacheDecision, Qt::QueuedConnection);
        connect(worker, &SearchWorker::indexLoaded, this, &WorkerObserver::indexLoaded, Qt::QueuedConnection);
        connect(worker, &SearchWorker::indexGrew, this, &WorkerObserver::indexGrew, Qt::QueuedConnection);
        connect(worker, &SearchWorker::indexRefreshed, this, &WorkerObserver::indexRefreshed, Qt::QueuedConnection);
        connect(worker, &SearchWorker::resultsReady, this, &WorkerObserver::resultsReady, Qt::QueuedConnection);
        connect(worker, &SearchWorker::cacheWriteFinished, this, &WorkerObserver::cacheWriteFinished, Qt::QueuedConnection);
    }

signals:
    void cacheDecision(QString reason, qint64 revision);
    void indexLoaded(int count);
    void indexGrew(int count);
    void indexRefreshed(int count);
    void resultsReady(quint64 queryId, QVector<Search::ScoredResult> results, int total, int hidden);
    void cacheWriteFinished(bool published);
};

class WorkerThread {
public:
    explicit WorkerThread(const QString &path, std::shared_ptr<WriterGate> gate = {})
        : gate(std::move(gate)), worker(new SearchWorker(path, nullptr, this->gate ? this->gate->writer()
                                                                                : SearchWorker::CacheWriter{}))
        , observer(worker)
    {
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();
    }

    ~WorkerThread()
    {
        QMetaObject::invokeMethod(worker, "clearIndex", Qt::BlockingQueuedConnection);
        if (gate) gate->release.release(100);
        thread.quit();
        thread.wait();
    }

    void build(bool force = false)
    {
        QMetaObject::invokeMethod(worker, force ? "rebuildIndex" : "buildIndex", Qt::QueuedConnection);
    }

    std::shared_ptr<WriterGate> gate;
    SearchWorker *worker;
    WorkerObserver observer;
    QThread thread;
};

QByteArray bytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

} // namespace

class TestSearchWorker : public QObject {
    Q_OBJECT
private:
    std::unique_ptr<QTemporaryDir> m_temp;
    std::unique_ptr<Database> m_db;
    Track m_track;

    QString dbPath() const { return m_temp->filePath(QStringLiteral("library.sqlite")); }
    QString cachePath() const { return IndexCache::defaultPath(); }
    CacheSignature signature() const { return IndexCache::currentSignature(*m_db); }

    bool retag(const QString &title)
    {
        m_track.title = title;
        return m_db->upsertTrack(m_track);
    }

    bool seedCache()
    {
        return IndexCache::write(cachePath(), signature(), m_db->allTracksForSearch());
    }

private slots:
    void initTestCase()
    {
        qRegisterMetaType<QVector<Search::ScoredResult>>();
    }

    void init()
    {
        m_temp = std::make_unique<QTemporaryDir>(QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("search-worker-XXXXXX")));
        QVERIFY(m_temp->isValid());
        qputenv("MUZAITEN_STATE_ROOT", m_temp->path().toUtf8());
        qputenv("MUZAITEN_CACHE_DIR", m_temp->filePath(QStringLiteral("cache")).toUtf8());
        m_db = std::make_unique<Database>(QStringLiteral("worker-test-source"));
        QVERIFY2(m_db->open(dbPath()), qPrintable(m_db->lastError()));
        m_track.path = QStringLiteral("/fixture/example.flac");
        m_track.parentDir = QStringLiteral("/fixture");
        m_track.filename = QStringLiteral("example.flac");
        m_track.artistName = m_track.albumArtistName = QStringLiteral("Artist");
        m_track.albumTitle = QStringLiteral("Album");
        QVERIFY(retag(QStringLiteral("Original")));
    }

    void cleanup()
    {
        m_db.reset();
        m_temp.reset();
    }

    void queryCompletesBeforeSerializationAndWarmStartHits()
    {
        auto gate = std::make_shared<WriterGate>();
        {
            WorkerThread running(dbPath(), gate);
            QSignalSpy loaded(&running.observer, &WorkerObserver::indexLoaded);
            QSignalSpy results(&running.observer, &WorkerObserver::resultsReady);
            QSignalSpy finished(&running.observer, &WorkerObserver::cacheWriteFinished);
            QElapsedTimer queryTimer;
            qint64 queryNs = -1;
            connect(running.worker, &SearchWorker::indexLoaded, &running.observer, [&running, &queryTimer] {
                queryTimer.start();
                running.worker->submitQuery(1, QStringLiteral("original"), false);
            });
            connect(&running.observer, &WorkerObserver::resultsReady, &running.observer, [&queryTimer, &queryNs] {
                queryNs = queryTimer.nsecsElapsed();
            });
            running.build();
            QTRY_COMPARE(loaded.count(), 1);
            QTRY_VERIFY(gate->entered.available() > 0);
            QVERIFY(gate->entered.tryAcquire());
            QTRY_COMPARE(results.count(), 1);
            QCOMPARE(results.first().at(2).toInt(), 1);
            QCOMPARE(finished.count(), 0);
            QVERIFY(!IndexCache::read(cachePath()).ok);
            QVERIFY(queryNs >= 0);
            qInfo("Query received while serialization held: %.3f ms since submission (QtTest event loop)",
                  static_cast<double>(queryNs) / 1000000.0);
            gate->release.release();
            QTRY_COMPARE(finished.count(), 1);
            QVERIFY(finished.first().first().toBool());
            auto saved = IndexCache::read(cachePath());
            QVERIFY(saved.ok);
            QVERIFY(saved.signature == signature());
            QCOMPARE(saved.records.size(), 1);
            QCOMPARE(saved.records.first().title, QStringLiteral("Original"));
        }
        WorkerThread warm(dbPath(), gate);
        QSignalSpy decision(&warm.observer, &WorkerObserver::cacheDecision);
        QSignalSpy loaded(&warm.observer, &WorkerObserver::indexLoaded);
        QSignalSpy grew(&warm.observer, &WorkerObserver::indexGrew);
        QSignalSpy results(&warm.observer, &WorkerObserver::resultsReady);
        warm.build();
        QTRY_COMPARE(loaded.count(), 1);
        QCOMPARE(decision.first().first().toString(), QStringLiteral("hit"));
        QCOMPARE(grew.count(), 0);
        warm.worker->submitQuery(2, QStringLiteral("original"), false);
        QTRY_COMPARE(results.count(), 1);
        QCOMPARE(results.first().at(2).toInt(), 1);
        QCOMPARE(gate->calls.load(), 1);
    }

    void sourceChangeDuringSerializationDiscardsBytesAndRefreshes()
    {
        QVERIFY(seedCache());
        const auto oldBytes = bytes(cachePath());
        auto gate = std::make_shared<WriterGate>();
        WorkerThread running(dbPath(), gate);
        QSignalSpy refreshed(&running.observer, &WorkerObserver::indexRefreshed);
        QSignalSpy finished(&running.observer, &WorkerObserver::cacheWriteFinished);
        running.build(true);
        QTRY_VERIFY(gate->entered.available() > 0);
        QVERIFY(gate->entered.tryAcquire());
        QVERIFY(retag(QStringLiteral("Retagged")));
        gate->release.release();
        QTRY_VERIFY(gate->entered.available() > 0);
        QVERIFY(gate->entered.tryAcquire());
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!finished.first().first().toBool());
        QCOMPARE(bytes(cachePath()), oldBytes);
        QTRY_COMPARE(refreshed.count(), 2);
        gate->release.release();
        QTRY_COMPARE(finished.count(), 2);
        QVERIFY(finished.last().first().toBool());
        const auto saved = IndexCache::read(cachePath());
        QVERIFY(saved.ok);
        QVERIFY(saved.signature == signature());
        QCOMPARE(saved.records.first().title, QStringLiteral("Retagged"));
    }

    void supersededPreparedWriteCannotOvertakeLatestBuild()
    {
        QVERIFY(seedCache());
        const auto oldBytes = bytes(cachePath());
        auto gate = std::make_shared<WriterGate>();
        gate->holdPrepared = true;
        WorkerThread running(dbPath(), gate);
        QSignalSpy refreshed(&running.observer, &WorkerObserver::indexRefreshed);
        QSignalSpy finished(&running.observer, &WorkerObserver::cacheWriteFinished);
        QSignalSpy results(&running.observer, &WorkerObserver::resultsReady);
        running.build(true);
        QTRY_VERIFY(gate->entered.available() > 0);
        QVERIFY(gate->entered.tryAcquire());
        QVERIFY(retag(QStringLiteral("Intermediate")));
        running.build(true);
        QTRY_COMPARE(refreshed.count(), 2);
        QVERIFY(retag(QStringLiteral("Latest")));
        running.build(true);
        QTRY_COMPARE(refreshed.count(), 3);
        QCOMPARE(gate->calls.load(), 1);
        running.worker->submitQuery(1, QStringLiteral("latest"), false);
        QTRY_COMPARE(results.count(), 1);
        QCOMPARE(results.first().at(2).toInt(), 1);
        gate->release.release();
        QTRY_VERIFY(gate->entered.available() > 0);
        QVERIFY(gate->entered.tryAcquire());
        QCOMPARE(gate->calls.load(), 2);
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!finished.first().first().toBool());
        QCOMPARE(bytes(cachePath()), oldBytes);
        gate->release.release();
        QTRY_COMPARE(finished.count(), 2);
        QVERIFY(finished.last().first().toBool());
        const auto saved = IndexCache::read(cachePath());
        QVERIFY(saved.ok);
        QVERIFY(saved.signature == signature());
        QCOMPARE(saved.records.first().title, QStringLiteral("Latest"));
    }

    void clearDiscardsPreparedWriteAndReleasesIndex()
    {
        auto gate = std::make_shared<WriterGate>();
        gate->holdPrepared = true;
        WorkerThread running(dbPath(), gate);
        QSignalSpy finished(&running.observer, &WorkerObserver::cacheWriteFinished);
        QSignalSpy results(&running.observer, &WorkerObserver::resultsReady);
        running.build();
        QTRY_VERIFY(gate->entered.available() > 0);
        QVERIFY(gate->entered.tryAcquire());
        QMetaObject::invokeMethod(running.worker, "clearIndex", Qt::BlockingQueuedConnection);
        running.worker->submitQuery(1, QStringLiteral("original"), false);
        QTRY_COMPARE(results.count(), 1);
        QCOMPARE(results.first().at(2).toInt(), 0);
        gate->release.release();
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!finished.first().first().toBool());
        QVERIFY(!IndexCache::read(cachePath()).ok);
        QCOMPARE(QDir(AppPaths::cacheDir()).entryList(QDir::Files).size(), 0);
    }

    void canceledStreamDoesNotPersist()
    {
        WorkerThread running(dbPath());
        QSignalSpy grew(&running.observer, &WorkerObserver::indexGrew);
        QSignalSpy loaded(&running.observer, &WorkerObserver::indexLoaded);
        QSignalSpy results(&running.observer, &WorkerObserver::resultsReady);
        connect(running.worker, &SearchWorker::indexGrew, running.worker, [&running] {
            running.worker->clearIndex();
            running.worker->submitQuery(1, QStringLiteral("original"), false);
        }, Qt::DirectConnection);
        running.build();
        QTRY_COMPARE(results.count(), 1);
        QCOMPARE(grew.count(), 1);
        QCOMPARE(loaded.count(), 0);
        QCOMPARE(results.first().at(2).toInt(), 0);
        QVERIFY(!IndexCache::read(cachePath()).ok);
    }

    void failedPersistenceKeepsIndexQueryable()
    {
        SearchWorker::CacheWriter failing = [](const QString &, const CacheSignature &,
                                              const QVector<SearchRecord> &, const std::function<bool()> &) {
            return std::unique_ptr<IndexCache::PreparedWrite>{};
        };
        SearchWorker worker(dbPath(), nullptr, failing);
        QSignalSpy finished(&worker, &SearchWorker::cacheWriteFinished);
        QSignalSpy results(&worker, &SearchWorker::resultsReady);
        worker.buildIndex();
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!finished.first().first().toBool());
        worker.submitQuery(1, QStringLiteral("original"), false);
        QTRY_COMPARE(results.count(), 1);
        QCOMPARE(results.first().at(2).toInt(), 1);
        QVERIFY(!IndexCache::read(cachePath()).ok);
    }

    void shutdownCancelsWriterWithoutPublishing()
    {
        QSemaphore entered;
        std::atomic<bool> canceled{false};
        SearchWorker::CacheWriter writer = [&entered, &canceled](
            const QString &path, const CacheSignature &signature,
            const QVector<SearchRecord> &records, const std::function<bool()> &cancelled) {
            auto prepared = IndexCache::prepare(path, signature, records, cancelled);
            entered.release();
            // Yielding only services the cancellation handshake; no delay sets correctness.
            while (!cancelled()) QThread::yieldCurrentThread();
            canceled.store(true);
            return prepared;
        };
        auto worker = std::make_unique<SearchWorker>(dbPath(), nullptr, writer);
        QSignalSpy loaded(worker.get(), &SearchWorker::indexLoaded);
        worker->buildIndex();
        QTRY_COMPARE(loaded.count(), 1);
        QTRY_VERIFY(entered.available() > 0);
        QVERIFY(entered.tryAcquire());
        worker.reset();
        QVERIFY(canceled.load());
        QVERIFY(!IndexCache::read(cachePath()).ok);
        QCOMPARE(QDir(AppPaths::cacheDir()).entryList(QDir::Files).size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestSearchWorker)
#include "test_search_worker.moc"
