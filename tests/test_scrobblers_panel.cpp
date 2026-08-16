#include "scrobble/ListenBrainzUrl.h"
#include "scrobble/ScrobbleDestination.h"
#include "ui/ScrobblersPanel.h"
#include "ui/ToggleSwitch.h"

#include <QAbstractButton>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTest>

// The panel saves as it is edited and applies nothing, so these drive the
// controls a user drives and assert on what reached the callbacks.
class ScrobblersPanelTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void aTokenIsWrittenAsSoonAsItIsTyped();
    void anAddedServerIsWithheldUntilItHasAnAddress();
    void aServerWithoutAnAddressCannotBeEnabled();
    void aTypedUrlIsNormalizedBeforeItIsSaved();
    void enablingADestinationSavesImmediately();
    void aConfiguredDestinationKeepsItsAddress();
    void aTokenIsHeldUntilThereIsSomewhereToSendIt();
    void aRejectedAddressIsNotTested();
    void externallyChangedStateIsAdopted();
    void anAddedServerIsConfiguredOnceItIsSaved();
    void aTestResultForChangedCredentialsIsIgnored();
    void anEarlierTestReplyCannotClaimALaterTest();

private:
    QHash<QString, QString> m_tokens;
    ScrobbleDestinationSet m_saved;
    int m_saveCount = 0;
    ScrobblersPanel::Callbacks m_callbacks;

    std::unique_ptr<ScrobblersPanel> makePanel(const ScrobbleDestinationSet &destinations)
    {
        auto panel = std::make_unique<ScrobblersPanel>(destinations, nullptr, m_callbacks);
        panel->show();
        QCoreApplication::processEvents();
        return panel;
    }

    // Every row's address field, named for the destination it belongs to.
    static QStringList urlFieldNames(QWidget *panel)
    {
        QStringList names;
        for (const QLineEdit *field : panel->findChildren<QLineEdit *>()) {
            if (field->objectName().endsWith(QStringLiteral(".url"))) {
                names << field->objectName();
            }
        }
        return names;
    }

    // Adds a server and gives it an address, returning its minted id.
    QString addServer(QWidget *panel, const QString &url)
    {
        const QStringList existing = urlFieldNames(panel);
        for (QPushButton *candidate : panel->findChildren<QPushButton *>()) {
            if (candidate->text() == QStringLiteral("Add server…")) {
                candidate->click();
            }
        }
        QCoreApplication::processEvents();

        QStringList added = urlFieldNames(panel);
        for (const QString &was : existing) {
            added.removeOne(was);
        }
        if (added.size() != 1) {
            return {};
        }
        const QString id = added.first().chopped(QStringLiteral(".url").size());
        if (!url.isEmpty()) {
            type(panel->findChild<QLineEdit *>(added.first()), url);
        }
        return id;
    }

    // Types into a field and ends the edit, which is what commits it.
    static void type(QLineEdit *field, const QString &text)
    {
        QVERIFY(field != nullptr);
        field->setText(text);
        emit field->editingFinished();
    }
};

void ScrobblersPanelTest::init()
{
    m_tokens.clear();
    m_saved = {};
    m_saveCount = 0;

    m_callbacks = {};
    m_callbacks.readToken = [this](const QString &id) { return m_tokens.value(id); };
    m_callbacks.writeToken = [this](const QString &id, const QString &token) { m_tokens.insert(id, token); };
    m_callbacks.removeToken = [this](const QString &id) { m_tokens.remove(id); };
    m_callbacks.saveDestinations = [this](const ScrobbleDestinationSet &destinations) {
        m_saved = destinations;
        ++m_saveCount;
    };
    m_callbacks.lastFmConfigured = []() { return false; };
    m_callbacks.testDestination = [](const QString &, quint64, const QString &, const QString &) {};
    m_callbacks.readOffline = []() { return false; };
    m_callbacks.writeOffline = [](bool) {};
    m_callbacks.openLastFmSettings = []() {};
}

