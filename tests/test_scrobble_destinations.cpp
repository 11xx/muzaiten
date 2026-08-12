#include "scrobble/ListenBrainzUrl.h"
#include "scrobble/ScrobbleDestination.h"
#include "ui/ScrobbleUploadDispatcher.h"

#include <QHash>
#include <QTest>

using namespace ScrobbleDestinationConfig;

namespace {

// JSON fixtures read far better without escaped quotes on every key, and moc's
// preprocessor does not cope with raw string literals, so single quotes stand in
// for double quotes here and are swapped back before parsing.
QString json(const QString &singleQuoted)
{
    return QString(singleQuoted).replace(QLatin1Char('\''), QLatin1Char('"'));
}

}   // namespace

class ScrobbleDestinationsTest final : public QObject {
    Q_OBJECT

private slots:
    void defaultsHoldTheReservedDestinations();
    void roundTripsCustomDestinations();
    void roundTripsAwkwardNames();
    void roundTripsPathPrefixedUrls();
    void mintedIdsAreUuidStyleAndNeverReused();
    void restoresMissingReservedDestinations();
    void correctsTamperedReservedFields();
    void dropsUnusableCustomEntries();
    void malformedCustomIdsCannotAddressTokenSettings();
    void disablingReservedDestinationUpdatesCentralDocument();
    void customCompatibleUploadForwardsItsExactId();
    void degradesToDefaultsOnGarbage_data();
    void degradesToDefaultsOnGarbage();
    void tokenKeysArePerDestination();
};

void ScrobbleDestinationsTest::defaultsHoldTheReservedDestinations()
{
    const ScrobbleDestinationSet set = defaults();
    QCOMPARE(set.items.size(), 2);
    QCOMPARE(set.items.at(0).id, lastFmId());
    QCOMPARE(set.items.at(0).type, ScrobbleDestination::Type::LastFm);
    QCOMPARE(set.items.at(1).id, listenBrainzId());
    QCOMPARE(set.items.at(1).apiRoot, ListenBrainzUrl::officialApiRoot());

    for (const ScrobbleDestination &item : set.items) {
        QVERIFY(item.isReserved());
        QVERIFY(!item.enabled);
    }
}

void ScrobbleDestinationsTest::roundTripsCustomDestinations()
{
    ScrobbleDestinationSet set = defaults();
    const QString id = set.addCustom(QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), true);

    const QString document = toJson(set);
    QVERIFY(!document.contains(QStringLiteral("nextCustomSequence")));
    const ScrobbleDestinationSet reloaded = fromJson(document);
    QCOMPARE(reloaded.items.size(), 3);

    const ScrobbleDestination *koito = reloaded.find(id);
    QVERIFY(koito != nullptr);
    QCOMPARE(koito->name, QStringLiteral("Koito"));
    QCOMPARE(koito->apiRoot, QStringLiteral("https://koito.example/1"));
    QCOMPARE(koito->type, ScrobbleDestination::Type::ListenBrainzCompatible);
    QVERIFY(koito->enabled);
    QVERIFY(!koito->isReserved());
}

void ScrobbleDestinationsTest::roundTripsAwkwardNames()
{
    // Names are free text and land in a JSON document; quoting, backslashes,
    // newlines and non-Latin scripts must survive the trip verbatim.
    const QStringList names = {
        QStringLiteral("He said \"scrobble\""),
        QStringLiteral("back\\slash"),
        QStringLiteral("two\nlines"),
        QStringLiteral("音楽 サーバー"),
        QStringLiteral("emoji \U0001F3B5"),
        QStringLiteral("   padded   "),
    };

    ScrobbleDestinationSet set = defaults();
    QStringList ids;
    for (const QString &name : names) {
        ids << set.addCustom(name, QStringLiteral("https://example.test/1"), false);
    }

    const ScrobbleDestinationSet reloaded = fromJson(toJson(set));
    for (int index = 0; index < names.size(); ++index) {
        const ScrobbleDestination *item = reloaded.find(ids.at(index));
        QVERIFY(item != nullptr);
        QCOMPARE(item->name, names.at(index));
    }
}

void ScrobbleDestinationsTest::roundTripsPathPrefixedUrls()
{
    ScrobbleDestinationSet set = defaults();
    const QString prefixed = ListenBrainzUrl::normalizeBase(QStringLiteral("https://example.test/music")).apiRoot;
    const QString ported = ListenBrainzUrl::normalizeBase(QStringLiteral("http://127.0.0.1:4110/koito")).apiRoot;
    const QString a = set.addCustom(QStringLiteral("Prefixed"), prefixed, true);
    const QString b = set.addCustom(QStringLiteral("Ported"), ported, true);

    const ScrobbleDestinationSet reloaded = fromJson(toJson(set));
    QCOMPARE(reloaded.find(a)->apiRoot, QStringLiteral("https://example.test/music/1"));
    QCOMPARE(reloaded.find(b)->apiRoot, QStringLiteral("http://127.0.0.1:4110/koito/1"));
}

