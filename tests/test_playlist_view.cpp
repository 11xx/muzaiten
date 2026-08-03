#include "db/PlaylistDatabase.h"
#include "ui/HeaderLabelStyle.h"
#include "ui/PlaylistView.h"
#include "ui/TableViewState.h"

#include <QApplication>
#include <algorithm>
#include <QBrush>
#include <QDateTime>
#include <functional>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMetaObject>
#include <QSignalSpy>
#include <QScrollBar>
#include <QSplitter>
#include <QTableView>
#include <QHeaderView>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>

namespace {

QTableView *itemTable(PlaylistView &view)
{
    return view.findChild<QTableView *>(QStringLiteral("PlaylistItemTable"));
}

QString identityAt(const QTableView &table, int row)
{
    return row >= 0 && row < table.model()->rowCount()
        ? table.model()->index(row, 0).data(TableViewState::IdentityRole).toString()
        : QString();
}

QSet<QString> selectedIdentities(const QTableView &table)
{
    QSet<QString> identities;
    if (const QItemSelectionModel *selection = table.selectionModel(); selection != nullptr) {
        for (const QModelIndex &index : selection->selectedRows(0)) {
            const QString identity = identityAt(table, index.row());
            if (!identity.isEmpty()) {
                identities.insert(identity);
            }
        }
    }
    return identities;
}

QString currentIdentity(const QTableView &table)
{
    return table.currentIndex().isValid() ? identityAt(table, table.currentIndex().row()) : QString();
}

QPair<QString, int> topAnchor(const QTableView &table)
{
    if (table.viewport() == nullptr || table.viewport()->rect().isEmpty()) {
        return {};
    }
    const QModelIndex top = table.indexAt(table.viewport()->rect().topLeft());
    return top.isValid() ? qMakePair(identityAt(table, top.row()), table.visualRect(top).top()) : QPair<QString, int>();
}

QHash<QString, int> rowsByIdentity(const QTableView &table)
{
    QHash<QString, int> rows;
    for (int row = 0; row < table.model()->rowCount(); ++row) {
        const QString identity = identityAt(table, row);
        if (!identity.isEmpty()) {
            rows.insert(identity, row);
        }
    }
    return rows;
}

void establishState(QTableView &table, const QVector<int> &selectedRows, int currentRow, int anchorRow)
{
    table.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    QItemSelectionModel *selection = table.selectionModel();
    QVERIFY(selection != nullptr);
    selection->clearSelection();
    for (const int row : selectedRows) {
        selection->select(table.model()->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    selection->setCurrentIndex(table.model()->index(currentRow, 0), QItemSelectionModel::NoUpdate);
    table.scrollTo(table.model()->index(anchorRow, 0), QAbstractItemView::PositionAtTop);
    table.verticalScrollBar()->setValue(table.verticalScrollBar()->value() + 7);
    QCoreApplication::processEvents();
}

QVector<qint64> addItems(PlaylistDatabase &db, qint64 playlistId, int count,
                         const std::function<QString(int)> &titleFor)
{
    QVector<qint64> ids;
    ids.reserve(count);
    for (int i = 0; i < count; ++i) {
        PlaylistItem item;
        item.titleSnapshot = titleFor(i);
        item.artistSnapshot = QStringLiteral("Artist %1").arg(i);
        ids.push_back(db.addItem(playlistId, item));
    }
    return ids;
}

} // namespace

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

    void savedQueueGroupsAreOrderedAndFoldedByDefault()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlaylistDatabase db(QStringLiteral("playlist-view-groups-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
        QVERIFY(db.createPlaylist(QStringLiteral("Daily")) > 0);

        const auto makeQueue = [](const QString &id, SavedQueuePlaylistEntry::Kind kind) {
            SavedQueuePlaylistEntry queue;
            queue.id = id;
            queue.name = id;
            queue.savedAt = 1781460895;
            queue.kind = kind;
            return queue;
        };
        // Deliberately scrambled input order: the view groups and orders them
        // manual → auto → radio regardless.
        PlaylistView view;
        view.resize(320, 420);
        view.setDatabase(&db);
        view.setSavedQueueEntries({
            makeQueue(QStringLiteral("radio-1"), SavedQueuePlaylistEntry::Kind::Radio),
            makeQueue(QStringLiteral("manual-1"), SavedQueuePlaylistEntry::Kind::Manual),
            makeQueue(QStringLiteral("auto-1"), SavedQueuePlaylistEntry::Kind::Auto),
        });
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        auto *list = view.findChild<QListWidget *>();
        QVERIFY(list != nullptr);
        // 0: playlist, 1: spacer, then three headers each followed by their entry.
        QCOMPARE(list->item(0)->text(), QStringLiteral("Daily"));
        QCOMPARE(list->item(2)->text(), QStringLiteral("Saved queues"));
        QCOMPARE(list->item(3)->text(), QStringLiteral("manual-1"));
        QCOMPARE(list->item(4)->text(), QStringLiteral("Auto-saved"));
        QCOMPARE(list->item(5)->text(), QStringLiteral("auto-1"));
        QCOMPARE(list->item(6)->text(), QStringLiteral("Radio sessions"));
        QCOMPARE(list->item(7)->text(), QStringLiteral("radio-1"));
        // Entries are folded away by default; the headers stay visible and
        // paint a folded chevron plus the group size.
        QVERIFY(list->item(3)->isHidden());
        QVERIFY(list->item(5)->isHidden());
        QVERIFY(list->item(7)->isHidden());
        QVERIFY(!list->item(2)->isHidden());
        QCOMPARE(list->item(2)->data(Qt::UserRole + 1).toString(), QStringLiteral("▸ Saved queues (1)"));
    }

    void unfoldedQueueGroupsPersistInViewSettings()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlaylistDatabase db(QStringLiteral("playlist-view-unfold-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
        QVERIFY(db.createPlaylist(QStringLiteral("Daily")) > 0);

        SavedQueuePlaylistEntry queue;
        queue.id = QStringLiteral("manual-1");
        queue.name = queue.id;
        queue.kind = SavedQueuePlaylistEntry::Kind::Manual;

        PlaylistView view;
        const QJsonObject root{{QStringLiteral("unfoldedQueueGroups"),
                                QJsonArray{QStringLiteral("manual")}}};
        view.applyViewSettingsJson(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
        view.setDatabase(&db);
        view.setSavedQueueEntries({queue});

        auto *list = view.findChild<QListWidget *>();
        QVERIFY(list != nullptr);
        QCOMPARE(list->item(3)->text(), QStringLiteral("manual-1"));
        QVERIFY(!list->item(3)->isHidden());

        const QJsonObject saved = QJsonDocument::fromJson(view.viewSettingsJson().toUtf8()).object();
        const QJsonArray unfolded = saved.value(QStringLiteral("unfoldedQueueGroups")).toArray();
        QCOMPARE(unfolded.size(), 1);
        QCOMPARE(unfolded.first().toString(), QStringLiteral("manual"));
    }

    void headerShowsOnlyWhenSelectedNameOverflowsSidebar()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlaylistDatabase db(QStringLiteral("playlist-view-header-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
        const qint64 shortId = db.createPlaylist(QStringLiteral("Short"));
        const QString longName = QStringLiteral("A very long playlist name that cannot possibly fit the sidebar width");
        const qint64 longId = db.createPlaylist(longName);
        QVERIFY(shortId > 0 && longId > 0);

        PlaylistView view;
        view.resize(900, 420);
        view.setDatabase(&db);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        auto *header = view.findChild<QLabel *>(QStringLiteral("PlaylistHeader"));
        QVERIFY(header != nullptr);

        view.selectPlaylist(shortId);
        QCoreApplication::processEvents();
        QVERIFY(header->isHidden());

        view.selectPlaylist(longId);
        QCoreApplication::processEvents();
        QVERIFY(!header->isHidden());
        QVERIFY(header->text().contains(longName));
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

    void tracklistIdentityStateSurvivesDatabaseReorder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlaylistDatabase db(QStringLiteral("playlist-view-reorder-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
        const qint64 playlistId = db.createPlaylist(QStringLiteral("Reordered"));
        QVERIFY(playlistId > 0);
        const QVector<qint64> ids = addItems(db, playlistId, 80, [](int i) {
            return QStringLiteral("Track %1").arg(i, 3, 10, QLatin1Char('0'));
        });
        QCOMPARE(ids.size(), 80);

        PlaylistView view;
        view.resize(900, 220);
        view.setDatabase(&db);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.selectPlaylist(playlistId);
        QCoreApplication::processEvents();
        QTableView *table = itemTable(view);
        QVERIFY(table != nullptr);
        establishState(*table, {10, 30, 60}, 45, 25);

        const QSet<QString> selectedBefore = selectedIdentities(*table);
        const QString currentBefore = currentIdentity(*table);
        const QPair<QString, int> anchorBefore = topAnchor(*table);
        const QHash<QString, int> rowsBefore = rowsByIdentity(*table);
        QVERIFY(selectedBefore.size() >= 3);
        QVERIFY(!currentBefore.isEmpty());
        QVERIFY(!anchorBefore.first.isEmpty());

        QVector<qint64> reversed = ids;
        std::reverse(reversed.begin(), reversed.end());
        QVERIFY(db.reorderItems(playlistId, reversed));
        view.reloadItems();
        QCoreApplication::processEvents();

        const QHash<QString, int> rowsAfter = rowsByIdentity(*table);
        QCOMPARE(selectedIdentities(*table), selectedBefore);
        QCOMPARE(currentIdentity(*table), currentBefore);
        QCOMPARE(topAnchor(*table), anchorBefore);
        for (const QString &identity : selectedBefore) {
            QVERIFY(rowsAfter.value(identity) != rowsBefore.value(identity));
        }
        QVERIFY(rowsAfter.value(currentBefore) != rowsBefore.value(currentBefore));
        QVERIFY(rowsAfter.value(anchorBefore.first) != rowsBefore.value(anchorBefore.first));
    }

    void tracklistSortResetPreservesIdentityState()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlaylistDatabase db(QStringLiteral("playlist-view-sort-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
        const qint64 playlistId = db.createPlaylist(QStringLiteral("Sorted"));
        QVERIFY(playlistId > 0);
        const QVector<qint64> ids = addItems(db, playlistId, 60, [](int i) {
            return QStringLiteral("Title %1").arg(59 - i, 3, 10, QLatin1Char('0'));
        });
        QCOMPARE(ids.size(), 60);

        PlaylistView view;
        view.resize(900, 220);
        view.setDatabase(&db);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.selectPlaylist(playlistId);
        QCoreApplication::processEvents();
        QTableView *table = itemTable(view);
        QVERIFY(table != nullptr);
        establishState(*table, {5, 20, 50}, 35, 12);
        const QSet<QString> selectedBefore = selectedIdentities(*table);
        const QString currentBefore = currentIdentity(*table);
        const QPair<QString, int> anchorBefore = topAnchor(*table);
        const QHash<QString, int> rowsBefore = rowsByIdentity(*table);

        QVERIFY(QMetaObject::invokeMethod(table->horizontalHeader(), "sectionClicked",
                                          Qt::DirectConnection, Q_ARG(int, 1)));
        QCoreApplication::processEvents();

        const QHash<QString, int> rowsAfter = rowsByIdentity(*table);
        QCOMPARE(selectedIdentities(*table), selectedBefore);
        QCOMPARE(currentIdentity(*table), currentBefore);
        QCOMPARE(topAnchor(*table), anchorBefore);
        for (const QString &identity : selectedBefore) {
            QVERIFY(rowsAfter.value(identity) != rowsBefore.value(identity));
        }
        QVERIFY(rowsAfter.value(currentBefore) != rowsBefore.value(currentBefore));
        QVERIFY(rowsAfter.value(anchorBefore.first) != rowsBefore.value(anchorBefore.first));
    }

    void streamingImportRefreshPreservesIdentityState()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlaylistDatabase db(QStringLiteral("playlist-view-stream-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
        const qint64 playlistId = db.createPlaylist(QStringLiteral("Streaming"));
        QVERIFY(playlistId > 0);
        const QVector<qint64> ids = addItems(db, playlistId, 60, [](int i) {
            return QStringLiteral("Item %1").arg(i, 3, 10, QLatin1Char('0'));
        });
        QCOMPARE(ids.size(), 60);

        PlaylistView view;
        view.resize(900, 220);
        view.setDatabase(&db);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.selectPlaylist(playlistId);
        QCoreApplication::processEvents();
        QTableView *table = itemTable(view);
        QVERIFY(table != nullptr);
        QVERIFY(QMetaObject::invokeMethod(table->horizontalHeader(), "sectionClicked",
                                          Qt::DirectConnection, Q_ARG(int, 1)));
        QCoreApplication::processEvents();
        establishState(*table, {15, 40}, 30, 20);
        const QSet<QString> selectedBefore = selectedIdentities(*table);
        const QString currentBefore = currentIdentity(*table);
        const QPair<QString, int> anchorBefore = topAnchor(*table);
        const QHash<QString, int> rowsBefore = rowsByIdentity(*table);

        PlaylistItem item;
        item.titleSnapshot = QStringLiteral("Aardvark");
        QVERIFY(db.addItem(playlistId, item) > 0);
        view.refreshImportingPlaylist(playlistId);
        QCoreApplication::processEvents();

        const QHash<QString, int> rowsAfter = rowsByIdentity(*table);
        QCOMPARE(selectedIdentities(*table), selectedBefore);
        QCOMPARE(currentIdentity(*table), currentBefore);
        QCOMPARE(topAnchor(*table), anchorBefore);
        for (const QString &identity : selectedBefore) {
            QVERIFY(rowsAfter.value(identity) != rowsBefore.value(identity));
        }
        QVERIFY(rowsAfter.value(currentBefore) != rowsBefore.value(currentBefore));
        QVERIFY(rowsAfter.value(anchorBefore.first) != rowsBefore.value(anchorBefore.first));
    }

    void tracklistStateIsKeyedByPlaylist()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlaylistDatabase db(QStringLiteral("playlist-view-keyed-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
        const qint64 firstId = db.createPlaylist(QStringLiteral("First"));
        const qint64 secondId = db.createPlaylist(QStringLiteral("Second"));
        QVERIFY(firstId > 0 && secondId > 0);
        QCOMPARE(addItems(db, firstId, 30, [](int i) { return QStringLiteral("First %1").arg(i); }).size(), 30);
        QCOMPARE(addItems(db, secondId, 30, [](int i) { return QStringLiteral("Second %1").arg(i); }).size(), 30);

        PlaylistView view;
        view.resize(900, 220);
        view.setDatabase(&db);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.selectPlaylist(firstId);
        QCoreApplication::processEvents();
        QTableView *table = itemTable(view);
        QVERIFY(table != nullptr);
        establishState(*table, {2, 9}, 6, 4);
        const QSet<QString> firstSelected = selectedIdentities(*table);
        const QString firstCurrent = currentIdentity(*table);
        const QPair<QString, int> firstAnchor = topAnchor(*table);

        view.selectPlaylist(secondId);
        QCoreApplication::processEvents();
        establishState(*table, {3, 14}, 10, 8);
        const QSet<QString> secondSelected = selectedIdentities(*table);
        const QString secondCurrent = currentIdentity(*table);
        const QPair<QString, int> secondAnchor = topAnchor(*table);

        view.selectPlaylist(firstId);
        QCoreApplication::processEvents();
        QCOMPARE(selectedIdentities(*table), firstSelected);
        QCOMPARE(currentIdentity(*table), firstCurrent);
        QCOMPARE(topAnchor(*table), firstAnchor);

        view.selectPlaylist(secondId);
        QCoreApplication::processEvents();
        QCOMPARE(selectedIdentities(*table), secondSelected);
        QCOMPARE(currentIdentity(*table), secondCurrent);
        QCOMPARE(topAnchor(*table), secondAnchor);
    }

    void idleReleasePreservesKeyedTracklistState()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlaylistDatabase db(QStringLiteral("playlist-view-idle-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
        const qint64 playlistId = db.createPlaylist(QStringLiteral("Idle"));
        QVERIFY(playlistId > 0);
        QCOMPARE(addItems(db, playlistId, 50, [](int i) { return QStringLiteral("Idle %1").arg(i); }).size(), 50);

        PlaylistView view(nullptr, 30);
        view.resize(900, 220);
        view.setDatabase(&db);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.selectPlaylist(playlistId);
        QCoreApplication::processEvents();
        QTableView *table = itemTable(view);
        QVERIFY(table != nullptr);
        establishState(*table, {7, 22}, 15, 12);
        const QSet<QString> selectedBefore = selectedIdentities(*table);
        const QString currentBefore = currentIdentity(*table);
        const QPair<QString, int> anchorBefore = topAnchor(*table);

        view.hide();
        QTest::qWait(80);
        QVERIFY(table->model()->rowCount() == 0);

        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.reloadPlaylists();
        QCoreApplication::processEvents();
        QVERIFY(table->model()->rowCount() > 0);
        QCOMPARE(selectedIdentities(*table), selectedBefore);
        QCOMPARE(currentIdentity(*table), currentBefore);
        QCOMPARE(topAnchor(*table), anchorBefore);
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

    void itemRadioMenuEmitsSelectedPathsInDisplayOrder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlaylistDatabase db(QStringLiteral("playlist-view-radio-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
        const qint64 playlistId = db.createPlaylist(QStringLiteral("Radio seeds"));
        QVERIFY(playlistId > 0);
        const QStringList paths{QStringLiteral("/first.flac"), QStringLiteral("/second.flac"), QStringLiteral("/third.flac")};
        for (const QString &path : paths) {
            PlaylistItem item;
            item.trackPath = path;
            item.titleSnapshot = path;
            item.status = PlaylistItemStatus::Matched;
            QVERIFY(db.addItem(playlistId, item) > 0);
        }

        PlaylistView view;
        view.setDatabase(&db);
        view.setTrackResolver([](const QString &path) {
            Track track;
            track.path = path;
            return track;
        });
        view.selectPlaylist(playlistId);
        view.resize(900, 420);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QTableView *table = itemTable(view);
        QVERIFY(table != nullptr);
        QCoreApplication::processEvents();
        QVERIFY(table->model()->rowCount() == paths.size());
        table->selectionModel()->clearSelection();
        for (const int row : {2, 0, 1}) {
            table->selectionModel()->select(table->model()->index(row, 0),
                                           QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
        table->selectionModel()->setCurrentIndex(table->model()->index(1, 0), QItemSelectionModel::NoUpdate);

        QSignalSpy startSpy(&view, &PlaylistView::startRadioRequested);
        bool sawAction = false;
        QTimer::singleShot(0, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (menu == nullptr) {
                return;
            }
            for (QAction *action : menu->actions()) {
                if (action->text() == QStringLiteral("Start Radio (3)")) {
                    sawAction = true;
                    action->trigger();
                    break;
                }
            }
            menu->close();
        });

        const QRect rowRect = table->visualRect(table->model()->index(1, 0));
        QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
                                          Qt::DirectConnection,
                                          Q_ARG(QPoint, rowRect.center())));
        QVERIFY(sawAction);
        QCOMPARE(startSpy.count(), 1);
        QCOMPARE(qvariant_cast<QStringList>(startSpy.first().at(0)), paths);
    }

    void emptyItemMenuOffersPlaylistActions()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        PlaylistDatabase db(QStringLiteral("playlist-view-empty-menu-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(db.open(dir.filePath(QStringLiteral("playlists.sqlite"))));
        const qint64 playlistId = db.createPlaylist(QStringLiteral("Empty"));
        QVERIFY(playlistId > 0);

        PlaylistView view;
        view.resize(900, 260);
        view.setDatabase(&db);
        view.selectPlaylist(playlistId);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        auto *table = view.findChild<QTableView *>();
        QVERIFY(table != nullptr);

        QStringList actions;
        QList<bool> enabled;
        bool sawMenu = false;
        QTimer::singleShot(0, [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (menu == nullptr) {
                return;
            }
            sawMenu = true;
            for (QAction *action : menu->actions()) {
                if (!action->isSeparator()) {
                    actions << action->text();
                    enabled << action->isEnabled();
                }
            }
            menu->close();
        });

        QVERIFY(QMetaObject::invokeMethod(table, "customContextMenuRequested",
                                          Qt::DirectConnection,
                                          Q_ARG(QPoint, QPoint(12, 80))));
        QVERIFY(sawMenu);
        QCOMPARE(actions, (QStringList{
                              QStringLiteral("Add song…"),
                              QStringLiteral("Import into this playlist…"),
                              QStringLiteral("Play playlist"),
                              QStringLiteral("New playlist…"),
                          }));
        QCOMPARE(enabled, (QList<bool>{true, true, true, true}));
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
