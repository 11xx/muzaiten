#include "scrobble/ListenHistoryStore.h"
#include "scrobble/ScrobbleDestination.h"
#include "ui/ListeningHistoryPanel.h"
#include "ui/StickyMenu.h"

#include <QAction>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>

namespace {

QPushButton *button(QWidget *parent, const QString &text)
{
    for (QPushButton *candidate : parent->findChildren<QPushButton *>()) {
        if (candidate->text() == text) {
            return candidate;
        }
    }
    return nullptr;
}

QString summaryText(QWidget *parent)
{
    // The summary is the first label that actually says something about counts.
    for (QLabel *label : parent->findChildren<QLabel *>()) {
        if (label->text().contains(QLatin1String("listens"))) {
            return label->text();
        }
    }
    return {};
}

// Picking a destination is a click on its entry, which toggles it.
QAction *destinationAction(QWidget *panel, const QString &destinationId)
{
    auto *menu = panel->findChild<StickyMenu *>();
    if (menu == nullptr) {
        return nullptr;
    }
    for (QAction *action : menu->actions()) {
        if (action->data().toString() == destinationId) {
            return action;
        }
    }
    return nullptr;
}

Track makeTrack(const QString &title)
{
    Track track;
    track.title = title;
    track.artistName = QStringLiteral("Artist");
    track.path = QStringLiteral("/music/%1.flac").arg(title);
    track.durationMs = 200000;
    return track;
}

}   // namespace

class ListeningHistoryPanelTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void offersEveryDestinationAndAnOverview();
    void theOverviewSummarizesAndDisablesMutations();
    void oneDestinationScopesCountsAndActions();
    void severalDestinationsAreScopedTogether();
    void theOverviewReportsDeliveryPerListen();
    void queueingSeveralDestinationsMarksEachOne();
    void clearingSeveralBacklogsClearsEachOne();
    void adoptingAChangedSetKeepsSurvivingPicks();
    void togglingADestinationDoesNotDismissTheMenu();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<ListenHistoryStore> m_store;
    ScrobbleDestinationSet m_destinations;
    QString m_koitoId;

    std::unique_ptr<ListeningHistoryPanel> makePanel()
    {
        return std::make_unique<ListeningHistoryPanel>(m_store.get(), m_destinations, nullptr);
    }
};

void ListeningHistoryPanelTest::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    m_store = std::make_unique<ListenHistoryStore>(m_dir->filePath(QStringLiteral("history.sqlite")));
    QVERIFY(m_store->isOpen());

    m_destinations = ScrobbleDestinationConfig::defaults();
    m_koitoId = m_destinations.addCustom(QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), true);

    // Two listens owed to Last.fm and Koito; one of them already delivered to
    // Koito. Official ListenBrainz is owed nothing.
    const qint64 first =
        m_store->recordListen(makeTrack(QStringLiteral("One")), 1000, {ListenHistoryStore::LastFm, m_koitoId});
    m_store->recordListen(makeTrack(QStringLiteral("Two")), 2000, {ListenHistoryStore::LastFm, m_koitoId});
    m_store->markSent(m_koitoId, {first});
}

void ListeningHistoryPanelTest::offersEveryDestinationAndAnOverview()
{
    const auto panel = makePanel();
    auto *menu = panel->findChild<StickyMenu *>();
    QVERIFY(menu != nullptr);

    QStringList ids;
    for (QAction *action : menu->actions()) {
        if (action->isCheckable()) {
            ids << action->data().toString();
        }
    }
    QCOMPARE(ids.size(), m_destinations.items.size());
    QVERIFY(ids.contains(ScrobbleDestinationConfig::lastFmId()));
    QVERIFY(ids.contains(ScrobbleDestinationConfig::listenBrainzId()));
    QVERIFY(ids.contains(m_koitoId));

    // Nothing picked is the overview, and it is what the panel opens on.
    QVERIFY(button(panel.get(), QStringLiteral("All destinations")) != nullptr);
}

