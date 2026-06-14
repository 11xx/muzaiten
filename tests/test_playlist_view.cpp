#include "ui/PlaylistView.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QSignalSpy>
#include <QSplitter>
#include <QTest>

class PlaylistViewTest : public QObject {
    Q_OBJECT

private slots:
    void splitterSizesRoundTripThroughViewSettings()
    {
        PlaylistView first;
        first.resize(900, 420);
        first.show();
        QVERIFY(QTest::qWaitForWindowExposed(&first));

        auto *firstSplitter = first.findChild<QSplitter *>();
        QVERIFY(firstSplitter != nullptr);
        firstSplitter->setSizes({210, 690});
        QCoreApplication::processEvents();

        const QJsonObject saved = QJsonDocument::fromJson(first.viewSettingsJson().toUtf8()).object();
        const QJsonArray savedSizes = saved.value(QStringLiteral("splitter")).toArray();
        QCOMPARE(savedSizes.size(), 2);

        PlaylistView second;
        second.resize(900, 420);
        second.show();
        QVERIFY(QTest::qWaitForWindowExposed(&second));
        second.applyViewSettingsJson(first.viewSettingsJson());
        QCoreApplication::processEvents();

        auto *secondSplitter = second.findChild<QSplitter *>();
        QVERIFY(secondSplitter != nullptr);
        QCOMPARE(secondSplitter->sizes(), firstSplitter->sizes());
    }

    void movingSplitterEmitsViewSettingsChanged()
    {
        PlaylistView view;
        view.resize(900, 420);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        auto *splitter = view.findChild<QSplitter *>();
        QVERIFY(splitter != nullptr);
        QSignalSpy spy(&view, &PlaylistView::viewSettingsChanged);

        QVERIFY(QMetaObject::invokeMethod(splitter, "splitterMoved",
                                          Q_ARG(int, 320),
                                          Q_ARG(int, 1)));
        QCoreApplication::processEvents();

        QVERIFY(spy.count() > 0);
    }
};

QTEST_MAIN(PlaylistViewTest)

#include "test_playlist_view.moc"
