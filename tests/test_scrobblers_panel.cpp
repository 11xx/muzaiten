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

// The dialog saves as it is edited and applies nothing, so these drive the
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

private:
    QHash<QString, QString> m_tokens;
    ScrobbleDestinationSet m_saved;
    int m_saveCount = 0;
    ScrobblersPanel::Callbacks m_callbacks;

    std::unique_ptr<ScrobblersPanel> makeDialog(const ScrobbleDestinationSet &destinations)
    {
        auto dialog = std::make_unique<ScrobblersPanel>(destinations, nullptr, m_callbacks);
        dialog->show();
        QCoreApplication::processEvents();
        return dialog;
    }

    // Every row's address field, named for the destination it belongs to.
    static QStringList urlFieldNames(QWidget *dialog)
    {
        QStringList names;
        for (const QLineEdit *field : dialog->findChildren<QLineEdit *>()) {
            if (field->objectName().endsWith(QStringLiteral(".url"))) {
                names << field->objectName();
            }
        }
        return names;
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
    m_callbacks.testDestination = [](const QString &, const QString &, const QString &) {};
    m_callbacks.readOffline = []() { return false; };
    m_callbacks.writeOffline = [](bool) {};
    m_callbacks.openLastFmSettings = []() {};
}

void ScrobblersPanelTest::aTokenIsWrittenAsSoonAsItIsTyped()
{
    const QString id = ScrobbleDestinationConfig::listenBrainzId();
    m_tokens.insert(id, QStringLiteral("old-token"));

    const auto dialog = makeDialog(ScrobbleDestinationConfig::defaults());
    type(dialog->findChild<QLineEdit *>(id + QStringLiteral(".token")), QStringLiteral("replacement-token"));

    // Nothing was accepted, and the token is already stored.
    QCOMPARE(m_tokens.value(id), QStringLiteral("replacement-token"));

    // The built-in destination keeps its identity and address whatever is typed.
    const ScrobbleDestination *official = dialog->destinations().find(id);
    QVERIFY(official != nullptr);
    QCOMPARE(official->type, ScrobbleDestination::Type::ListenBrainzCompatible);
    QCOMPARE(official->apiRoot, ListenBrainzUrl::officialApiRoot());
}

void ScrobblersPanelTest::anAddedServerIsWithheldUntilItHasAnAddress()
{
    const auto dialog = makeDialog(ScrobbleDestinationConfig::defaults());
    const int before = dialog->destinations().items.size();

    QPushButton *add = nullptr;
    for (QPushButton *candidate : dialog->findChildren<QPushButton *>()) {
        if (candidate->text() == QStringLiteral("Add server…")) {
            add = candidate;
        }
    }
    QVERIFY(add != nullptr);

    const QStringList before_fields = urlFieldNames(dialog.get());
    add->click();
    QCoreApplication::processEvents();

    // The row exists in the dialog, but an entry with nowhere to deliver is not
    // a destination yet, so it is not handed out or saved.
    QCOMPARE(dialog->destinations().items.size(), before);

    QStringList added = urlFieldNames(dialog.get());
    for (const QString &existing : before_fields) {
        added.removeOne(existing);
    }
    QCOMPARE(added.size(), 1);
    type(dialog->findChild<QLineEdit *>(added.first()), QStringLiteral("https://koito.example"));

    QCOMPARE(dialog->destinations().items.size(), before + 1);
    QCOMPARE(m_saved.items.size(), before + 1);
}

void ScrobblersPanelTest::aServerWithoutAnAddressCannotBeEnabled()
{
    ScrobbleDestinationSet destinations = ScrobbleDestinationConfig::defaults();
    const QString id = destinations.addCustom(QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), true);

    const auto dialog = makeDialog(destinations);
    auto *toggle = dialog->findChild<ToggleSwitch *>(id + QStringLiteral(".enabled"));
    QVERIFY(toggle != nullptr);
    QVERIFY(toggle->isChecked());

    // Clearing the address strips the destination of the only thing that made
    // delivery possible, so it turns itself off and refuses to be turned back on.
    type(dialog->findChild<QLineEdit *>(id + QStringLiteral(".url")), QString());
    QVERIFY(!toggle->isChecked());
    QVERIFY(!toggle->isEnabled());

    auto *status = dialog->findChild<QLabel *>(id + QStringLiteral(".status"));
    QVERIFY(status != nullptr);
    QCOMPARE(status->text(), QStringLiteral("Server URL required"));
    QVERIFY(dialog->destinations().find(id) == nullptr);
}

void ScrobblersPanelTest::aTypedUrlIsNormalizedBeforeItIsSaved()
{
    ScrobbleDestinationSet destinations = ScrobbleDestinationConfig::defaults();
    const QString id = destinations.addCustom(QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), false);

    const auto dialog = makeDialog(destinations);
    type(dialog->findChild<QLineEdit *>(id + QStringLiteral(".url")), QStringLiteral("https://moved.example/"));

    const ScrobbleDestination *saved = m_saved.find(id);
    QVERIFY(saved != nullptr);
    QCOMPARE(saved->apiRoot, ListenBrainzUrl::normalizeBase(QStringLiteral("https://moved.example/")).apiRoot);

    // A rejected address is reported and never saved over the working one.
    const int saves = m_saveCount;
    type(dialog->findChild<QLineEdit *>(id + QStringLiteral(".url")), QStringLiteral("ftp://nope.example"));
    QCOMPARE(m_saveCount, saves);
    QCOMPARE(m_saved.find(id)->apiRoot, saved->apiRoot);
}

void ScrobblersPanelTest::enablingADestinationSavesImmediately()
{
    const QString id = ScrobbleDestinationConfig::listenBrainzId();
    const auto dialog = makeDialog(ScrobbleDestinationConfig::defaults());
    auto *toggle = dialog->findChild<ToggleSwitch *>(id + QStringLiteral(".enabled"));
    QVERIFY(toggle != nullptr);
    QVERIFY(!toggle->isChecked());

    toggle->click();

    QVERIFY(m_saveCount > 0);
    const ScrobbleDestination *saved = m_saved.find(id);
    QVERIFY(saved != nullptr);
    QVERIFY(saved->enabled);
}

QTEST_MAIN(ScrobblersPanelTest)
#include "test_scrobblers_panel.moc"
