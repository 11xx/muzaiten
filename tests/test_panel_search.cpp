#include <QLineEdit>
#include <QTest>
#include <QWidget>

#include "search/SearchMatcher.h"
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
};

QTEST_MAIN(TestPanelSearch)
#include "test_panel_search.moc"