void ScrobbleDestinationsTest::mintedIdsAreUuidStyleAndNeverReused()
{
    ScrobbleDestinationSet set = defaults();
    const QString first = set.addCustom(QStringLiteral("A"), QStringLiteral("https://a.test/1"), true);
    const QString second = set.addCustom(QStringLiteral("B"), QStringLiteral("https://b.test/1"), true);
    QVERIFY(first != second);
    QVERIFY(isCustomId(first));
    QVERIFY(isCustomId(second));

    // Removing a destination must not hand its id to the next one: delivery rows
    // and tokens are keyed by id, so reuse would resurrect a dead backlog.
    set.items.removeIf([&](const ScrobbleDestination &item) { return item.id == second; });
    ScrobbleDestinationSet reloaded = fromJson(toJson(set));
    const QString third = reloaded.addCustom(QStringLiteral("C"), QStringLiteral("https://c.test/1"), true);
    QVERIFY(third != first);
    QVERIFY(third != second);

    // And the guarantee survives removing every custom entry.
    reloaded.items.removeIf([](const ScrobbleDestination &item) { return !item.isReserved(); });
    const QString fourth = fromJson(toJson(reloaded))
                               .addCustom(QStringLiteral("D"), QStringLiteral("https://d.test/1"), true);
    QVERIFY(fourth != first);
    QVERIFY(fourth != second);
    QVERIFY(fourth != third);
    QVERIFY(isCustomId(fourth));
}

void ScrobbleDestinationsTest::restoresMissingReservedDestinations()
{
    const QString document = json(QStringLiteral(
        "{'version':1,'destinations':["
        "{'id':'4f32f046-df07-4be8-9d8b-6d7a8e6f1b2c','type':'listenbrainz','name':'Koito',"
        "'apiRoot':'https://koito.example/1','enabled':true}]}"));

    const ScrobbleDestinationSet set = fromJson(document);
    QCOMPARE(set.items.size(), 3);
    QVERIFY(set.find(lastFmId()) != nullptr);
    QVERIFY(set.find(listenBrainzId()) != nullptr);
    QVERIFY(set.find(QStringLiteral("4f32f046-df07-4be8-9d8b-6d7a8e6f1b2c")) != nullptr);
    QCOMPARE(set.find(listenBrainzId())->apiRoot, ListenBrainzUrl::officialApiRoot());
}

void ScrobbleDestinationsTest::correctsTamperedReservedFields()
{
    // The reserved destinations' type and URL are ours to define. A document
    // claiming otherwise must not be able to redirect official ListenBrainz
    // traffic or turn Last.fm into a compatible endpoint.
    const QString document = json(QStringLiteral(
        "{'version':1,'destinations':["
        "{'id':'listenbrainz','type':'listenbrainz','name':'LB',"
        "'apiRoot':'https://evil.test/1','enabled':true},"
        "{'id':'lastfm','type':'listenbrainz','name':'Last.fm',"
        "'apiRoot':'https://evil.test/1','enabled':true}]}"));

    const ScrobbleDestinationSet set = fromJson(document);
    QCOMPARE(set.find(listenBrainzId())->apiRoot, ListenBrainzUrl::officialApiRoot());
    QCOMPARE(set.find(lastFmId())->type, ScrobbleDestination::Type::LastFm);
    QVERIFY(set.find(lastFmId())->apiRoot.isEmpty());
    // Enabled state, unlike identity, is genuinely the user's.
    QVERIFY(set.find(listenBrainzId())->enabled);
}

void ScrobbleDestinationsTest::dropsUnusableCustomEntries()
{
    const QString document = json(QStringLiteral(
        "{'version':1,'destinations':["
        "{'id':'','type':'listenbrainz','name':'No id','apiRoot':'https://a.test/1'},"
        "{'id':'custom-1','type':'listenbrainz','name':'No url','apiRoot':''},"
        "{'id':'5e97b3c7-5a40-4d1f-bfdc-d9a5cf8e205f','type':'listenbrainz','name':'Fine','apiRoot':'https://b.test/1'},"
        "{'id':'5e97b3c7-5a40-4d1f-bfdc-d9a5cf8e205f','type':'listenbrainz','name':'Duplicate','apiRoot':'https://c.test/1'}]}"));

    const ScrobbleDestinationSet set = fromJson(document);
    QCOMPARE(set.items.size(), 3);   // two reserved plus the one usable custom
    QVERIFY(set.find(QStringLiteral("custom-1")) == nullptr);
    QCOMPARE(set.find(QStringLiteral("5e97b3c7-5a40-4d1f-bfdc-d9a5cf8e205f"))->name, QStringLiteral("Fine"));
}

