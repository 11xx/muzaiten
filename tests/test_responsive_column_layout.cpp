#include "ui/ResponsiveColumnLayout.h"
#include "ui/TrackTable.h"

#include <QApplication>
#include <QBrush>
#include <QFrame>
#include <QHeaderView>
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

        const QVariant fontValue = table.model()->headerData(2, Qt::Horizontal, Qt::FontRole);
        QVERIFY(fontValue.isValid());
        QCOMPARE(fontValue.value<QFont>().weight(), QFont::Normal);

        const QVariant brushValue = table.model()->headerData(2, Qt::Horizontal, Qt::ForegroundRole);
        QVERIFY(brushValue.isValid());
        QCOMPARE(brushValue.value<QBrush>().color(), QApplication::palette().color(QPalette::Disabled, QPalette::Text));
    }
};

QTEST_MAIN(ResponsiveColumnLayoutTest)

#include "test_responsive_column_layout.moc"
