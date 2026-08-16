#include "scrobble/ScrobbleDestination.h"
#include "ui/ListeningHistoryPanel.h"
#include "ui/ScrobblersPanel.h"
#include "ui/ScrobblingDialog.h"

#include <QAction>
#include <QLabel>
#include <QPushButton>
#include <QApplication>
#include <QDialogButtonBox>

#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>

#include "scrobble/ListenHistoryStore.h"
#include "ui/StickyMenu.h"

// Both scrobbling menu entries open one window; which entry was used decides
// only which tab it opens on.
class ScrobblingDialogTest final : public QObject {
    Q_OBJECT

private slots:
    void carriesBothPanelsAsTabs();
    void eachEntryPointOpensItsOwnTab();
    void theTabStripDoesNotOpenWearingTheFocus();
    void aBacklogClearedInOneTabUpdatesTheOther();
    void aDestinationRemovedInOneTabLeavesTheOther();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<ListenHistoryStore> m_store;
    ScrobbleDestinationSet m_destinations;
    QString m_koitoId;

    // A dialog over a real store, so the two tabs have something to disagree
    // about.
    std::unique_ptr<ScrobblingDialog> makeLiveDialog()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        m_store = std::make_unique<ListenHistoryStore>(m_dir->filePath(QStringLiteral("history.sqlite")));
        m_destinations = ScrobbleDestinationConfig::defaults();
        m_koitoId =
            m_destinations.addCustom(QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), true);

        Track track;
        track.title = QStringLiteral("One");
        track.path = QStringLiteral("/music/one.flac");
        track.durationMs = 200000;
        m_store->recordListen(track, 1000, {m_koitoId});

        ScrobblersPanel::Callbacks callbacks = liveCallbacks();
        auto *scrobblers = new ScrobblersPanel(m_destinations, m_store.get(), callbacks);
        auto *history = new ListeningHistoryPanel(m_store.get(), m_destinations);
        return std::make_unique<ScrobblingDialog>(scrobblers, history);
    }

    ScrobblersPanel::Callbacks liveCallbacks()
    {
        ScrobblersPanel::Callbacks callbacks;
        callbacks.readToken = [](const QString &) { return QString(); };
        callbacks.writeToken = [](const QString &, const QString &) {};
        callbacks.removeToken = [](const QString &) {};
        callbacks.saveDestinations = [](const ScrobbleDestinationSet &) {};
        callbacks.lastFmConfigured = []() { return false; };
        callbacks.testDestination = [](const QString &, quint64, const QString &, const QString &) {};
        callbacks.readOffline = []() { return false; };
        callbacks.writeOffline = [](bool) {};
        callbacks.openLastFmSettings = []() {};
        return callbacks;
    }

    std::unique_ptr<ScrobblingDialog> makeDialog()
    {
        ScrobblersPanel::Callbacks callbacks;
        callbacks.readToken = [](const QString &) { return QString(); };
        callbacks.writeToken = [](const QString &, const QString &) {};
        callbacks.removeToken = [](const QString &) {};
        callbacks.saveDestinations = [](const ScrobbleDestinationSet &) {};
        callbacks.lastFmConfigured = []() { return false; };
        callbacks.testDestination = [](const QString &, quint64, const QString &, const QString &) {};
        callbacks.readOffline = []() { return false; };
        callbacks.writeOffline = [](bool) {};
        callbacks.openLastFmSettings = []() {};

        const ScrobbleDestinationSet destinations = ScrobbleDestinationConfig::defaults();
        return std::make_unique<ScrobblingDialog>(new ScrobblersPanel(destinations, nullptr, callbacks),
                                                  new ListeningHistoryPanel(nullptr, destinations));
    }
};

