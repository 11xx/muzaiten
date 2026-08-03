#include "ui/HeaderLabelStyle.h"
#include "ui/ResponsiveColumnLayout.h"
#include "ui/TableViewState.h"
#include "ui/TrackTable.h"

#include <QApplication>
#include <QBrush>
#include <QFrame>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTableView>
#include <QTest>

namespace {

QVector<ResponsiveColumnSpec> basicSpecs()
{
    return {
        {3, QStringLiteral("year"), 60, 40, ResponsiveColumnPriority::HideEarly},
        {2, QStringLiteral("album"), 120, 60, ResponsiveColumnPriority::Normal},
        {1, QStringLiteral("artist"), 120, 60, ResponsiveColumnPriority::Normal},
        {0, QStringLiteral("title"), 300, 80, ResponsiveColumnPriority::Keep, true},
    };
}

QSet<QString> allBasicKeys()
{
    return {QStringLiteral("title"), QStringLiteral("artist"), QStringLiteral("album"), QStringLiteral("year")};
}

void prepareView(QTableView *view, QStandardItemModel *model)
{
    model->setColumnCount(4);
    model->setRowCount(1);
    view->setModel(model);
    view->setFrameShape(QFrame::NoFrame);
    view->verticalHeader()->setVisible(false);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setMinimumSectionSize(1);
    view->resize(600, 120);
    view->show();
    QTest::qWait(0);
}

} // namespace

class ResponsiveColumnLayoutTest : public QObject {
    Q_OBJECT

private slots:
    void preservesBaselineAfterSqueezeRestore()
    {
        QTableView view;
        QStandardItemModel model;
        prepareView(&view, &model);
        ResponsiveColumnLayout layout(&view, basicSpecs());
        layout.setUserVisibleColumns(allBasicKeys());
        layout.relayout();

        QCOMPARE(view.columnWidth(0), 300);
        QCOMPARE(view.columnWidth(1), 120);
        QCOMPARE(view.columnWidth(2), 120);
        QCOMPARE(view.columnWidth(3), 60);

        view.resize(220, 120);
        layout.relayout();
        QVERIFY(view.columnWidth(0) <= 300);
        QVERIFY(view.isColumnHidden(2) || view.isColumnHidden(3));

        view.resize(600, 120);
        layout.relayout();
        QCOMPARE(view.columnWidth(0), 300);
        QCOMPARE(view.columnWidth(1), 120);
        QCOMPARE(view.columnWidth(2), 120);
        QCOMPARE(view.columnWidth(3), 60);
    }

    void temporaryAutoHideIsNotUserVisibility()
    {
        QTableView view;
        QStandardItemModel model;
        prepareView(&view, &model);
        ResponsiveColumnLayout layout(&view, basicSpecs());
        layout.setUserVisibleColumns(allBasicKeys());

        view.resize(180, 120);
        layout.relayout();
        QVERIFY(view.isColumnHidden(1) || view.isColumnHidden(2) || view.isColumnHidden(3));
        QCOMPARE(layout.userVisibleColumns(), allBasicKeys());
    }

    void wideExpansionOnlyResizesAbsorber()
    {
        QTableView view;
        QStandardItemModel model;
        prepareView(&view, &model);
        ResponsiveColumnLayout layout(&view, basicSpecs());
        layout.setUserVisibleColumns(allBasicKeys());

        view.resize(900, 120);
        layout.relayout();
        QCOMPARE(view.columnWidth(0), 600);
        QCOMPARE(view.columnWidth(1), 120);
        QCOMPARE(view.columnWidth(2), 120);
        QCOMPARE(view.columnWidth(3), 60);
    }

