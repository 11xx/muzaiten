#include "scrobble/ListenHistoryStore.h"
#include "scrobble/ScrobbleDestination.h"
#include "ui/ListeningHistoryDialog.h"
#include "ui/StickyMenu.h"

#include <QAction>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
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
QAction *destinationAction(QWidget *dialog, const QString &destinationId)
{
    auto *menu = dialog->findChild<StickyMenu *>();
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

class ListeningHistoryDialogTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void offersEveryDestinationAndAnOverview();
    void theOverviewSummarizesAndDisablesMutations();
    void oneDestinationScopesCountsAndActions();
    void severalDestinationsAreScopedTogether();
    void togglingADestinationDoesNotDismissTheMenu();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<ListenHistoryStore> m_store;
    ScrobbleDestinationSet m_destinations;
    QString m_koitoId;

    std::unique_ptr<ListeningHistoryDialog> makeDialog()
    {
        return std::make_unique<ListeningHistoryDialog>(m_store.get(), m_destinations, nullptr);
    }
};

void ListeningHistoryDialogTest::init()
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

void ListeningHistoryDialogTest::offersEveryDestinationAndAnOverview()
{
    const auto dialog = makeDialog();
    auto *menu = dialog->findChild<StickyMenu *>();
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

    // Nothing picked is the overview, and it is what the dialog opens on.
    QVERIFY(button(dialog.get(), QStringLiteral("All destinations")) != nullptr);
}

void ListeningHistoryDialogTest::theOverviewSummarizesAndDisablesMutations()
{
    const auto dialog = makeDialog();

    // Four obligations exist (Last.fm twice, Koito twice) and one has been
    // delivered, so the overview reads 1 of 4.
    QVERIFY(summaryText(dialog.get()).contains(QStringLiteral("1/4 sent")));

    // The overview is read-only: no destination is named, so there is no
    // backlog to act on.
    QVERIFY(!button(dialog.get(), QStringLiteral("Clear backlog"))->isEnabled());
    QVERIFY(!button(dialog.get(), QStringLiteral("Retry pending"))->isEnabled());
    QVERIFY(!button(dialog.get(), QStringLiteral("Scrobble selected"))->isEnabled());
}

void ListeningHistoryDialogTest::oneDestinationScopesCountsAndActions()
{
    const auto dialog = makeDialog();

    destinationAction(dialog.get(), m_koitoId)->trigger();
    QCOMPARE(m_store->pendingCount(m_koitoId), 1);
    const QString koito = summaryText(dialog.get());
    QVERIFY(koito.contains(QStringLiteral("Koito")));
    QVERIFY(koito.contains(QStringLiteral("1 pending")));
    QVERIFY(koito.contains(QStringLiteral("1 sent")));
    QVERIFY(button(dialog.get(), QStringLiteral("Clear backlog"))->isEnabled());
    QVERIFY(button(dialog.get(), QStringLiteral("Retry pending"))->isEnabled());

    // Official ListenBrainz was never owed these listens, so it has nothing to
    // clear or retry even though the history is full.
    destinationAction(dialog.get(), m_koitoId)->trigger();
    destinationAction(dialog.get(), ScrobbleDestinationConfig::listenBrainzId())->trigger();
    QVERIFY(summaryText(dialog.get()).contains(QStringLiteral("0 pending")));
    QVERIFY(!button(dialog.get(), QStringLiteral("Clear backlog"))->isEnabled());
    QVERIFY(!button(dialog.get(), QStringLiteral("Retry pending"))->isEnabled());
}

void ListeningHistoryDialogTest::severalDestinationsAreScopedTogether()
{
    const auto dialog = makeDialog();

    destinationAction(dialog.get(), m_koitoId)->trigger();
    destinationAction(dialog.get(), ScrobbleDestinationConfig::lastFmId())->trigger();

    // Koito owes one and Last.fm owes two, so the pair owes three; only Koito's
    // single delivery has landed.
    const QString summary = summaryText(dialog.get());
    QVERIFY(summary.contains(QStringLiteral("2 destinations")));
    QVERIFY(summary.contains(QStringLiteral("3 pending")));
    QVERIFY(summary.contains(QStringLiteral("1 sent")));
    QVERIFY(button(dialog.get(), QStringLiteral("Clear backlog"))->isEnabled());
    QVERIFY(button(dialog.get(), QStringLiteral("2 of 3 destinations")) != nullptr);
}

void ListeningHistoryDialogTest::togglingADestinationDoesNotDismissTheMenu()
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
}

QTEST_MAIN(ListeningHistoryDialogTest)
#include "test_listening_history_dialog.moc"