void ScrobblersPanelTest::aTokenIsWrittenAsSoonAsItIsTyped()
{
    const QString id = ScrobbleDestinationConfig::listenBrainzId();
    m_tokens.insert(id, QStringLiteral("old-token"));

    const auto panel = makePanel(ScrobbleDestinationConfig::defaults());
    type(panel->findChild<QLineEdit *>(id + QStringLiteral(".token")), QStringLiteral("replacement-token"));

    // Nothing was accepted, and the token is already stored.
    QCOMPARE(m_tokens.value(id), QStringLiteral("replacement-token"));

    // The built-in destination keeps its identity and address whatever is typed.
    const ScrobbleDestination *official = panel->destinations().find(id);
    QVERIFY(official != nullptr);
    QCOMPARE(official->type, ScrobbleDestination::Type::ListenBrainzCompatible);
    QCOMPARE(official->apiRoot, ListenBrainzUrl::officialApiRoot());
}

void ScrobblersPanelTest::anAddedServerIsWithheldUntilItHasAnAddress()
{
    const auto panel = makePanel(ScrobbleDestinationConfig::defaults());
    const int before = panel->destinations().items.size();

    QPushButton *add = nullptr;
    for (QPushButton *candidate : panel->findChildren<QPushButton *>()) {
        if (candidate->text() == QStringLiteral("Add server…")) {
            add = candidate;
        }
    }
    QVERIFY(add != nullptr);

    const QStringList before_fields = urlFieldNames(panel.get());
    add->click();
    QCoreApplication::processEvents();

    // The row exists in the panel, but an entry with nowhere to deliver is not
    // a destination yet, so it is not handed out or saved.
    QCOMPARE(panel->destinations().items.size(), before);

    QStringList added = urlFieldNames(panel.get());
    for (const QString &existing : before_fields) {
        added.removeOne(existing);
    }
    QCOMPARE(added.size(), 1);
    type(panel->findChild<QLineEdit *>(added.first()), QStringLiteral("https://koito.example"));

    QCOMPARE(panel->destinations().items.size(), before + 1);
    QCOMPARE(m_saved.items.size(), before + 1);
}

void ScrobblersPanelTest::aServerWithoutAnAddressCannotBeEnabled()
{
    const auto panel = makePanel(ScrobbleDestinationConfig::defaults());
    const QStringList existing = urlFieldNames(panel.get());

    QPushButton *add = nullptr;
    for (QPushButton *candidate : panel->findChildren<QPushButton *>()) {
        if (candidate->text() == QStringLiteral("Add server…")) {
            add = candidate;
        }
    }
    add->click();
    QCoreApplication::processEvents();

    QStringList added = urlFieldNames(panel.get());
    for (const QString &was : existing) {
        added.removeOne(was);
    }
    QCOMPARE(added.size(), 1);
    const QString id = added.first().chopped(QStringLiteral(".url").size());

    // Nowhere to deliver is not a state a destination can be enabled in, so the
    // switch refuses rather than promising delivery that cannot happen.
    auto *toggle = panel->findChild<ToggleSwitch *>(id + QStringLiteral(".enabled"));
    QVERIFY(toggle != nullptr);
    QVERIFY(!toggle->isChecked());
    QVERIFY(!toggle->isEnabled());

    auto *status = panel->findChild<QLabel *>(id + QStringLiteral(".status"));
    QVERIFY(status != nullptr);
    QCOMPARE(status->text(), QStringLiteral("Server URL required"));
    QVERIFY(panel->destinations().find(id) == nullptr);
}

void ScrobblersPanelTest::aTypedUrlIsNormalizedBeforeItIsSaved()
{
    ScrobbleDestinationSet destinations = ScrobbleDestinationConfig::defaults();
    const QString id = destinations.addCustom(QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), false);

    const auto panel = makePanel(destinations);
    type(panel->findChild<QLineEdit *>(id + QStringLiteral(".url")), QStringLiteral("https://moved.example/"));

    const ScrobbleDestination *saved = m_saved.find(id);
    QVERIFY(saved != nullptr);
    QCOMPARE(saved->apiRoot, ListenBrainzUrl::normalizeBase(QStringLiteral("https://moved.example/")).apiRoot);

    // A rejected address is reported and never saved over the working one.
    const int saves = m_saveCount;
    type(panel->findChild<QLineEdit *>(id + QStringLiteral(".url")), QStringLiteral("ftp://nope.example"));
    QCOMPARE(m_saveCount, saves);
    QCOMPARE(m_saved.find(id)->apiRoot, saved->apiRoot);
}

