#include "ui/NavigableTableView.h"

#include <QHeaderView>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTest>

#include <memory>

class NavigationScrollTest final : public QObject {
    Q_OBJECT

private slots:
    void downMovementAnchorsAtBottomPadding();
    void upMovementAnchorsAtTopPadding();
    void directJumpUsesMinimalVisibilityScroll();

private:
    static NavigableTableView *makeView(QStandardItemModel *model);
};

NavigableTableView *NavigationScrollTest::makeView(QStandardItemModel *model)
{
    auto *view = new NavigableTableView;
    view->setModel(model);
    view->setFrameShape(QFrame::NoFrame);
    view->horizontalHeader()->hide();
    view->verticalHeader()->hide();
    view->verticalHeader()->setDefaultSectionSize(20);
    view->verticalHeader()->setMinimumSectionSize(20);
    for (int row = 0; row < model->rowCount(); ++row) {
        view->setRowHeight(row, 20);
    }
    view->setFixedSize(120, 200);
    view->show();
    const bool exposed = QTest::qWaitForWindowExposed(view);
    Q_UNUSED(exposed)
    return view;
}

void NavigationScrollTest::downMovementAnchorsAtBottomPadding()
{
    QStandardItemModel model(100, 1);
    std::unique_ptr<NavigableTableView> view(makeView(&model));
    view->setNavigationScrollPadding(3);

    view->setCurrentNavigationRow(6, +1);
    QCOMPARE(view->verticalScrollBar()->value(), 0);

    view->setCurrentNavigationRow(7, +1);
    QCOMPARE(view->verticalScrollBar()->value(), 1);

    view->setCurrentNavigationRow(8, +1);
    QCOMPARE(view->verticalScrollBar()->value(), 2);
}

void NavigationScrollTest::upMovementAnchorsAtTopPadding()
{
    QStandardItemModel model(100, 1);
    std::unique_ptr<NavigableTableView> view(makeView(&model));
    view->setNavigationScrollPadding(3);
    view->verticalScrollBar()->setValue(10);

    view->setCurrentNavigationRow(13, -1);
    QCOMPARE(view->verticalScrollBar()->value(), 10);

    view->setCurrentNavigationRow(12, -1);
    QCOMPARE(view->verticalScrollBar()->value(), 9);

    view->setCurrentNavigationRow(11, -1);
    QCOMPARE(view->verticalScrollBar()->value(), 8);
}

void NavigationScrollTest::directJumpUsesMinimalVisibilityScroll()
{
    QStandardItemModel model(100, 1);
    std::unique_ptr<NavigableTableView> view(makeView(&model));
    view->setNavigationScrollPadding(3);
    view->verticalScrollBar()->setValue(10);

    view->setCurrentNavigationRow(15, 0);
    QCOMPARE(view->verticalScrollBar()->value(), 10);

    view->setCurrentNavigationRow(9, 0);
    QCOMPARE(view->verticalScrollBar()->value(), 9);

    view->setCurrentNavigationRow(20, 0);
    QCOMPARE(view->verticalScrollBar()->value(), 11);
}

QTEST_MAIN(NavigationScrollTest)

#include "test_navigation_scroll.moc"