void ScrobbleDestinationsTest::malformedCustomIdsCannotAddressTokenSettings()
{
    const QString document = json(QStringLiteral(
        "{'version':1,'destinations':["
        "{'id':'custom-9','type':'listenbrainz','name':'Koito','apiRoot':'https://koito.example/1'},"
        "{'id':'A4E26AE7-C361-4797-B457-46A31436934E','type':'listenbrainz','name':'Uppercase','apiRoot':'https://upper.example/1'}]}"));

    const ScrobbleDestinationSet set = fromJson(document);
    QCOMPARE(set.items.size(), 2);
    QVERIFY(set.find(QStringLiteral("custom-9")) == nullptr);
    QVERIFY(set.find(QStringLiteral("A4E26AE7-C361-4797-B457-46A31436934E")) == nullptr);
    QVERIFY(tokenSettingKey(QStringLiteral("custom-9")).isEmpty());
    QVERIFY(tokenSettingKey(QStringLiteral("A4E26AE7-C361-4797-B457-46A31436934E")).isEmpty());
}

void ScrobbleDestinationsTest::disablingReservedDestinationUpdatesCentralDocument()
{
    ScrobbleDestinationSet set = defaults();
    QVERIFY(set.setEnabled(lastFmId(), true));

    QHash<QString, QString> settings;
    save([&settings](const QString &key, const QString &value) { settings.insert(key, value); }, set);

    QCOMPARE(settings.value(QStringLiteral("lastfm.enabled")), QStringLiteral("true"));
    ScrobbleDestinationSet reloaded =
        load([&settings](const QString &key) { return settings.value(key); });
    QVERIFY(reloaded.find(lastFmId())->enabled);

    QVERIFY(reloaded.setEnabled(lastFmId(), false));
    save([&settings](const QString &key, const QString &value) { settings.insert(key, value); }, reloaded);

    QCOMPARE(settings.value(QStringLiteral("lastfm.enabled")), QStringLiteral("false"));
    reloaded = load([&settings](const QString &key) { return settings.value(key); });
    QVERIFY(!reloaded.find(lastFmId())->enabled);
}

void ScrobbleDestinationsTest::customCompatibleUploadForwardsItsExactId()
{
    ScrobbleDestinationSet set = defaults();
    const QString customId = set.addCustom(QStringLiteral("Koito"), QStringLiteral("https://koito.example/1"), true);
    QString forwarded;
    bool lastFmCalled = false;

    ScrobbleUploadDispatcher::dispatch(
        set, customId, [&lastFmCalled]() { lastFmCalled = true; },
        [&forwarded](const QString &destinationId) { forwarded = destinationId; });

    QCOMPARE(forwarded, customId);
    QVERIFY(!lastFmCalled);
}

void ScrobbleDestinationsTest::degradesToDefaultsOnGarbage_data()
{
    QTest::addColumn<QString>("document");

    QTest::newRow("empty") << QString();
    QTest::newRow("not json") << QStringLiteral("this is not json");
    QTest::newRow("json array") << QStringLiteral("[1,2,3]");
    QTest::newRow("no destinations key") << json(QStringLiteral("{'version':1}"));
    QTest::newRow("empty destinations") << json(QStringLiteral("{'version':1,'destinations':[]}"));
}

void ScrobbleDestinationsTest::degradesToDefaultsOnGarbage()
{
    QFETCH(QString, document);

    const ScrobbleDestinationSet set = fromJson(document);
    QCOMPARE(set.items.size(), 2);
    QVERIFY(set.find(lastFmId()) != nullptr);
    QVERIFY(set.find(listenBrainzId()) != nullptr);
}

void ScrobbleDestinationsTest::tokenKeysArePerDestination()
{
    // Official ListenBrainz keeps its long-standing key, so upgrading does not
    // silently lose a configured token.
    QCOMPARE(tokenSettingKey(listenBrainzId()), QStringLiteral("listenbrainz.token"));
    const QString first = QStringLiteral("7ba96314-64ba-4bf7-8fb4-e3b2a1c21f9a");
    const QString second = QStringLiteral("2e16ad27-e5d4-4c76-b143-53e9cb1b678a");
    QCOMPARE(tokenSettingKey(first), QStringLiteral("scrobble.destination.%1.token").arg(first));
    QVERIFY(tokenSettingKey(first) != tokenSettingKey(second));
}

QTEST_MAIN(ScrobbleDestinationsTest)
#include "test_scrobble_destinations.moc"