void ScrobblersPanelTest::enablingADestinationSavesImmediately()
{
    const QString id = ScrobbleDestinationConfig::listenBrainzId();
    const auto panel = makePanel(ScrobbleDestinationConfig::defaults());
    auto *toggle = panel->findChild<ToggleSwitch *>(id + QStringLiteral(".enabled"));
    QVERIFY(toggle != nullptr);
    QVERIFY(!toggle->isChecked());

    toggle->click();

    QVERIFY(m_saveCount > 0);
    const ScrobbleDestination *saved = m_saved.find(id);
    QVERIFY(saved != nullptr);
    QVERIFY(saved->enabled);
}

// Clearing the address of a destination that is already configured would drop
// it from the configuration and strand its delivery records, so the field
// refuses instead and says what to do about it.
void ScrobblersPanelTest::aConfiguredDestinationKeepsItsAddress()
{
    ScrobbleDestinationSet destinations = ScrobbleDestinationConfig::defaults();
    const QString id = destinations.addCustom(QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), true);

    const auto panel = makePanel(destinations);
    auto *url = panel->findChild<QLineEdit *>(id + QStringLiteral(".url"));
    type(url, QString());

    QCOMPARE(url->text(), QStringLiteral("https://koito.example/1"));
    const ScrobbleDestination *kept = panel->destinations().find(id);
    QVERIFY(kept != nullptr);
    QCOMPARE(kept->apiRoot, QStringLiteral("https://koito.example/1"));
}

void ScrobblersPanelTest::aTokenIsHeldUntilThereIsSomewhereToSendIt()
{
    const auto panel = makePanel(ScrobbleDestinationConfig::defaults());
    const QStringList existing = urlFieldNames(panel.get());

    QPushButton *add = nullptr;
    for (QPushButton *candidate : panel->findChildren<QPushButton *>()) {
        if (candidate->text() == QStringLiteral("Add server…")) {
            add = candidate;
        }
    }
    add->click();
    QCoreApplication::processEvents();

    QStringList added = urlFieldNames(panel.get());
    for (const QString &was : existing) {
        added.removeOne(was);
    }
    QCOMPARE(added.size(), 1);
    const QString id = added.first().chopped(QStringLiteral(".url").size());

    // A token typed before the row has an address would otherwise be stored
    // against an id no configuration mentions.
    type(panel->findChild<QLineEdit *>(id + QStringLiteral(".token")), QStringLiteral("early-token"));
    QVERIFY(!m_tokens.contains(id));

    // Once there is somewhere to send, the token that was held goes with it.
    type(panel->findChild<QLineEdit *>(id + QStringLiteral(".url")), QStringLiteral("https://koito.example"));
    QCOMPARE(m_tokens.value(id), QStringLiteral("early-token"));
}

void ScrobblersPanelTest::aRejectedAddressIsNotTested()
{
    ScrobbleDestinationSet destinations = ScrobbleDestinationConfig::defaults();
    const QString id = destinations.addCustom(QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), false);

    QStringList tested;
    m_callbacks.testDestination = [&tested](const QString &, quint64, const QString &apiRoot, const QString &) {
        tested << apiRoot;
    };

    const auto panel = makePanel(destinations);
    auto *url = panel->findChild<QLineEdit *>(id + QStringLiteral(".url"));
    url->setText(QStringLiteral("ftp://nope.example"));

    // Testing an address the field has already refused would report on the old
    // one, which is not what is on screen.
    panel->findChild<QPushButton *>(id + QStringLiteral(".test"))->click();
    QVERIFY(tested.isEmpty());
}

void ScrobblersPanelTest::externallyChangedStateIsAdopted()
{
    const QString id = ScrobbleDestinationConfig::lastFmId();
    const auto panel = makePanel(ScrobbleDestinationConfig::defaults());
    QVERIFY(!panel->destinations().find(id)->enabled);

    // Last.fm enables itself when it authenticates, which happens outside this
    // panel while the window is open. The next save must not write that back.
    panel->adoptEnabledState(id, true);
    QVERIFY(panel->destinations().find(id)->enabled);

    panel->findChild<ToggleSwitch *>(ScrobbleDestinationConfig::listenBrainzId() + QStringLiteral(".enabled"))
        ->click();
    QVERIFY(m_saved.find(id) != nullptr);
    QVERIFY(m_saved.find(id)->enabled);
}

