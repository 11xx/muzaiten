#include "scrobble/ScrobbleDestination.h"
#include "ui/ListeningHistoryPanel.h"
#include "ui/ScrobblersPanel.h"
#include "ui/ScrobblingDialog.h"

#include <QTabWidget>
#include <QTest>

// Both scrobbling menu entries open one window; which entry was used decides
// only which tab it opens on.
class ScrobblingDialogTest final : public QObject {
    Q_OBJECT

private slots:
    void carriesBothPanelsAsTabs();
    void eachEntryPointOpensItsOwnTab();

private:
    std::unique_ptr<ScrobblingDialog> makeDialog()
    {
        ScrobblersPanel::Callbacks callbacks;
        callbacks.readToken = [](const QString &) { return QString(); };
        callbacks.writeToken = [](const QString &, const QString &) {};
        callbacks.removeToken = [](const QString &) {};
        callbacks.saveDestinations = [](const ScrobbleDestinationSet &) {};
        callbacks.lastFmConfigured = []() { return false; };
        callbacks.testDestination = [](const QString &, const QString &, const QString &) {};
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

QTEST_MAIN(ScrobblingDialogTest)
#include "test_scrobbling_dialog.moc"
