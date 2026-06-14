#include "db/PlaylistDatabase.h"
#include "ui/PlaylistView.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMetaObject>
#include <QSignalSpy>
#include <QSplitter>
#include <QTableView>
#include <QHeaderView>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

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

    void savedQueuesSitBelowFlexibleSpacer()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlaylistDatabase db(QStringLiteral("playlist-view-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
        QVERIFY(db.createPlaylist(QStringLiteral("Daily")) > 0);
        QVERIFY(db.createPlaylist(QStringLiteral("Archive")) > 0);

        SavedQueuePlaylistEntry queue;
        queue.id = QStringLiteral("queue:one");
        queue.name = QStringLiteral("saved queue 1");
        queue.savedAt = 1781460895;
        const QString expectedMeta = QDateTime::fromSecsSinceEpoch(queue.savedAt)
                                         .toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss"));

        PlaylistView view;
        view.resize(320, 420);
        view.setDatabase(&db);
        view.setSavedQueueEntries({queue});
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QCoreApplication::processEvents();

        auto *list = view.findChild<QListWidget *>();
        QVERIFY(list != nullptr);
        QCOMPARE(list->item(0)->text(), QStringLiteral("Archive"));
        QCOMPARE(list->item(1)->text(), QStringLiteral("Daily"));
        QCOMPARE(list->item(2)->text(), QString());
        QVERIFY(list->item(2)->sizeHint().height() > 100);
        QCOMPARE(list->item(3)->text(), QStringLiteral("Saved queues"));
        QCOMPARE(list->item(4)->text(), QStringLiteral("saved queue 1"));
        QCOMPARE(list->item(4)->data(Qt::UserRole + 8).toString(), expectedMeta);
    }

    void selectorMetadataModePersistsInViewSettings()
    {
        PlaylistView view;
        const QJsonObject root{{QStringLiteral("selectorMetadata"), QStringLiteral("comment")}};
        view.applyViewSettingsJson(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));

        const QJsonObject saved = QJsonDocument::fromJson(view.viewSettingsJson().toUtf8()).object();
        QCOMPARE(saved.value(QStringLiteral("selectorMetadata")).toString(), QStringLiteral("comment"));
    }

    void selectorDateFormatPersistsInViewSettings()
    {
        PlaylistView view;
        const QJsonObject root{{QStringLiteral("selectorDateFormat"), QStringLiteral("yyyy/MM/dd HH:mm")}};
        view.applyViewSettingsJson(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));

        const QJsonObject saved = QJsonDocument::fromJson(view.viewSettingsJson().toUtf8()).object();
        QCOMPARE(saved.value(QStringLiteral("selectorDateFormat")).toString(), QStringLiteral("yyyy/MM/dd HH:mm"));
    }

    void ratingColumnIsVisibleAndRightmostByDefault()
    {
        PlaylistView view;
        auto *table = view.findChild<QTableView *>();
        QVERIFY(table != nullptr);
        auto *header = table->horizontalHeader();
        QVERIFY(header != nullptr);

        QCOMPARE(table->model()->headerData(5, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("Rating"));
        QVERIFY(!header->isSectionHidden(5));
        QCOMPARE(header->visualIndex(5), header->count() - 1);
    }
};

QTEST_MAIN(PlaylistViewTest)

#include "test_playlist_view.moc"
