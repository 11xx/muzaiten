#include <QLineEdit>
#include <QTest>
#include <QWidget>

#include "search/SearchMatcher.h"
#include "ui/KeybindCatalog.h"
#include "ui/MainPanelKeybindings.h"
#include "ui/PanelSearchController.h"

// Backing store for a fake "Artists"-style panel: a flat list of names plus the
// currently highlighted row, all driven through the MainPanelTarget callbacks.
struct FakePanel {
    QStringList names;
    int currentRow = 0;

    MainPanelTarget makeTarget(QWidget *focusWidget)
    {
        MainPanelTarget target;
        target.id = MainPanelId::Artists;
        target.label = QStringLiteral("Artists");
        target.focusWidget = focusWidget;
        target.rowCount = [this]() { return static_cast<int>(names.size()); };
        target.currentRow = [this]() { return currentRow; };
        target.setCurrentRow = [this](int row) { currentRow = row; };
        target.setCurrentRowWithDirection = [this](int row, int) { currentRow = row; };
        target.documents = [this]() {
            QVector<Search::MatchDocument> docs;
            for (int i = 0; i < names.size(); ++i) {
                Search::MatchDocument doc;
                doc.row = i;
                doc.fields.push_back(Search::makeField(Search::MatchFieldRole::Artist, names.at(i), 100));
                docs.push_back(doc);
            }
            return docs;
        };
        return target;
    }
};

class TestPanelSearch : public QObject {
    Q_OBJECT

private slots:
    // Typing a space must land in the query box. A regression guard against the
    // old trimming that swallowed every trailing space on each keystroke.
    void spaceSurvivesTyping()
    {
        FakePanel panel{{QStringLiteral("Alpha"), QStringLiteral("Beatles"), QStringLiteral("Beta")}, 0};
        QWidget focusWidget;
        PanelSearchController controller;
        QLineEdit *edit = controller.findChild<QLineEdit *>();
        QVERIFY(edit != nullptr);

        controller.registerTarget(panel.makeTarget(&focusWidget));
        controller.setActivePanel(MainPanelId::Artists, false);
        controller.activateForMainView();

        QTest::keyClick(&focusWidget, Qt::Key_Slash);  // open the search bar
        QTest::keyClicks(edit, QStringLiteral("be at"));

        QCOMPARE(edit->text(), QStringLiteral("be at"));
    }

    // Confirming a query that has matches keeps it alive so M-n / M-p keep
    // cycling through the matches after the bar closes.
    void confirmWithMatchesKeepsCycling()
    {
        FakePanel panel{{QStringLiteral("Apple"), QStringLiteral("Banana"), QStringLiteral("Cavailable")}, 0};
        QWidget focusWidget;
        PanelSearchController controller;
        QLineEdit *edit = controller.findChild<QLineEdit *>();
        QVERIFY(edit != nullptr);

        controller.registerTarget(panel.makeTarget(&focusWidget));
        controller.setActivePanel(MainPanelId::Artists, false);
        controller.activateForMainView();

        QTest::keyClick(&focusWidget, Qt::Key_Slash);
        QTest::keyClicks(edit, QStringLiteral("a"));  // matches every row
        QCOMPARE(panel.currentRow, 0);

        QTest::keyClick(edit, Qt::Key_Return);  // confirm
        QVERIFY(!controller.isVisible());

        QTest::keyClick(&focusWidget, Qt::Key_N, Qt::AltModifier);  // M-n
        QCOMPARE(panel.currentRow, 1);
        QTest::keyClick(&focusWidget, Qt::Key_N, Qt::AltModifier);
        QCOMPARE(panel.currentRow, 2);
        QTest::keyClick(&focusWidget, Qt::Key_P, Qt::AltModifier);  // M-p back
        QCOMPARE(panel.currentRow, 1);
    }

    // Confirming with no matches closes without leaving a constraint behind, so a
    // following M-n simply reopens the search bar rather than cycling nothing.
    void confirmWithoutMatchesDropsQuery()
    {
        FakePanel panel{{QStringLiteral("Alpha"), QStringLiteral("Beta")}, 0};
        QWidget focusWidget;
        PanelSearchController controller;
        QLineEdit *edit = controller.findChild<QLineEdit *>();
        QVERIFY(edit != nullptr);

        controller.registerTarget(panel.makeTarget(&focusWidget));
        controller.setActivePanel(MainPanelId::Artists, false);
        controller.activateForMainView();

        QTest::keyClick(&focusWidget, Qt::Key_Slash);
        QTest::keyClicks(edit, QStringLiteral("zzzz"));  // matches nothing
        QTest::keyClick(edit, Qt::Key_Return);

        QVERIFY(!controller.isVisible());
        QCOMPARE(panel.currentRow, 0);

        QTest::keyClick(&focusWidget, Qt::Key_N, Qt::AltModifier);  // no query left
        QVERIFY(controller.isVisible());
    }