// A row added here stops being provisional the moment it is saved with an
// address: its delivery records exist from then on, so it gets the same
// protection as one that was already configured.
void ScrobblersPanelTest::anAddedServerIsConfiguredOnceItIsSaved()
{
    const auto panel = makePanel(ScrobbleDestinationConfig::defaults());
    const QString id = addServer(panel.get(), QStringLiteral("https://koito.example"));
    QVERIFY(panel->destinations().find(id) != nullptr);

    auto *url = panel->findChild<QLineEdit *>(id + QStringLiteral(".url"));
    type(url, QString());

    QCOMPARE(url->text(), QStringLiteral("https://koito.example/1"));
    QVERIFY(panel->destinations().find(id) != nullptr);
}

void ScrobblersPanelTest::aTestResultForChangedCredentialsIsIgnored()
{
    ScrobbleDestinationSet destinations = ScrobbleDestinationConfig::defaults();
    const QString id = destinations.addCustom(QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), false);

    QList<quint64> issued;
    m_callbacks.testDestination = [&issued](const QString &, quint64 requestId, const QString &, const QString &) {
        issued << requestId;
    };

    const auto panel = makePanel(destinations);
    type(panel->findChild<QLineEdit *>(id + QStringLiteral(".token")), QStringLiteral("first"));
    panel->findChild<QPushButton *>(id + QStringLiteral(".test"))->click();
    QCOMPARE(issued.size(), 1);

    auto *status = panel->findChild<QLabel *>(id + QStringLiteral(".status"));
    QCOMPARE(status->text(), QStringLiteral("Testing…"));

    // The token changes while the request is in flight; the reply describes the
    // token that was sent, not the one now on screen.
    type(panel->findChild<QLineEdit *>(id + QStringLiteral(".token")), QStringLiteral("second"));
    panel->reportTestResult(id, issued.last(), true, QStringLiteral("somebody"));
    QVERIFY(status->text() != QStringLiteral("Connected as somebody"));

    // A reply to a test of what is actually on screen is still this row's.
    panel->findChild<QPushButton *>(id + QStringLiteral(".test"))->click();
    QCOMPARE(issued.size(), 2);
    panel->reportTestResult(id, issued.last(), true, QStringLiteral("lobo"));
    QCOMPARE(status->text(), QStringLiteral("Connected as lobo"));
}

// Two tests of the same credentials can still answer differently, because one
// of them can meet a transient failure. The later request is the one the row
// is waiting on, so the earlier answer cannot claim it.
void ScrobblersPanelTest::anEarlierTestReplyCannotClaimALaterTest()
{
    ScrobbleDestinationSet destinations = ScrobbleDestinationConfig::defaults();
    const QString id = destinations.addCustom(QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), false);

    QList<quint64> issued;
    m_callbacks.testDestination = [&issued](const QString &, quint64 requestId, const QString &, const QString &) {
        issued << requestId;
    };

    const auto panel = makePanel(destinations);
    type(panel->findChild<QLineEdit *>(id + QStringLiteral(".token")), QStringLiteral("token"));
    auto *test = panel->findChild<QPushButton *>(id + QStringLiteral(".test"));
    test->click();
    test->click();
    QCOMPARE(issued.size(), 2);
    QVERIFY(issued.at(0) != issued.at(1));

    auto *status = panel->findChild<QLabel *>(id + QStringLiteral(".status"));
    // The first request comes back rejected, having met a transient failure.
    panel->reportTestResult(id, issued.at(0), false, QString());
    QCOMPARE(status->text(), QStringLiteral("Testing…"));

    // The second is the one the row asked last, and it is what it reports.
    panel->reportTestResult(id, issued.at(1), true, QStringLiteral("lobo"));
    QCOMPARE(status->text(), QStringLiteral("Connected as lobo"));
}

QTEST_MAIN(ScrobblersPanelTest)
#include "test_scrobblers_panel.moc"
