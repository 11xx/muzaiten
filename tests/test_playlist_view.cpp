#include "db/PlaylistDatabase.h"
#include "ui/HeaderLabelStyle.h"
#include "ui/PlaylistView.h"

#include <QApplication>
#include <QBrush>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMetaObject>
#include <QSignalSpy>
#include <QScrollBar>
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
        // Simulate a real user drag: splitterMoved is the only path that updates
        // the persisted sizes. A bare setSizes() must NOT persist (that's how
        // programmatic redistributions stay out of settings).
        firstSplitter->setSizes({210, 690});
        QVERIFY(QMetaObject::invokeMethod(firstSplitter, "splitterMoved",
                                          Q_ARG(int, 210),
                                          Q_ARG(int, 1)));
        QCoreApplication::processEvents();
        const QList<int> draggedSizes = firstSplitter->sizes();
        QVERIFY(draggedSizes.size() == 2);
        QVERIFY(draggedSizes.at(0) >= 180);

        const QJsonObject saved = QJsonDocument::fromJson(first.viewSettingsJson().toUtf8()).object();
        const QJsonArray savedSizes = saved.value(QStringLiteral("splitter")).toArray();
        QCOMPARE(savedSizes.size(), 2);
        QCOMPARE(savedSizes.at(0).toInt(), draggedSizes.at(0));
        QCOMPARE(savedSizes.at(1).toInt(), draggedSizes.at(1));

        PlaylistView second;
        second.resize(900, 420);
        second.show();
        QVERIFY(QTest::qWaitForWindowExposed(&second));
        second.applyViewSettingsJson(first.viewSettingsJson());
        QCoreApplication::processEvents();

        auto *secondSplitter = second.findChild<QSplitter *>();
        QVERIFY(secondSplitter != nullptr);
        QCOMPARE(secondSplitter->sizes(), draggedSizes);
    }

    void programmaticSetSizesDoesNotPersist()
    {
        PlaylistView view;
        view.resize(900, 420);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        auto *splitter = view.findChild<QSplitter *>();
        QVERIFY(splitter != nullptr);

        // A bare setSizes() (no splitterMoved) must leave the persisted sizes
        // untouched — only a real user drag updates them. The persisted value
        // stays at the constructor default (269 / remainder).
        splitter->setSizes({500, 400});
        QCoreApplication::processEvents();

        const QJsonObject saved = QJsonDocument::fromJson(view.viewSettingsJson().toUtf8()).object();
        const QJsonArray savedSizes = saved.value(QStringLiteral("splitter")).toArray();
        QCOMPARE(savedSizes.size(), 2);
        QCOMPARE(savedSizes.at(0).toInt(), 269);
    }

    void unstableSplitterSizesAreIgnoredOnRestore()
    {
        // A degenerate stored distribution (one pane below its minimum) must
        // never be restored — it would shrink the playlist list to a sliver.
        const QJsonObject root{{QStringLiteral("splitter"),
                                QJsonArray{10, 2000}}};
        PlaylistView view;
        view.resize(900, 420);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.applyViewSettingsJson(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
        QCoreApplication::processEvents();

        auto *splitter = view.findChild<QSplitter *>();
        QVERIFY(splitter != nullptr);
        QVERIFY(splitter->sizes().at(0) >= 180);
    }

    void defaultPlaylistListWidthIs269Pixels()
    {
        PlaylistView view;
        const QJsonObject saved = QJsonDocument::fromJson(view.viewSettingsJson().toUtf8()).object();
        const QJsonArray savedSizes = saved.value(QStringLiteral("splitter")).toArray();
        QCOMPARE(savedSizes.size(), 2);
        QCOMPARE(savedSizes.at(0).toInt(), 269);
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

    void tracklistScrollAndSelectionSurvivePlaylistRefresh()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlaylistDatabase db(QStringLiteral("playlist-view-state-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
        const qint64 playlistId = db.createPlaylist(QStringLiteral("Long list"));
        QVERIFY(playlistId > 0);
        qint64 selectedId = 0;
        for (int i = 0; i < 80; ++i) {
            PlaylistItem item;
            item.titleSnapshot = QStringLiteral("Track %1").arg(i);
            const qint64 id = db.addItem(playlistId, item);
            QVERIFY(id > 0);
            if (i == 50) {
                selectedId = id;
            }
        }

        PlaylistView view;
        view.resize(900, 220);
        view.setDatabase(&db);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.selectItemById(selectedId);
        auto *table = view.findChild<QTableView *>();
        QVERIFY(table != nullptr);
        table->verticalScrollBar()->setValue(35);
        const int scrollBefore = table->verticalScrollBar()->value();
        QVERIFY(scrollBefore > 0);

        // MainWindow refreshes the selector whenever returning to key-5.
        view.reloadPlaylists();
        QCoreApplication::processEvents();

        QCOMPARE(table->verticalScrollBar()->value(), scrollBefore);
        QVERIFY(table->currentIndex().isValid());
        QCOMPARE(table->currentIndex().row(), 50);
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

    void ratingColumnIsHiddenByDefault()
    {
        PlaylistView view;
        auto *table = view.findChild<QTableView *>();
        QVERIFY(table != nullptr);
        auto *header = table->horizontalHeader();
        QVERIFY(header != nullptr);

        QCOMPARE(table->model()->headerData(5, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("Rating"));
        QVERIFY(header->isSectionHidden(5));
        QCOMPARE(header->visualIndex(5), header->count() - 1);
    }

    void nowPlayingRowPaintsLeftMarker()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlaylistDatabase db(QStringLiteral("playlist-view-marker-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
        const qint64 playlistId = db.createPlaylist(QStringLiteral("Now playing"));
        QVERIFY(playlistId > 0);

        PlaylistItem item;
        item.trackPath = QStringLiteral("/music/current.flac");
        item.titleSnapshot = QStringLiteral("Current");
        QVERIFY(db.addItem(playlistId, item) > 0);

        PlaylistView view;
        view.resize(900, 260);
        view.setDatabase(&db);
        view.selectPlaylist(playlistId);
        view.setNowPlaying(item.trackPath, playlistId);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QCoreApplication::processEvents();

        auto *table = view.findChild<QTableView *>();
        QVERIFY(table != nullptr);
        const QRect rowRect = table->visualRect(table->model()->index(0, 0));
        QVERIFY(rowRect.isValid());

        const QImage image = table->viewport()->grab().toImage();
        const QColor marker = QColor::fromRgba(image.pixel(1, rowRect.center().y()));
        QCOMPARE(marker, table->palette().color(QPalette::Highlight));
    }

    void tracklistHeaderUsesMutedFlatStyle()
    {
        PlaylistView view;
        auto *table = view.findChild<QTableView *>();
        QVERIFY(table != nullptr);
        auto *header = table->horizontalHeader();
        QVERIFY(header != nullptr);
        static constexpr HeaderViewStyle kHeaderStyle{
            HeaderLabelStyle{QFont::Normal, true, HeaderLabelTone::Muted, 0.20},
            false,
        };

        const QVariant fontValue = table->model()->headerData(1, Qt::Horizontal, Qt::FontRole);
        QVERIFY(fontValue.isValid());
        QCOMPARE(fontValue.value<QFont>().weight(), QFont::Normal);

        const QVariant brushValue = table->model()->headerData(1, Qt::Horizontal, Qt::ForegroundRole);
        QVERIFY(brushValue.isValid());
        QCOMPARE(brushValue.value<QBrush>().color(), headerLabelBrush(QApplication::palette(), kHeaderStyle.labels).color());
        QCOMPARE(header->styleSheet(), headerViewStyleSheet(kHeaderStyle, header));
    }
};

QTEST_MAIN(PlaylistViewTest)

#include "test_playlist_view.moc"
