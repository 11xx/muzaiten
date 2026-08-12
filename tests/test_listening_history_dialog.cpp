#include "scrobble/ListenHistoryStore.h"
#include "scrobble/ScrobbleDestination.h"
#include "ui/ListeningHistoryDialog.h"

#include <QComboBox>
#include <QLabel>
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
    void listsEveryDestinationPlusTheAggregate();
    void aggregateSummarizesAndDisablesMutations();
    void aConcreteSelectionScopesCountsAndActions();

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

void ListeningHistoryDialogTest::listsEveryDestinationPlusTheAggregate()
{
    const auto dialog = makeDialog();
    auto *selector = dialog->findChild<QComboBox *>();
    QVERIFY(selector != nullptr);

    QCOMPARE(selector->count(), m_destinations.items.size() + 1);
    // The aggregate leads and carries no id, which is what marks it aggregate.
    QCOMPARE(selector->itemText(0), QStringLiteral("All destinations"));
    QVERIFY(selector->itemData(0).toString().isEmpty());

    QStringList ids;
    for (int index = 1; index < selector->count(); ++index) {
        ids << selector->itemData(index).toString();
    }
    QVERIFY(ids.contains(ScrobbleDestinationConfig::lastFmId()));
    QVERIFY(ids.contains(ScrobbleDestinationConfig::listenBrainzId()));
    QVERIFY(ids.contains(m_koitoId));
}

void ListeningHistoryDialogTest::aggregateSummarizesAndDisablesMutations()
{
    const auto dialog = makeDialog();
    auto *selector = dialog->findChild<QComboBox *>();
    selector->setCurrentIndex(0);

    // Three obligations exist (Last.fm twice, Koito twice) and one has been
    // delivered, so the overview reads 1 of 4.
    QVERIFY(summaryText(dialog.get()).contains(QStringLiteral("1/4 sent")));

    // Aggregate mode is read-only: there is no single backlog to act on.
    QVERIFY(!button(dialog.get(), QStringLiteral("Clear backlog"))->isEnabled());
    QVERIFY(!button(dialog.get(), QStringLiteral("Retry pending"))->isEnabled());
    QVERIFY(!button(dialog.get(), QStringLiteral("Scrobble selected"))->isEnabled());
}

void ListeningHistoryDialogTest::aConcreteSelectionScopesCountsAndActions()
{
    const auto dialog = makeDialog();
    auto *selector = dialog->findChild<QComboBox *>();

    selector->setCurrentIndex(selector->findData(m_koitoId));
    QCOMPARE(m_store->pendingCount(m_koitoId), 1);
    const QString koito = summaryText(dialog.get());
    QVERIFY(koito.contains(QStringLiteral("Koito")));
    QVERIFY(koito.contains(QStringLiteral("1 pending")));
    QVERIFY(koito.contains(QStringLiteral("1 sent")));
    QVERIFY(button(dialog.get(), QStringLiteral("Clear backlog"))->isEnabled());
    QVERIFY(button(dialog.get(), QStringLiteral("Retry pending"))->isEnabled());

    // Official ListenBrainz was never owed these listens, so it has nothing to
    // clear or retry even though the history is full.
    selector->setCurrentIndex(selector->findData(ScrobbleDestinationConfig::listenBrainzId()));
    QVERIFY(summaryText(dialog.get()).contains(QStringLiteral("0 pending")));
    QVERIFY(!button(dialog.get(), QStringLiteral("Clear backlog"))->isEnabled());
    QVERIFY(!button(dialog.get(), QStringLiteral("Retry pending"))->isEnabled());
}

QTEST_MAIN(ListeningHistoryDialogTest)
#include "test_listening_history_dialog.moc"