void ListeningHistoryPanelTest::theOverviewSummarizesAndDisablesMutations()
{
    const auto panel = makePanel();

    // Four obligations exist (Last.fm twice, Koito twice) and one has been
    // delivered, so the overview reads 1 of 4.
    QVERIFY(summaryText(panel.get()).contains(QStringLiteral("1/4 sent")));

    // The overview is read-only: no destination is named, so there is no
    // backlog to act on.
    QVERIFY(!button(panel.get(), QStringLiteral("Clear backlog"))->isEnabled());
    QVERIFY(!button(panel.get(), QStringLiteral("Retry pending"))->isEnabled());
    QVERIFY(!button(panel.get(), QStringLiteral("Scrobble selected"))->isEnabled());
}

void ListeningHistoryPanelTest::oneDestinationScopesCountsAndActions()
{
    const auto panel = makePanel();

    destinationAction(panel.get(), m_koitoId)->trigger();
    QCOMPARE(m_store->pendingCount(m_koitoId), 1);
    const QString koito = summaryText(panel.get());
    QVERIFY(koito.contains(QStringLiteral("Koito")));
    QVERIFY(koito.contains(QStringLiteral("1 pending")));
    QVERIFY(koito.contains(QStringLiteral("1 sent")));
    QVERIFY(button(panel.get(), QStringLiteral("Clear backlog"))->isEnabled());
    QVERIFY(button(panel.get(), QStringLiteral("Retry pending"))->isEnabled());

    // Official ListenBrainz was never owed these listens, so it has nothing to
    // clear or retry even though the history is full.
    destinationAction(panel.get(), m_koitoId)->trigger();
    destinationAction(panel.get(), ScrobbleDestinationConfig::listenBrainzId())->trigger();
    QVERIFY(summaryText(panel.get()).contains(QStringLiteral("0 pending")));
    QVERIFY(!button(panel.get(), QStringLiteral("Clear backlog"))->isEnabled());
    QVERIFY(!button(panel.get(), QStringLiteral("Retry pending"))->isEnabled());
}

void ListeningHistoryPanelTest::severalDestinationsAreScopedTogether()
{
    const auto panel = makePanel();

    destinationAction(panel.get(), m_koitoId)->trigger();
    destinationAction(panel.get(), ScrobbleDestinationConfig::lastFmId())->trigger();

    // Koito owes one and Last.fm owes two, so the pair owes three; only Koito's
    // single delivery has landed.
    const QString summary = summaryText(panel.get());
    QVERIFY(summary.contains(QStringLiteral("2 destinations")));
    QVERIFY(summary.contains(QStringLiteral("3 pending")));
    QVERIFY(summary.contains(QStringLiteral("1 sent")));
    QVERIFY(button(panel.get(), QStringLiteral("Clear backlog"))->isEnabled());
    QVERIFY(button(panel.get(), QStringLiteral("2 of 3 destinations")) != nullptr);
}

// The overview answers for every destination, so a listen owed to two and
// delivered to one reads as a ratio rather than as an empty cell.
void ListeningHistoryPanelTest::theOverviewReportsDeliveryPerListen()
{
    const auto panel = makePanel();
    auto *view = panel->findChild<QTableView *>();
    QVERIFY(view != nullptr);

    const int scrobbled = 5;
    QCOMPARE(view->model()->headerData(scrobbled, Qt::Horizontal).toString(), QStringLiteral("Scrobbled"));

    QStringList cells;
    for (int row = 0; row < view->model()->rowCount(); ++row) {
        cells << view->model()->index(row, scrobbled).data().toString();
    }
    QCOMPARE(cells.size(), 2);
    // "One" is owed to Last.fm and Koito and has reached Koito; "Two" is owed
    // to both and has reached neither.
    QVERIFY(cells.contains(QStringLiteral("1/2 sent")));
    QVERIFY(cells.contains(QStringLiteral("0/2 sent")));
}