void ScrobblingDialogTest::carriesBothPanelsAsTabs()
{
    const auto dialog = makeDialog();
    auto *tabs = dialog->findChild<QTabWidget *>();
    QVERIFY(tabs != nullptr);
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->tabText(0), QStringLiteral("Scrobblers"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("Listening history"));
    QVERIFY(qobject_cast<ScrobblersPanel *>(tabs->widget(0)) != nullptr);
    QVERIFY(qobject_cast<ListeningHistoryPanel *>(tabs->widget(1)) != nullptr);
}

void ScrobblingDialogTest::eachEntryPointOpensItsOwnTab()
{
    const auto dialog = makeDialog();
    auto *tabs = dialog->findChild<QTabWidget *>();

    dialog->showTab(ScrobblingDialog::Tab::History);
    QCOMPARE(tabs->currentIndex(), 1);
    dialog->showTab(ScrobblingDialog::Tab::Scrobblers);
    QCOMPARE(tabs->currentIndex(), 0);
}

// The pending count in one tab is the thing the other tab's actions change,
// which is the whole reason they share a window.
void ScrobblingDialogTest::aBacklogClearedInOneTabUpdatesTheOther()
{
    const auto dialog = makeLiveDialog();
    auto *pending = dialog->findChild<QLabel *>(m_koitoId + QStringLiteral(".pending"));
    QVERIFY(pending != nullptr);
    QCOMPARE(pending->text(), QStringLiteral("1"));

    auto *history = dialog->findChild<ListeningHistoryPanel *>();
    auto *menu = history->findChild<StickyMenu *>();
    for (QAction *action : menu->actions()) {
        if (action->data().toString() == m_koitoId) {
            action->trigger();
        }
    }
    for (QPushButton *candidate : history->findChildren<QPushButton *>()) {
        if (candidate->text() == QStringLiteral("Clear backlog")) {
            QVERIFY(candidate->isEnabled());
            candidate->click();
        }
    }

    QCOMPARE(m_store->pendingCount(m_koitoId), 0);
    QCOMPARE(pending->text(), QStringLiteral("—"));
}

void ScrobblingDialogTest::aDestinationRemovedInOneTabLeavesTheOther()
{
    const auto dialog = makeLiveDialog();
    auto *scrobblers = dialog->findChild<ScrobblersPanel *>();
    auto *history = dialog->findChild<ListeningHistoryPanel *>();
    auto *menu = history->findChild<StickyMenu *>();

    const auto offers = [menu](const QString &id) {
        for (const QAction *action : menu->actions()) {
            if (action->data().toString() == id) {
                return true;
            }
        }
        return false;
    };
    QVERIFY(offers(m_koitoId));

    // Saving without Koito is what removal amounts to from the history's side.
    emit scrobblers->destinationsChanged(ScrobbleDestinationConfig::defaults());
    QVERIFY(!offers(m_koitoId));
}

// The strip is first in the focus chain, so left alone it opens wearing the
// style's focus marks: a ring around the tab and a rule under its label,
// directly above the strip's own. A window that has only just appeared has
// nothing to say with them.
void ScrobblingDialogTest::theTabStripDoesNotOpenWearingTheFocus()
{
    const auto dialog = makeDialog();
    dialog->show();
    QCoreApplication::processEvents();

    auto *bar = dialog->findChild<QTabBar *>();
    QVERIFY(bar != nullptr);
    QVERIFY(!bar->hasFocus());

    // Something else holds it, rather than nothing holding it: a dialog whose
    // focus went nowhere would also pass a bare !hasFocus().
    auto *close = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Close);
    QVERIFY(close != nullptr);
    QCOMPARE(QApplication::focusWidget(), close);

    // Still reachable, so a keyboard user can get to it. That they are then
    // shown they have is the style's doing rather than this window's: the strip
    // carries no focus rule of its own. It is drawn from the widget's focus
    // state and only while the window is active, which is not a condition an
    // offscreen suite can hold, so it is checked by rendering the window rather
    // than asserted here.
    QVERIFY(bar->focusPolicy() & Qt::TabFocus);
}

QTEST_MAIN(ScrobblingDialogTest)
#include "test_scrobbling_dialog.moc"