    void deferredRelayoutRefillsViewportAfterShow()
    {
        QTableView view;
        QStandardItemModel model;
        prepareView(&view, &model);
        ResponsiveColumnLayout layout(&view, basicSpecs());
        layout.setUserVisibleColumns(allBasicKeys());
        QTest::qWait(0); // let the platform settle the real viewport width
        layout.relayout();

        const auto visibleSum = [&]() {
            int sum = 0;
            for (int c = 0; c < model.columnCount(); ++c) {
                if (!view.isColumnHidden(c)) {
                    sum += view.columnWidth(c);
                }
            }
            return sum;
        };

        const int filled = view.columnWidth(0);
        QCOMPARE(visibleSum(), view.viewport()->width()); // fills, no blank space

        // Reproduce the startup race: relayout() ran while the table was still
        // hidden during loadViewSettings(), so it laid columns out against the
        // baseline-sum width and left the absorber short — columns bunched at the
        // left with empty space on the right.
        view.setColumnWidth(0, filled - 100);
        QVERIFY(visibleSum() < view.viewport()->width()); // empty space on the right

        // The deferred relayout scheduled on the viewport's Show event must
        // recompute against the real width once the event loop runs, refilling it.
        layout.scheduleDeferredRelayout();
        QTest::qWait(0);
        QCOMPARE(view.columnWidth(0), filled);
        QCOMPARE(visibleSum(), view.viewport()->width());
    }

    void deferredRelayoutRefillsViewportAfterAbruptResize()
    {
        QTableView view;
        QStandardItemModel model;
        prepareView(&view, &model);
        ResponsiveColumnLayout layout(&view, basicSpecs());
        layout.setUserVisibleColumns(allBasicKeys());
        QTest::qWait(0);
        layout.relayout();

        const auto visibleSum = [&]() {
            int sum = 0;
            for (int c = 0; c < model.columnCount(); ++c) {
                if (!view.isColumnHidden(c)) {
                    sum += view.columnWidth(c);
                }
            }
            return sum;
        };

        view.resize(260, 120);
        QTest::qWait(0);
        layout.relayout();
        QVERIFY(view.columnWidth(0) < 300);

        view.resize(900, 120);
        view.setColumnWidth(0, 180);
        QVERIFY(visibleSum() < view.viewport()->width());

        QTest::qWait(0);
        QCOMPARE(visibleSum(), view.viewport()->width());
    }

    void priorityPersists()
    {
        QTableView view;
        QStandardItemModel model;
        prepareView(&view, &model);
        ResponsiveColumnLayout first(&view, basicSpecs());
        first.setColumnPriority(QStringLiteral("album"), ResponsiveColumnPriority::HideEarly);

        QJsonObject root;
        first.writePrioritiesJson(&root);

        QTableView secondView;
        QStandardItemModel secondModel;
        prepareView(&secondView, &secondModel);
        ResponsiveColumnLayout second(&secondView, basicSpecs());
        second.applyPrioritiesJson(root);

        QCOMPARE(second.columnPriority(QStringLiteral("album")), ResponsiveColumnPriority::HideEarly);
    }

    void minimumWidthPersistsAndControlsAbsorberShrink()
    {
        QTableView view;
        QStandardItemModel model;
        prepareView(&view, &model);
        ResponsiveColumnLayout first(&view, basicSpecs());
        first.setUserVisibleColumns(allBasicKeys());
        first.setColumnMinimumWidth(QStringLiteral("title"), 220);

        view.resize(460, 120);
        first.relayout();
        QCOMPARE(view.columnWidth(0), 220);
        QVERIFY(!view.isColumnHidden(1));
        QVERIFY(!view.isColumnHidden(2));
        QVERIFY(view.isColumnHidden(3));

        QJsonObject root;
        first.writeMinimumWidthsJson(&root);

        QTableView secondView;
        QStandardItemModel secondModel;
        prepareView(&secondView, &secondModel);
        ResponsiveColumnLayout second(&secondView, basicSpecs());
        second.applyMinimumWidthsJson(root);
        QCOMPARE(second.columnMinimumWidth(QStringLiteral("title")), 220);
    }

    void dropOrderPersistsAndControlsTieBreaks()
    {
        QTableView view;
        QStandardItemModel model;
        prepareView(&view, &model);
        ResponsiveColumnLayout first(&view, basicSpecs());
        first.setUserVisibleColumns({QStringLiteral("title"), QStringLiteral("artist"), QStringLiteral("album")});
        first.setDropOrderKeys({QStringLiteral("artist"), QStringLiteral("album"), QStringLiteral("title")});

        view.resize(240, 120);
        first.relayout();
        QVERIFY(view.isColumnHidden(1));
        QVERIFY(!view.isColumnHidden(2));

        QJsonObject root;
        first.writeDropOrderJson(&root);

        QTableView secondView;
        QStandardItemModel secondModel;
        prepareView(&secondView, &secondModel);
        ResponsiveColumnLayout second(&secondView, basicSpecs());
        second.applyDropOrderJson(root);
        QCOMPARE(second.dropOrderKeys().at(0), QStringLiteral("artist"));
    }

