#include "ui/ResponsiveColumnLayout.h"
#include "ui/TrackTable.h"

#include <QFrame>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
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
};

QTEST_MAIN(ResponsiveColumnLayoutTest)

#include "test_responsive_column_layout.moc"
