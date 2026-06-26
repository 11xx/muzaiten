#include <QtTest>
#include <QLineEdit>

#include "ui/PanelSearchBar.h"
#include "search/SearchMatcher.h"

// Drives the shared ncmpcpp-style search state machine: live filtering jumps to
// the first match, Return confirms while keeping the query so M-n/M-p cycle,
// cycling wraps, and Escape drops the query.
class TestPanelSearchBar : public QObject {
    Q_OBJECT

private:
    static QVector<Search::MatchDocument> docsFor(const QStringList &rows)
    {
        QVector<Search::MatchDocument> docs;
        for (int row = 0; row < rows.size(); ++row) {
            docs.push_back({row,
                            {Search::makeField(Search::MatchFieldRole::Free, rows.at(row), 100)},
                            {}});
        }
        return docs;
    }

private slots:
    void filtersConfirmsAndCycles();
    void escapeDropsQuery();
};

void TestPanelSearchBar::filtersConfirmsAndCycles()
{
    const QStringList rows{"apple", "banana", "apricot", "cherry"};
    int current = 0; // matches jump to the first one at/after the cursor

    PanelSearchBar bar;
    PanelSearchBar::Providers providers;
    providers.documents = [&] { return docsFor(rows); };
    providers.rowCount = [&] { return rows.size(); };
    providers.currentRow = [&] { return current; };
    providers.setCurrentRow = [&](int row) { current = row; };
    providers.focusList = [] {};
    bar.setProviders(providers);
    bar.setLabel(QStringLiteral("Test"));

    bar.open();
    QVERIFY(bar.isSearchVisible());

    auto *edit = bar.findChild<QLineEdit *>();
    QVERIFY(edit != nullptr);
    QTest::keyClicks(edit, "ap"); // matches apple(0) and apricot(2)
    QVERIFY(bar.hasActiveQuery());
    QCOMPARE(current, 0); // jumped to the first match

    // Return confirms: the bar hides but the query/matches stay resident.
    QTest::keyClick(edit, Qt::Key_Return);
    QVERIFY(!bar.isSearchVisible());
    QVERIFY(bar.hasActiveQuery());

    // M-n / M-p cycle through matches with the bar closed, and wrap around.
    bar.cycle(+1);
    QCOMPARE(current, 2);
    bar.cycle(+1);
    QCOMPARE(current, 0); // wrapped
    bar.cycle(-1);
    QCOMPARE(current, 2); // wrapped back
}

void TestPanelSearchBar::escapeDropsQuery()
{
    const QStringList rows{"alpha", "beta"};
    int current = 0;

    PanelSearchBar bar;
    PanelSearchBar::Providers providers;
    providers.documents = [&] { return docsFor(rows); };
    providers.rowCount = [&] { return rows.size(); };
    providers.currentRow = [&] { return current; };
    providers.setCurrentRow = [&](int row) { current = row; };
    providers.focusList = [] {};
    bar.setProviders(providers);

    bar.open();
    auto *edit = bar.findChild<QLineEdit *>();
    QTest::keyClicks(edit, "alp");
    QVERIFY(bar.hasActiveQuery());

    QTest::keyClick(edit, Qt::Key_Escape); // first Escape clears the text
    QVERIFY(!bar.hasActiveQuery());
    QVERIFY(bar.isSearchVisible());

    QTest::keyClick(edit, Qt::Key_Escape); // second dismisses the bar
    QVERIFY(!bar.isSearchVisible());
}

QTEST_MAIN(TestPanelSearchBar)
#include "test_panel_search_bar.moc"
