#include "SemanticDescendantFixture.h"
#include "ui/SemanticStatusProcess.h"

#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {

QString fixtureTemplate()
{
    const QString evidence = qEnvironmentVariable("MUZAITEN_TEST_EVIDENCE_DIR");
    return QDir(evidence.isEmpty() ? QDir::tempPath() : evidence)
        .filePath(QStringLiteral("status-process-XXXXXX"));
}

} // namespace

class SemanticStatusProcessTest final : public QObject {
    Q_OBJECT
private slots:
    void cancelReapsDescendant()
    {
#ifndef Q_OS_LINUX
        QSKIP("Descendant wait-status evidence requires Linux child-subreaper support");
#else
        QTemporaryDir state(fixtureTemplate());
        QVERIFY(state.isValid());
        if (!qEnvironmentVariableIsEmpty("MUZAITEN_TEST_EVIDENCE_DIR")) state.setAutoRemove(false);
        SemanticDescendantFixture fixture(state.path());
        QVERIFY(fixture.install());
        SemanticStatusProcess helper;
        helper.start(fixture.binary, {QStringLiteral("status")});
        QTRY_VERIFY_WITH_TIMEOUT(fixture.ready(), 2'000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.read(QStringLiteral("sentinel")).isEmpty(), 2'000);
        const qint64 helperPid =
            fixture.read(QStringLiteral("helper")).value(QStringLiteral("pid")).toInteger();
        QCOMPARE(fixture.read(QStringLiteral("helper")).value(QStringLiteral("pgid")).toInteger(), helperPid);
        QCOMPARE(fixture.read(QStringLiteral("provider")).value(QStringLiteral("pgid")).toInteger(), helperPid);
        QVERIFY(fixture.read(QStringLiteral("sentinel")).value(QStringLiteral("pgid")).toInteger() != helperPid);
        QVERIFY(!fixture.providerExited());
        bool heartbeat = false;
        QTimer heartbeatContext;
        QTimer::singleShot(0, &heartbeatContext, [&]() { heartbeat = true; });
        QElapsedTimer elapsed;
        elapsed.start();
        helper.cancel();
        while (elapsed.elapsed() < 1'500
               && (!heartbeat || helper.state() != QProcess::NotRunning || !fixture.providerExited())) {
            QTest::qWait(10);
        }
        const bool providerExited = fixture.providerExited();
        QVERIFY(fixture.recordResult(elapsed.elapsed(), helper.state() == QProcess::NotRunning, heartbeat));
        qInfo("Fixture %s: helper=%lld provider=%lld sentinel=%lld exit=%lldms reaped=%d status=%d",
              qPrintable(state.path()),
              fixture.read(QStringLiteral("helper")).value(QStringLiteral("pid")).toInteger(),
              fixture.read(QStringLiteral("provider")).value(QStringLiteral("pid")).toInteger(),
              fixture.read(QStringLiteral("sentinel")).value(QStringLiteral("pid")).toInteger(),
              elapsed.elapsed(), int(providerExited), fixture.providerWaitStatus);
        QCOMPARE(helper.state(), QProcess::NotRunning);
        QVERIFY(heartbeat);
        QVERIFY2(providerExited, "The provider grandchild must exit and be reaped within 1500 ms");
        QVERIFY(WIFSIGNALED(fixture.providerWaitStatus));
        QCOMPARE(WTERMSIG(fixture.providerWaitStatus), SIGKILL);
        QVERIFY(fixture.sentinelAlive());
        QVERIFY(!QFile::exists(fixture.root.filePath(QStringLiteral("provider-release"))));
#endif
    }

    void successfulStatusPreservesOutput_data()
    {
        QTest::addColumn<bool>("jsonl");
        QTest::newRow("json") << false;
        QTest::newRow("jsonl") << true;
    }

    void successfulStatusPreservesOutput()
    {
        QFETCH(bool, jsonl);
        QTemporaryDir state(fixtureTemplate());
        QVERIFY(state.isValid());
        if (!qEnvironmentVariableIsEmpty("MUZAITEN_TEST_EVIDENCE_DIR")) state.setAutoRemove(false);
        SemanticDescendantFixture fixture(state.path());
        QVERIFY(fixture.install());
        const QByteArray savedJsonl = qgetenv("MUZAITEN_SEMANTIC_FIXTURE_JSONL");
        const auto restoreJsonl = qScopeGuard([&]() {
            if (savedJsonl.isNull()) qunsetenv("MUZAITEN_SEMANTIC_FIXTURE_JSONL");
            else qputenv("MUZAITEN_SEMANTIC_FIXTURE_JSONL", savedJsonl);
        });
        if (jsonl) qputenv("MUZAITEN_SEMANTIC_FIXTURE_JSONL", "1");
        SemanticStatusProcess helper;
        helper.start(fixture.binary, {jsonl ? QStringLiteral("relay") : QStringLiteral("status")});
        QTRY_VERIFY_WITH_TIMEOUT(fixture.ready(), 2'000);
        QVERIFY(fixture.release());
        QTRY_COMPARE_WITH_TIMEOUT(helper.state(), QProcess::NotRunning, 2'000);
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helper.exitCode(), 0);
        const QByteArray output = helper.readAllStandardOutput();
        QVERIFY(output.endsWith('\n'));
        QCOMPARE(output.count('\n'), jsonl ? 2 : 1);
        const auto lines = output.trimmed().split('\n');
        if (jsonl) {
            const auto phase = QJsonDocument::fromJson(lines.first()).object();
            QCOMPARE(phase.value(QStringLiteral("protocol_version")).toInt(), 1);
            QCOMPARE(phase.value(QStringLiteral("event")).toString(), QStringLiteral("phase"));
            QCOMPARE(phase.value(QStringLiteral("phase")).toString(), QStringLiteral("fixture"));
            QVERIFY(!phase.value(QStringLiteral("request_id")).toString().isEmpty());
        }
        const auto semantic = QJsonDocument::fromJson(lines.last()).object()
                                  .value(QStringLiteral("semantic")).toObject();
        QCOMPARE(semantic.value(QStringLiteral("state")).toString(), QStringLiteral("provider-ready"));
        QVERIFY(semantic.value(QStringLiteral("provider")).toObject()
                    .value(QStringLiteral("model")).toObject().value(QStringLiteral("valid")).toBool());
        QVERIFY(helper.readAllStandardError().isEmpty());
        QVERIFY(fixture.sentinelAlive());
        QVERIFY(!fixture.providerExited()); // The helper reaped its child on normal completion.
    }
};

QTEST_GUILESS_MAIN(SemanticStatusProcessTest)
#include "test_semantic_status_process.moc"