    void keepColumnsNeverAutoHide()
    {
        QTableView view;
        QStandardItemModel model;
        prepareView(&view, &model);
        ResponsiveColumnLayout layout(&view, basicSpecs());
        layout.setUserVisibleColumns(allBasicKeys());

        view.resize(20, 120);
        layout.relayout();
        QVERIFY(!view.isColumnHidden(0));
        QCOMPARE(view.horizontalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    }

    void restoreFromScrolledSqueezeClearsHiddenHorizontalOffset()
    {
        QTableView view;
        QStandardItemModel model;
        prepareView(&view, &model);
        ResponsiveColumnLayout layout(&view, basicSpecs());
        layout.setUserVisibleColumns(allBasicKeys());

        view.resize(20, 120);
        layout.relayout();
        QTest::qWait(0);
        QCOMPARE(view.horizontalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
        QVERIFY(view.horizontalScrollBar()->maximum() > 0);
        view.horizontalScrollBar()->setValue(view.horizontalScrollBar()->maximum());
        QVERIFY(view.horizontalScrollBar()->value() > 0);

        view.resize(600, 120);
        layout.relayout();

        QCOMPARE(view.horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QCOMPARE(view.horizontalScrollBar()->value(), 0);
    }

    void baselineUpdateSurvivesSqueezeRestore()
    {
        QTableView view;
        QStandardItemModel model;
        prepareView(&view, &model);
        ResponsiveColumnLayout layout(&view, basicSpecs());
        layout.setUserVisibleColumns(allBasicKeys());

        view.setColumnWidth(0, 260);
        view.setColumnWidth(1, 160);
        layout.updateBaselineWidthsForResize(0, 1);
        QCOMPARE(layout.baselineWidth(QStringLiteral("title")), 260);
        QCOMPARE(layout.baselineWidth(QStringLiteral("artist")), 160);

        view.resize(220, 120);
        layout.relayout();
        view.resize(600, 120);
        layout.relayout();
        QCOMPARE(view.columnWidth(0), 260);
        QCOMPARE(view.columnWidth(1), 160);
    }

    void trackTableUsesSharedInteractiveLayout()
    {
        TrackTable table;
        table.resize(900, 240);
        table.show();
        QTest::qWait(0);

        auto *layout = table.findChild<ResponsiveColumnLayout *>();
        QVERIFY(layout != nullptr);
        QCOMPARE(table.horizontalHeader()->sectionResizeMode(2), QHeaderView::Interactive);
        QVERIFY(layout->isResponsiveAbsorber(QStringLiteral("title")));
        QCOMPARE(layout->columnPriority(QStringLiteral("title")), ResponsiveColumnPriority::Keep);

        layout->setColumnPriority(QStringLiteral("year"), ResponsiveColumnPriority::Normal);
        table.resetViewSettings();
        QCOMPARE(layout->columnPriority(QStringLiteral("year")), ResponsiveColumnPriority::HideEarly);
    }

    void trackHeaderLabelsUseMutedThemeRoles()
    {
        TrackTable table;
        static constexpr HeaderViewStyle kHeaderStyle{
            HeaderLabelStyle{QFont::Normal, true, HeaderLabelTone::Muted, 0.20},
            false,
        };

        const QVariant fontValue = table.model()->headerData(2, Qt::Horizontal, Qt::FontRole);
        QVERIFY(fontValue.isValid());
        QCOMPARE(fontValue.value<QFont>().weight(), QFont::Normal);

        const QVariant brushValue = table.model()->headerData(2, Qt::Horizontal, Qt::ForegroundRole);
        QVERIFY(brushValue.isValid());
        QCOMPARE(brushValue.value<QBrush>().color(), headerLabelBrush(QApplication::palette(), kHeaderStyle.labels).color());
        QCOMPARE(table.horizontalHeader()->styleSheet(), headerViewStyleSheet(kHeaderStyle, table.horizontalHeader()));
    }

    void trackRefreshPreservesIdentityState()
    {
        constexpr int identityRole = TableViewState::IdentityRole;
        const auto makeTrack = [](int number, const QString &prefix) {
            Track track;
            const QString id = QStringLiteral("%1").arg(number, 3, 10, QLatin1Char('0'));
            track.path = QStringLiteral("/music/track-%1.flac").arg(id);
            track.title = QStringLiteral("%1 Track %2").arg(prefix, id);
            track.artistName = QStringLiteral("Artist %1").arg(prefix);
            return track;
        };
        const auto pathForRow = [identityRole](const QModelIndex &index) {
            return index.data(identityRole).toString();
        };

        TrackTable table;
        table.setFixedSize(700, 180);
        table.show();
        QTest::qWait(0);

        QVector<Track> initialTracks;
        for (int number = 0; number < 60; ++number) {
            initialTracks.push_back(makeTrack(number, QStringLiteral("Initial")));
        }
        table.setTracks(initialTracks);
        table.sortByColumn(2, Qt::AscendingOrder);

        const auto selectedPaths = [&table, pathForRow]() {
            QSet<QString> paths;
            for (const QModelIndex &index : table.selectionModel()->selectedRows()) {
                const QString path = pathForRow(index);
                if (!path.isEmpty()) {
                    paths.insert(path);
                }
            }
            return paths;
        };
        const auto topVisibleIndex = [&table]() {
            const QRect viewportRect = table.viewport()->rect();
            return table.indexAt(QPoint(viewportRect.left() + 1, viewportRect.top() + 1));
        };

        table.selectionModel()->clearSelection();
        for (const int row : {10, 20, 50}) {
            table.selectionModel()->select(table.model()->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
        table.selectionModel()->setCurrentIndex(table.model()->index(35, 0), QItemSelectionModel::NoUpdate);
        QCOMPARE(table.currentIndex().row(), 35);
        QVERIFY(selectedPaths().size() >= 2);

        table.scrollTo(table.model()->index(30, 0), QAbstractItemView::PositionAtTop);
        QTest::qWait(0);
        const QModelIndex topBefore = topVisibleIndex();
        QVERIFY(topBefore.isValid());
        QCOMPARE(topBefore.row(), 30);
        const QString topPath = pathForRow(topBefore);
        const int topOffset = table.visualRect(topBefore).top();
        const QSet<QString> selectedBefore = selectedPaths();
        const QString currentPath = pathForRow(table.currentIndex());
        QVERIFY(!currentPath.isEmpty());

        QVector<Track> refreshedTracks;
        for (int number = 59; number >= 0; --number) {
            refreshedTracks.push_back(makeTrack(number, QStringLiteral("Refreshed")));
        }
        table.setTracks(refreshedTracks);
        QTest::qWait(0);

        QVERIFY(selectedPaths() == selectedBefore);
        QCOMPARE(pathForRow(table.currentIndex()), currentPath);
        const QModelIndex topAfter = topVisibleIndex();
        QVERIFY(topAfter.isValid());
        QCOMPARE(pathForRow(topAfter), topPath);
        QCOMPARE(table.visualRect(topAfter).top(), topOffset);

        const QString removedSelectionPath = pathForRow(table.model()->index(10, 0));
        const int oldCurrentRow = table.currentIndex().row();
        QSet<QString> survivingPaths = selectedBefore;
        survivingPaths.remove(removedSelectionPath);
        survivingPaths.remove(currentPath);

        QVector<Track> reducedTracks;
        for (int number = 59; number >= 0; --number) {
            if (number == 10 || number == 35) {
                continue;
            }
            reducedTracks.push_back(makeTrack(number, QStringLiteral("Reduced")));
        }
        table.setTracks(reducedTracks);
        QTest::qWait(0);

        QVERIFY(!selectedPaths().contains(removedSelectionPath));
        QVERIFY(!selectedPaths().contains(currentPath));
        QVERIFY(selectedPaths() == survivingPaths);
        QCOMPARE(oldCurrentRow, 35);
        QCOMPARE(pathForRow(table.currentIndex()), QStringLiteral("/music/track-020.flac"));
        const QModelIndex topAfterRemoval = topVisibleIndex();
        QVERIFY(topAfterRemoval.isValid());
        QCOMPARE(pathForRow(topAfterRemoval), topPath);
        QCOMPARE(table.visualRect(topAfterRemoval).top(), topOffset);

        QVector<Track> anchorlessTracks;
        for (int number = 59; number >= 0; --number) {
            if (number == 10 || number == 30 || number == 35) {
                continue;
            }
            anchorlessTracks.push_back(makeTrack(number, QStringLiteral("Anchorless")));
        }
        table.setTracks(anchorlessTracks);
        QTest::qWait(0);
        QCOMPARE(table.verticalScrollBar()->value(), table.verticalScrollBar()->minimum());
    }

    void trackRefreshKeepsMarksAndRecomputesAutoHeight()
    {
        const auto makeTrack = [](int number) {
            Track track;
            track.path = QStringLiteral("/music/mark-%1.flac").arg(number);
            track.title = QStringLiteral("Track %1").arg(number);
            return track;
        };
        const auto selectedPaths = [](const TrackTable &table) {
            QSet<QString> paths;
            for (const QModelIndex &index : table.selectionModel()->selectedRows()) {
                paths.insert(index.data(TableViewState::IdentityRole).toString());
            }
            return paths;
        };

        TrackTable table;
        table.resize(700, 180);
        table.show();
        QTest::qWait(0);
        table.setAutoHeightToRows(true);
        table.setTracks({makeTrack(1), makeTrack(2)});
        QTest::qWait(0);
        table.setCurrentRow(1);
        table.markCurrentTrack();
        const int initialHeight = table.height();

        table.setTracks({makeTrack(2), makeTrack(3), makeTrack(4), makeTrack(1)});
        QTest::qWait(0);
        QVERIFY(table.height() > initialHeight);
        QCOMPARE(selectedPaths(table), QSet<QString>{QStringLiteral("/music/mark-2.flac")});

        table.setTracks({makeTrack(3), makeTrack(4), makeTrack(1)});
        QTest::qWait(0);
        QVERIFY(!selectedPaths(table).contains(QStringLiteral("/music/mark-2.flac")));

        table.setTracks({makeTrack(2), makeTrack(3), makeTrack(4), makeTrack(1)});
        QTest::qWait(0);
        QVERIFY(!selectedPaths(table).contains(QStringLiteral("/music/mark-2.flac")));
    }

    void autoHeightNavigationPreservesHorizontalScroll()
    {
        Track track;
        track.path = QStringLiteral("/music/auto-height.flac");
        track.title = QString(500, QLatin1Char('x'));

        QWidget host;
        TrackTable table(&host);
        host.resize(600, 120);
        table.setGeometry(host.rect());
        host.show();
        table.show();
        QTest::qWait(0);
        table.setTracks({track});
        QTest::qWait(0);
        host.resize(100, 120);
        table.setGeometry(host.rect());
        QTest::qWait(0);
        auto *layout = table.findChild<ResponsiveColumnLayout *>();
        QVERIFY(layout != nullptr);
        layout->setUserVisibleColumns({QStringLiteral("title"), QStringLiteral("year")});
        layout->setColumnPriority(QStringLiteral("year"), ResponsiveColumnPriority::Keep);
        layout->setColumnMinimumWidth(QStringLiteral("title"), 500);
        QTest::qWait(0);
        table.setAutoHeightToRows(true);
        QTest::qWait(0);
        layout->relayout();
        QTest::qWait(0);

        QVERIFY(table.horizontalScrollBar()->maximum() > 0);
        table.horizontalScrollBar()->setValue(table.horizontalScrollBar()->maximum());
        QVERIFY(table.horizontalScrollBar()->value() > 0);
        const int horizontalValue = table.horizontalScrollBar()->value();

        table.setCurrentRow(0);

        QCOMPARE(table.horizontalScrollBar()->value(), horizontalValue);
    }
};

QTEST_MAIN(ResponsiveColumnLayoutTest)

#include "test_responsive_column_layout.moc"