    // The album grid's own keys yield to keys the user gave another action, and
    // any key the user gave Play now plays.
    void albumGridHonorsReassignedKeys()
    {
        using namespace MainPanelAction;
        FakePanel panel{{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}, 0};
        QWidget grid;
        PanelSearchController controller;
        MainPanelTarget target = panel.makeTarget(&grid);
        target.id = MainPanelId::Albums;
        target.label = QStringLiteral("Albums");
        int activated = 0;
        int marked = 0;
        int played = 0;
        target.activateCurrent = [&activated]() { ++activated; };
        target.markCurrent = [&marked]() { ++marked; };
        target.playCurrentNow = [&played]() { ++played; };
        target.moveCurrentInGrid = [&panel](int horizontal, int) { panel.currentRow += horizontal; };
        controller.registerTarget(target);
        controller.setActivePanel(MainPanelId::Albums, false);
        controller.activateForMainView();
        controller.setKeyBindingOverrides(
            {{QString::fromLatin1(MoveDown), {QKeySequence(Qt::Key_J), QKeySequence(Qt::Key_Down)}},
             {QString::fromLatin1(Mark), {QKeySequence(Qt::Key_M), QKeySequence(Qt::Key_N)}},
             {QString::fromLatin1(PlayNow),
              {QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter), QKeySequence(Qt::AltModifier | Qt::Key_Return),
               QKeySequence(Qt::AltModifier | Qt::Key_Enter), QKeySequence(Qt::ControlModifier | Qt::Key_L)}}});

        QTest::keyClick(&grid, Qt::Key_N);
        QCOMPARE(activated, 0);
        QCOMPARE(marked, 1);
        QTest::keyClick(&grid, Qt::Key_L, Qt::ControlModifier);
        QCOMPARE(played, 1);
        // Preset keys the user left alone keep the grid's meaning.
        QTest::keyClick(&grid, Qt::Key_Return);
        QCOMPARE(activated, 1);
        QCOMPARE(played, 1);
    }

    void removedKeysDoNothing()
    {
        using namespace MainPanelAction;
        QStringList names;
        for (int i = 0; i < 40; ++i) {
            names.push_back(QString::number(i));
        }
        FakePanel panel{names, 5};
        QWidget focusWidget;
        PanelSearchController controller;
        controller.registerTarget(panel.makeTarget(&focusWidget));
        controller.setActivePanel(MainPanelId::Artists, false);
        controller.activateForMainView();
        controller.setKeyBindingOverrides({{QString::fromLatin1(PageDown), {}}, {QString::fromLatin1(MoveDown), {}}});

        QTest::keyClick(&focusWidget, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(panel.currentRow, 5);
        QTest::keyClick(&focusWidget, Qt::Key_PageDown);
        QCOMPARE(panel.currentRow, 5);
        QTest::keyClick(&focusWidget, Qt::Key_J);
        QCOMPARE(panel.currentRow, 5);
        QTest::keyClick(&focusWidget, Qt::Key_K);
        QCOMPARE(panel.currentRow, 4);
    }

    // A text key given to a search command types into the query instead.
    void searchFieldKeepsTextKeys()
    {
        FakePanel panel{{QStringLiteral("Axe"), QStringLiteral("Box")}, 0};
        QWidget focusWidget;
        PanelSearchController controller;
        QLineEdit *edit = controller.findChild<QLineEdit *>();
        QVERIFY(edit != nullptr);
        controller.registerTarget(panel.makeTarget(&focusWidget));
        controller.setActivePanel(MainPanelId::Artists, false);
        controller.activateForMainView();
        controller.setKeyBindingOverrides(
            {{QString::fromLatin1(MainPanelAction::SearchNext), {QKeySequence(Qt::Key_X)}}});

        QTest::keyClick(&focusWidget, Qt::Key_Slash);
        QTest::keyClicks(edit, QStringLiteral("ax"));
        QCOMPARE(edit->text(), QStringLiteral("ax"));
    }

    // Return given to Close search clears the query instead of confirming it.
    void returnRunsAnEditedSearchCommand()
    {
        FakePanel panel{{QStringLiteral("Axe"), QStringLiteral("Box")}, 0};
        QWidget focusWidget;
        PanelSearchController controller;
        QLineEdit *edit = controller.findChild<QLineEdit *>();
        QVERIFY(edit != nullptr);
        controller.registerTarget(panel.makeTarget(&focusWidget));
        controller.setActivePanel(MainPanelId::Artists, false);
        controller.activateForMainView();
        controller.setKeyBindingOverrides(
            {{QString::fromLatin1(MainPanelAction::Escape), {QKeySequence(Qt::Key_Return)}}});

        QTest::keyClick(&focusWidget, Qt::Key_Slash);
        QTest::keyClicks(edit, QStringLiteral("box"));
        QTest::keyClick(edit, Qt::Key_Return);
        QVERIFY(controller.isVisible());
        QVERIFY(edit->text().isEmpty());
    }
};

QTEST_MAIN(TestPanelSearch)
#include "test_panel_search.moc"