void ListeningHistoryPanelTest::queueingSeveralDestinationsMarksEachOne()
{
    const auto panel = makePanel();

    // ListenBrainz was owed nothing; picking it beside Koito must queue the
    // listens it is missing without touching what Koito already has. Picking
    // reloads the table, which drops any selection, so the rows are chosen
    // after the destinations are.
    destinationAction(panel.get(), m_koitoId)->trigger();
    destinationAction(panel.get(), ScrobbleDestinationConfig::listenBrainzId())->trigger();
    panel->findChild<QTableView *>()->selectAll();

    QVERIFY(button(panel.get(), QStringLiteral("Scrobble selected"))->isEnabled());

    QList<QPair<QString, int>> changes;
    connect(panel.get(), &ListeningHistoryPanel::backlogChanged, panel.get(),
            [&changes](const QString &service, int count) { changes.append({service, count}); });
    button(panel.get(), QStringLiteral("Scrobble selected"))->click();

    QCOMPARE(m_store->pendingCount(ScrobbleDestinationConfig::listenBrainzId()), 2);
    // Koito already owed both, so nothing there was queueable a second time.
    QCOMPARE(m_store->pendingCount(m_koitoId), 1);
    QCOMPARE(changes.size(), 2);
}

void ListeningHistoryPanelTest::clearingSeveralBacklogsClearsEachOne()
{
    const auto panel = makePanel();
    destinationAction(panel.get(), m_koitoId)->trigger();
    destinationAction(panel.get(), ScrobbleDestinationConfig::lastFmId())->trigger();

    QCOMPARE(m_store->pendingCount(m_koitoId), 1);
    QCOMPARE(m_store->pendingCount(ScrobbleDestinationConfig::lastFmId()), 2);
    button(panel.get(), QStringLiteral("Clear backlog"))->click();

    QCOMPARE(m_store->pendingCount(m_koitoId), 0);
    QCOMPARE(m_store->pendingCount(ScrobbleDestinationConfig::lastFmId()), 0);
}

void ListeningHistoryPanelTest::adoptingAChangedSetKeepsSurvivingPicks()
{
    const auto panel = makePanel();
    destinationAction(panel.get(), m_koitoId)->trigger();
    destinationAction(panel.get(), ScrobbleDestinationConfig::lastFmId())->trigger();

    // Koito is removed in the other tab; Last.fm keeps the pick it had, and
    // Koito is no longer offered or acted on.
    panel->setDestinations(ScrobbleDestinationConfig::defaults());

    QVERIFY(destinationAction(panel.get(), m_koitoId) == nullptr);
    QAction *lastFm = destinationAction(panel.get(), ScrobbleDestinationConfig::lastFmId());
    QVERIFY(lastFm != nullptr);
    QVERIFY(lastFm->isChecked());
    QVERIFY(button(panel.get(), QStringLiteral("Last.fm")) != nullptr);
}

void ListeningHistoryPanelTest::togglingADestinationDoesNotDismissTheMenu()
{
    StickyMenu menu;
    QAction *checkable = menu.addAction(QStringLiteral("Koito"));
    checkable->setCheckable(true);
    QAction *plain = menu.addAction(QStringLiteral("Show all destinations"));
    menu.show();
    QCoreApplication::processEvents();

    // A click, as the menu sees one: the press is what arms the release.
    const auto click = [&menu](QAction *action) {
        const QPoint at = menu.actionGeometry(action).center();
        for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, QPointF(at), QPointF(menu.mapToGlobal(at)), Qt::LeftButton, Qt::LeftButton, {});
            QCoreApplication::sendEvent(&menu, &event);
        }
    };

    click(checkable);
    QVERIFY(checkable->isChecked());
    QVERIFY(menu.isVisible());

    click(checkable);
    QVERIFY(!checkable->isChecked());
    QVERIFY(menu.isVisible());

    // An entry that is not a toggle still acts and closes, as any menu entry does.
    bool acted = false;
    connect(plain, &QAction::triggered, this, [&acted]() { acted = true; });
    click(plain);
    QVERIFY(acted);
    QVERIFY(!menu.isVisible());
}

QTEST_MAIN(ListeningHistoryPanelTest)
#include "test_listening_history_panel.moc"
