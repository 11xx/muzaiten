#include "ui/NavigableTableView.h"
#include "ui/SelectionColors.h"

#include <QApplication>
#include <QCoreApplication>
#include <QHeaderView>
#include <QPalette>
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
    void inactivePanelDimsSelectionHighlight();
    void applicationPaletteChangeRefreshesViewAndHeaders();
    void hiddenApplicationPaletteChangeRefreshesView();

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

// QTableView paints the selected-row background itself through the palette
// Highlight role, so an out-of-focus panel must dim that role to mirror the
// dimming list-view delegates get for free. Guards against the highlight
// staying at full strength when the panel is inactive.
void NavigationScrollTest::inactivePanelDimsSelectionHighlight()
{
    QStandardItemModel model(10, 1);
    std::unique_ptr<NavigableTableView> view(makeView(&model));

    view->setMainPanelActive(true);
    const QColor activeHighlight = view->palette().color(QPalette::Highlight);
    const QColor base = view->palette().color(QPalette::Base);

    view->setMainPanelActive(false);
    const QColor inactiveHighlight = view->palette().color(QPalette::Highlight);

    QVERIFY(inactiveHighlight != activeHighlight);
    QCOMPARE(inactiveHighlight, SelectionColors::dimmedHighlight(base, activeHighlight));

    view->setMainPanelActive(true);
    QCOMPARE(view->palette().color(QPalette::Highlight), activeHighlight);
}

void NavigationScrollTest::applicationPaletteChangeRefreshesViewAndHeaders()
{
    QStandardItemModel model(10, 1);
    std::unique_ptr<NavigableTableView> view(makeView(&model));
    view->setMainPanelActive(true);

    const QPalette original = QApplication::palette();
    QPalette custom = original;
    custom.setColor(QPalette::Base, QColor(12, 34, 56));
    custom.setColor(QPalette::Button, QColor(65, 43, 21));
    custom.setColor(QPalette::Highlight, QColor(98, 76, 54));

    QApplication::setPalette(custom);
    QCoreApplication::processEvents();

    QCOMPARE(view->palette().color(QPalette::Base), custom.color(QPalette::Base));
    QCOMPARE(view->palette().color(QPalette::Highlight), custom.color(QPalette::Highlight));
    QCOMPARE(view->viewport()->palette().color(QPalette::Base), custom.color(QPalette::Base));
    QCOMPARE(view->horizontalHeader()->palette().color(QPalette::Button), custom.color(QPalette::Button));

    QApplication::setPalette(original);
    QCoreApplication::processEvents();
}

void NavigationScrollTest::hiddenApplicationPaletteChangeRefreshesView()
{
    QStandardItemModel model(10, 1);
    NavigableTableView view;
    view.setModel(&model);
    view.setMainPanelActive(true);

    const QPalette original = QApplication::palette();
    QPalette custom = original;
    custom.setColor(QPalette::Base, QColor(23, 45, 67));
    custom.setColor(QPalette::Highlight, QColor(89, 67, 45));

    QApplication::setPalette(custom);
    QCoreApplication::processEvents();

    QCOMPARE(view.palette().color(QPalette::Base), custom.color(QPalette::Base));
    QCOMPARE(view.palette().color(QPalette::Highlight), custom.color(QPalette::Highlight));

    QApplication::setPalette(original);
    QCoreApplication::processEvents();
}

QTEST_MAIN(NavigationScrollTest)

#include "test_navigation_scroll.moc"
