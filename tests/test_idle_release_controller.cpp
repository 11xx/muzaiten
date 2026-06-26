#include <QtTest>
#include <QWidget>

#include "ui/IdleReleaseController.h"

// Verifies the idle-release lifecycle: a page frees its data only after staying
// hidden past the idle window, quick re-shows cancel the release, and the
// restore callback runs exactly once per release when the page returns.
class TestIdleReleaseController : public QObject {
    Q_OBJECT

private slots:
    void releasesAfterIdleHide();
    void quickReshowCancelsRelease();
    void restoreRunsOncePerRelease();
};

void TestIdleReleaseController::releasesAfterIdleHide()
{
    QWidget page;
    int released = 0;
    IdleReleaseController controller(&page, [&] { ++released; }, {}, /*idleMs=*/30);
    page.show();
    QTest::qWait(10);
    QCOMPARE(released, 0); // resident while visible

    page.hide();
    QTest::qWait(120);
    QCOMPARE(released, 1); // freed once the idle window elapsed
}

void TestIdleReleaseController::quickReshowCancelsRelease()
{
    QWidget page;
    int released = 0;
    IdleReleaseController controller(&page, [&] { ++released; }, {}, /*idleMs=*/80);
    page.show();
    QTest::qWait(10);

    page.hide();
    QTest::qWait(20); // well under the idle window
    page.show();      // back before the timer fires
    QTest::qWait(120);
    QCOMPARE(released, 0); // stayed resident across the quick toggle
}

void TestIdleReleaseController::restoreRunsOncePerRelease()
{
    QWidget page;
    int released = 0;
    int restored = 0;
    IdleReleaseController controller(&page, [&] { ++released; }, [&] { ++restored; }, /*idleMs=*/30);
    page.show();
    QTest::qWait(10);

    page.hide();
    QTest::qWait(120);
    QCOMPARE(released, 1);

    page.show(); // returning after a release rebuilds
    QTest::qWait(10);
    QCOMPARE(restored, 1);

    // A toggle that never released must not trigger a redundant restore.
    page.hide();
    QTest::qWait(5);
    page.show();
    QTest::qWait(10);
    QCOMPARE(restored, 1);
}

QTEST_MAIN(TestIdleReleaseController)
#include "test_idle_release_controller.moc"
